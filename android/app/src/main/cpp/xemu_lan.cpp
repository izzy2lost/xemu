// System Link (LAN) relay. See xemu_lan.h for the overall design.
#include "xemu_lan.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace xemu_lan {
namespace {

constexpr const char* kLogTag = "xemu-lan";

// 4-byte control packets: 'X' '1' 'L' <type>.
constexpr size_t kLanCtlSize = 4;
constexpr char kCtlHello = 'H';  // join -> host, every kHelloIntervalMs
constexpr char kCtlAck = 'A';    // host -> join, answer to a hello
constexpr char kCtlBye = 'B';    // join -> host, on shutdown
constexpr char kCtlDupMac = 'D'; // host -> join, your MAC is already in use

constexpr char kDiscoverRequest[] = "X1LAN-DISCOVER";
constexpr char kDiscoverReplyPrefix[] = "X1LAN-HOST\n";

constexpr size_t kEthHeaderSize = 14;
constexpr size_t kMaxDatagram = 65536;
constexpr size_t kMaxPeers = 16;
constexpr size_t kMaxMacs = 256;
constexpr int64_t kHelloIntervalMs = 2000;
// Peers that speak the control protocol send a hello every 2 s; drop them
// after this much silence. Peers that never sent a hello (desktop xemu) only
// go away when the table is full.
constexpr int64_t kPeerTimeoutMs = 15000;
constexpr int64_t kMacTimeoutMs = 120000;
// A joined console reports the host as lost after this long without an ack.
constexpr int64_t kHostTimeoutMs = 8000;
constexpr int kSocketBufferBytes = 512 * 1024;

constexpr int kLocalOwner = -1;  // MAC table owner id for this device's guest

void Log(int prio, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
#ifdef __ANDROID__
  __android_log_vprint(prio, kLogTag, fmt, ap);
#else
  (void)prio;
  fprintf(stderr, "[%s] ", kLogTag);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
#endif
  va_end(ap);
}

#ifdef __ANDROID__
#define LAN_INFO(...) Log(ANDROID_LOG_INFO, __VA_ARGS__)
#define LAN_WARN(...) Log(ANDROID_LOG_WARN, __VA_ARGS__)
#else
#define LAN_INFO(...) Log(0, __VA_ARGS__)
#define LAN_WARN(...) Log(0, __VA_ARGS__)
#endif

std::mutex g_events_mutex;
std::deque<std::pair<Event, int>> g_events;

void Post(Event event, int arg = 0) {
  std::lock_guard<std::mutex> lock(g_events_mutex);
  if (g_events.size() < 32) g_events.emplace_back(event, arg);
}

int64_t NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool SameAddr(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::string AddrStr(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

std::string MacStr(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

int OpenUdp(uint32_t ip, uint16_t port, uint16_t* bound_port) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  // No SO_REUSEADDR: on Linux it lets two UDP sockets share a port and
  // split its traffic, and a join must notice the port is taken.
  int buf = kSocketBufferBytes;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(ip);
  addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  if (bound_port) {
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    *bound_port = ntohs(addr.sin_port);
  }
  return fd;
}

bool Resolve(const std::string& spec, uint16_t default_port, sockaddr_in* out) {
  std::string host = spec;
  uint16_t port = default_port;
  size_t colon = spec.rfind(':');
  if (colon != std::string::npos) {
    host = spec.substr(0, colon);
    int p = atoi(spec.c_str() + colon + 1);
    if (p <= 0 || p > 65535) return false;
    port = uint16_t(p);
  }
  // Trim the whitespace an on-screen keyboard tends to leave behind.
  while (!host.empty() && isspace(uint8_t(host.back()))) host.pop_back();
  while (!host.empty() && isspace(uint8_t(host.front()))) host.erase(0, 1);
  if (host.empty()) return false;

  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
    return false;
  }
  *out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
  out->sin_port = htons(port);
  freeaddrinfo(res);
  return true;
}

struct Peer {
  int id;
  sockaddr_in addr;
  int64_t last_seen;
  bool speaks_control;
};

struct MacEntry {
  uint8_t mac[6];
  int owner;  // kLocalOwner or a Peer::id
  int64_t last_seen;
  bool dup_reported;
};

class Relay {
 public:
  Relay(const Config& cfg) : cfg_(cfg) {}

  ~Relay() {
    for (int fd : {qemu_fd_, lan_fd_, disc_fd_, wake_[0], wake_[1]}) {
      if (fd >= 0) close(fd);
    }
  }

  bool Init(std::string* qemu_bind, std::string* qemu_remote) {
    if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) < 0) {
      Post(Event::StartFailed, 0);
      return Fail("wake pipe");
    }

    // Pick a loopback port for QEMU to bind. QEMU sets SO_REUSEADDR and binds
    // it a moment later, so the probe socket only has to tell us a free port.
    uint16_t qemu_port = 0;
    int probe = OpenUdp(INADDR_LOOPBACK, 0, &qemu_port);
    if (probe < 0) {
      Post(Event::StartFailed, 0);
      return Fail("loopback probe socket");
    }
    close(probe);
    qemu_addr_ = {};
    qemu_addr_.sin_family = AF_INET;
    qemu_addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    qemu_addr_.sin_port = htons(qemu_port);

    uint16_t relay_port = 0;
    qemu_fd_ = OpenUdp(INADDR_LOOPBACK, 0, &relay_port);
    if (qemu_fd_ < 0) {
      Post(Event::StartFailed, 0);
      return Fail("loopback relay socket");
    }

    if (cfg_.mode == Mode::Host) {
      lan_fd_ = OpenUdp(INADDR_ANY, cfg_.port, nullptr);
      if (lan_fd_ < 0) {
        Post(Event::StartFailed, cfg_.port);
        return Fail("LAN port");
      }
      disc_fd_ = OpenUdp(INADDR_ANY, kDiscoveryPort, nullptr);
      if (disc_fd_ < 0) {
        // Discovery is a convenience; joining by address still works.
        LAN_WARN("discovery port %u unavailable (%s), hosts must be joined by "
                 "address", kDiscoveryPort, strerror(errno));
      }
      LAN_INFO("hosting System Link on UDP %u", cfg_.port);
      Post(Event::Hosting, cfg_.port);
    } else {
      if (!Resolve(cfg_.host_addr, cfg_.port, &host_addr_)) {
        LAN_WARN("cannot resolve host address \"%s\"", cfg_.host_addr.c_str());
        Post(Event::HostUnresolved);
        return false;
      }
      // Bind the well-known port when it is free so a desktop xemu host,
      // which only sends to its configured remote_addr, can reach us. Fall
      // back to any port (e.g. hosting and joining on one device for tests).
      lan_fd_ = OpenUdp(INADDR_ANY, cfg_.port, nullptr);
      if (lan_fd_ < 0) lan_fd_ = OpenUdp(INADDR_ANY, 0, nullptr);
      if (lan_fd_ < 0) {
        Post(Event::StartFailed, 0);
        return Fail("LAN socket");
      }
      LAN_INFO("joining System Link host %s", AddrStr(host_addr_).c_str());
    }

    *qemu_bind = "127.0.0.1:" + std::to_string(qemu_port);
    *qemu_remote = "127.0.0.1:" + std::to_string(relay_port);
    return true;
  }

  void Run() {
    std::vector<uint8_t> buf(kMaxDatagram);
    int64_t next_tick = 0;
    for (;;) {
      int64_t now = NowMs();
      if (now >= next_tick) {
        Tick(now);
        next_tick = now + kHelloIntervalMs;
      }

      pollfd fds[4];
      int n = 0;
      fds[n++] = {wake_[0], POLLIN, 0};
      fds[n++] = {qemu_fd_, POLLIN, 0};
      fds[n++] = {lan_fd_, POLLIN, 0};
      if (disc_fd_ >= 0) fds[n++] = {disc_fd_, POLLIN, 0};
      int timeout = int(next_tick - now);
      if (poll(fds, n, timeout < 0 ? 0 : timeout) < 0 && errno != EINTR) {
        LAN_WARN("poll failed: %s", strerror(errno));
        break;
      }
      if (fds[0].revents) break;
      if (fds[1].revents) DrainQemu(buf.data());
      if (fds[2].revents) DrainLan(buf.data());
      if (n > 3 && fds[3].revents) DrainDiscovery(buf.data());
    }
    if (cfg_.mode == Mode::Join) SendCtl(host_addr_, kCtlBye);
  }

  void Wake() {
    char c = 0;
    (void)!write(wake_[1], &c, 1);
  }

 private:
  bool Fail(const char* what) {
    LAN_WARN("cannot open %s: %s", what, strerror(errno));
    return false;
  }

  // --- Traffic from this device's guest ---------------------------------
  void DrainQemu(uint8_t* buf) {
    for (;;) {
      sockaddr_in from;
      socklen_t len = sizeof(from);
      ssize_t n = recvfrom(qemu_fd_, buf, kMaxDatagram, 0,
                           reinterpret_cast<sockaddr*>(&from), &len);
      if (n < 0) return;
      // The loopback interface is shared by every app on the device; only
      // accept frames from the port QEMU was told to bind.
      if (!SameAddr(from, qemu_addr_) || size_t(n) < kEthHeaderSize) continue;

      if (cfg_.mode == Mode::Join) {
        SendTo(host_addr_, buf, n);
        continue;
      }
      int64_t now = NowMs();
      Learn(buf + 6, kLocalOwner, now);
      Forward(buf, n, kLocalOwner, now);
    }
  }

  // --- Traffic from the LAN ---------------------------------------------
  void DrainLan(uint8_t* buf) {
    for (;;) {
      sockaddr_in from;
      socklen_t len = sizeof(from);
      ssize_t n = recvfrom(lan_fd_, buf, kMaxDatagram, 0,
                           reinterpret_cast<sockaddr*>(&from), &len);
      if (n < 0) return;
      if (cfg_.mode == Mode::Join) {
        HandleJoinDatagram(from, buf, n);
      } else {
        HandleHostDatagram(from, buf, n);
      }
    }
  }

  void HandleJoinDatagram(const sockaddr_in& from, const uint8_t* buf,
                          ssize_t n) {
    if (!SameAddr(from, host_addr_)) return;
    if (IsCtl(buf, n)) {
      if (buf[3] == kCtlAck) {
        last_ack_ = NowMs();
        if (!connected_) {
          connected_ = true;
          LAN_INFO("connected to System Link host %s", AddrStr(from).c_str());
          Post(Event::Connected);
        }
      } else if (buf[3] == kCtlDupMac && !dup_warned_) {
        dup_warned_ = true;
        Post(Event::DuplicateMac);
        LAN_WARN("the host reports another console with this console's MAC "
                 "address; System Link games will not see each other. Use a "
                 "different EEPROM (Settings > EEPROM > Randomize MAC).");
      }
      return;
    }
    if (size_t(n) >= kEthHeaderSize) SendToQemu(buf, n);
  }

  void HandleHostDatagram(const sockaddr_in& from, const uint8_t* buf,
                          ssize_t n) {
    int64_t now = NowMs();
    if (IsCtl(buf, n)) {
      if (buf[3] == kCtlBye) {
        RemovePeer(from);
        return;
      }
      if (buf[3] == kCtlHello) {
        Peer* p = TouchPeer(from, now);
        if (p) {
          p->speaks_control = true;
          SendCtl(from, kCtlAck);
        }
      }
      return;
    }
    if (size_t(n) < kEthHeaderSize) return;
    Peer* p = TouchPeer(from, now);
    if (!p) return;
    // Learn() may erase other peers, which invalidates |p|.
    const int id = p->id;
    const bool speaks_control = p->speaks_control;
    if (Learn(buf + 6, id, now) && speaks_control) {
      SendCtl(from, kCtlDupMac);
    }
    Forward(buf, n, id, now);
  }

  // --- Discovery ----------------------------------------------------------
  void DrainDiscovery(uint8_t* buf) {
    for (;;) {
      sockaddr_in from;
      socklen_t len = sizeof(from);
      ssize_t n = recvfrom(disc_fd_, buf, kMaxDatagram, 0,
                           reinterpret_cast<sockaddr*>(&from), &len);
      if (n < 0) return;
      if (size_t(n) < sizeof(kDiscoverRequest) - 1 ||
          memcmp(buf, kDiscoverRequest, sizeof(kDiscoverRequest) - 1) != 0) {
        continue;
      }
      std::string reply = kDiscoverReplyPrefix;
      reply += std::to_string(cfg_.port) + "\n" + cfg_.name + "\n" +
               cfg_.game + "\n" + std::to_string(peers_.size() + 1);
      sendto(disc_fd_, reply.data(), reply.size(), MSG_DONTWAIT,
             reinterpret_cast<const sockaddr*>(&from), sizeof(from));
    }
  }

  // --- Switching ----------------------------------------------------------
  // Records which port |mac| lives behind. Returns true the first time the
  // MAC shows up behind a second port, i.e. two consoles share an EEPROM MAC.
  bool Learn(const uint8_t* mac, int owner, int64_t now) {
    if (mac[0] & 1) return false;  // never learn a group address
    for (MacEntry& e : macs_) {
      if (memcmp(e.mac, mac, 6) != 0) continue;
      if (e.owner != owner && SameHost(e.owner, owner)) {
        // The emulator on that device restarted and came back from a new
        // source port; retire the old entry instead of calling it a clash.
        RemovePeerById(e.owner);
      }
      bool conflict = e.owner != owner &&
                      now - e.last_seen < kPeerTimeoutMs &&
                      OwnerAlive(e.owner);
      if (conflict) {
        if (!e.dup_reported) {
          e.dup_reported = true;
          LAN_WARN("MAC %s is used by more than one console; System Link "
                   "needs a unique MAC per console", MacStr(mac).c_str());
          Post(Event::DuplicateMac);
          return true;
        }
        return false;
      }
      e.owner = owner;
      e.last_seen = now;
      return false;
    }
    if (macs_.size() >= kMaxMacs) {
      size_t oldest = 0;
      for (size_t i = 1; i < macs_.size(); i++) {
        if (macs_[i].last_seen < macs_[oldest].last_seen) oldest = i;
      }
      macs_.erase(macs_.begin() + oldest);
    }
    MacEntry e = {};
    memcpy(e.mac, mac, 6);
    e.owner = owner;
    e.last_seen = now;
    macs_.push_back(e);
    return false;
  }

  void Forward(const uint8_t* frame, ssize_t n, int src_owner, int64_t now) {
    const uint8_t* dst = frame;
    if (!(dst[0] & 1)) {
      for (const MacEntry& e : macs_) {
        if (memcmp(e.mac, dst, 6) != 0 || now - e.last_seen > kMacTimeoutMs) {
          continue;
        }
        if (e.owner == src_owner) return;  // same segment, nothing to do
        if (SendToOwner(e.owner, frame, n)) return;
        break;  // stale owner: fall through to flooding
      }
    }
    // Broadcast, multicast or unknown unicast: flood.
    if (src_owner != kLocalOwner) SendToQemu(frame, n);
    for (const Peer& p : peers_) {
      if (p.id != src_owner) SendTo(p.addr, frame, n);
    }
  }

  bool SendToOwner(int owner, const uint8_t* frame, ssize_t n) {
    if (owner == kLocalOwner) {
      SendToQemu(frame, n);
      return true;
    }
    for (const Peer& p : peers_) {
      if (p.id == owner) {
        SendTo(p.addr, frame, n);
        return true;
      }
    }
    return false;
  }

  const Peer* FindPeer(int id) const {
    for (const Peer& p : peers_) {
      if (p.id == id) return &p;
    }
    return nullptr;
  }

  bool OwnerAlive(int owner) const {
    return owner == kLocalOwner || FindPeer(owner) != nullptr;
  }

  bool SameHost(int a, int b) const {
    const Peer* pa = FindPeer(a);
    const Peer* pb = FindPeer(b);
    return pa && pb && pa->addr.sin_addr.s_addr == pb->addr.sin_addr.s_addr;
  }

  void RemovePeerById(int id) {
    if (const Peer* p = FindPeer(id)) RemovePeer(p->addr);
  }

  Peer* TouchPeer(const sockaddr_in& addr, int64_t now) {
    for (Peer& p : peers_) {
      if (SameAddr(p.addr, addr)) {
        p.last_seen = now;
        return &p;
      }
    }
    if (peers_.size() >= kMaxPeers) {
      // Evict the quietest peer rather than refusing a new console.
      size_t oldest = 0;
      for (size_t i = 1; i < peers_.size(); i++) {
        if (peers_[i].last_seen < peers_[oldest].last_seen) oldest = i;
      }
      LAN_WARN("peer table full, dropping %s",
               AddrStr(peers_[oldest].addr).c_str());
      peers_.erase(peers_.begin() + oldest);
    }
    peers_.push_back({next_peer_id_++, addr, now, false});
    LAN_INFO("console joined from %s (%zu connected)", AddrStr(addr).c_str(),
             peers_.size());
    Post(Event::PeerJoined, int(peers_.size()));
    return &peers_.back();
  }

  void RemovePeer(const sockaddr_in& addr) {
    for (size_t i = 0; i < peers_.size(); i++) {
      if (SameAddr(peers_[i].addr, addr)) {
        peers_.erase(peers_.begin() + i);
        LAN_INFO("console at %s left (%zu connected)", AddrStr(addr).c_str(),
                 peers_.size());
        Post(Event::PeerLeft, int(peers_.size()));
        return;
      }
    }
  }

  void Tick(int64_t now) {
    if (cfg_.mode == Mode::Join) {
      SendCtl(host_addr_, kCtlHello);
      if (connected_ && now - last_ack_ > kHostTimeoutMs) {
        connected_ = false;
        LAN_WARN("System Link host %s stopped answering",
                 AddrStr(host_addr_).c_str());
        Post(Event::ConnectionLost);
      }
      return;
    }
    for (size_t i = 0; i < peers_.size();) {
      if (peers_[i].speaks_control &&
          now - peers_[i].last_seen > kPeerTimeoutMs) {
        LAN_INFO("console at %s timed out", AddrStr(peers_[i].addr).c_str());
        peers_.erase(peers_.begin() + i);
        Post(Event::PeerLeft, int(peers_.size()));
      } else {
        i++;
      }
    }
  }

  // --- I/O helpers --------------------------------------------------------
  static bool IsCtl(const uint8_t* buf, ssize_t n) {
    return size_t(n) == kLanCtlSize && buf[0] == 'X' && buf[1] == '1' &&
           buf[2] == 'L';
  }

  void SendCtl(const sockaddr_in& to, char type) {
    const uint8_t pkt[kLanCtlSize] = {'X', '1', 'L', uint8_t(type)};
    SendTo(to, pkt, sizeof(pkt));
  }

  void SendTo(const sockaddr_in& to, const uint8_t* buf, ssize_t n) {
    // Dropping on a full socket buffer is what a real switch does too; the
    // games' own protocols retransmit.
    sendto(lan_fd_, buf, n, MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
  }

  void SendToQemu(const uint8_t* buf, ssize_t n) {
    sendto(qemu_fd_, buf, n, MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&qemu_addr_), sizeof(qemu_addr_));
  }

  Config cfg_;
  int qemu_fd_ = -1;
  int lan_fd_ = -1;
  int disc_fd_ = -1;
  int wake_[2] = {-1, -1};
  sockaddr_in qemu_addr_ = {};
  sockaddr_in host_addr_ = {};
  bool connected_ = false;
  int64_t last_ack_ = 0;
  bool dup_warned_ = false;
  int next_peer_id_ = 0;
  std::vector<Peer> peers_;
  std::vector<MacEntry> macs_;
};

Relay* g_relay = nullptr;
pthread_t g_thread;

// --- Auto ------------------------------------------------------------------
// Everyone can pick Auto. For kAutoScanMs this device broadcasts the Find
// probe (the one SystemLink.discover sends) and listens on the discovery
// port: a host's reply means join it (the one with the most consoles, then
// the lowest address). Probes from other devices mean they are deciding too,
// so with no host yet the lowest address hosts and the others join it (their
// hellos connect once it is up). Nobody at all: host.
constexpr int64_t kAutoScanMs = 2000;
constexpr int64_t kAutoProbeEveryMs = 400;

struct AutoHost {
  sockaddr_in addr;
  int consoles;
};

// This device's IPv4 addresses and the broadcast addresses of its LANs.
void LocalNetworks(std::vector<uint32_t>* own, std::vector<uint32_t>* bcast) {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    own->push_back(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
      bcast->push_back(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
    }
  }
  freeifaddrs(list);
}

// Turns an Auto config into Host or Join.
void ResolveAuto(Config* cfg) {
  std::vector<uint32_t> own, bcast;
  LocalNetworks(&own, &bcast);
  bcast.push_back(htonl(INADDR_BROADCAST));
  int fd = OpenUdp(INADDR_ANY, kDiscoveryPort, nullptr);
  const bool listening = fd >= 0;   // another relay here holds it: probe only
  if (!listening) fd = OpenUdp(INADDR_ANY, 0, nullptr);
  if (fd < 0) {
    cfg->mode = Mode::Host;
    return;
  }
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
  // A few hundred ms of jitter: two phones started together don't probe in lockstep.
  timespec jitter = {0, long(rand() % 400) * 1000000L};
  nanosleep(&jitter, nullptr);

  std::vector<AutoHost> hosts;
  std::vector<uint32_t> deciding;
  auto mine = [&](uint32_t a) {
    for (uint32_t o : own) if (o == a) return true;
    return false;
  };
  std::vector<uint8_t> buf(2048);
  const int64_t end = NowMs() + kAutoScanMs;
  int64_t next_probe = 0;
  for (int64_t now = NowMs(); now < end; now = NowMs()) {
    if (now >= next_probe) {
      next_probe = now + kAutoProbeEveryMs;
      for (uint32_t b : bcast) {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = b;
        to.sin_port = htons(kDiscoveryPort);
        sendto(fd, kDiscoverRequest, sizeof(kDiscoverRequest) - 1, MSG_DONTWAIT,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
      }
    }
    pollfd p = {fd, POLLIN, 0};
    const int64_t wait = std::min(next_probe, end) - now;
    if (poll(&p, 1, int(wait > 0 ? wait : 0)) <= 0) continue;
    for (;;) {
      sockaddr_in from;
      socklen_t len = sizeof(from);
      ssize_t n = recvfrom(fd, buf.data(), buf.size() - 1, MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&from), &len);
      if (n < 0) break;
      if (mine(from.sin_addr.s_addr)) continue;  // our own probes, looped back
      const std::string text(reinterpret_cast<char*>(buf.data()), size_t(n));
      if (text.compare(0, sizeof(kDiscoverReplyPrefix) - 1, kDiscoverReplyPrefix) == 0) {
        // "X1LAN-HOST\n<port>\n<name>\n<game>\n<consoles>"
        std::vector<std::string> f;
        size_t at = sizeof(kDiscoverReplyPrefix) - 1;
        while (at <= text.size()) {
          size_t nl = text.find('\n', at);
          f.push_back(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
          if (nl == std::string::npos) break;
          at = nl + 1;
        }
        int port = f.empty() ? 0 : atoi(f[0].c_str());
        if (port <= 0 || port > 65535) continue;
        AutoHost h = {from, f.size() > 3 ? atoi(f[3].c_str()) : 1};
        h.addr.sin_port = htons(uint16_t(port));
        bool known = false;
        for (const AutoHost& k : hosts) known |= SameAddr(k.addr, h.addr);
        if (!known) hosts.push_back(h);
      } else if (text.compare(0, sizeof(kDiscoverRequest) - 1, kDiscoverRequest) == 0) {
        bool known = false;
        for (uint32_t d : deciding) known |= d == from.sin_addr.s_addr;
        if (!known) deciding.push_back(from.sin_addr.s_addr);
      }
    }
  }
  close(fd);

  if (!hosts.empty()) {
    const AutoHost* best = &hosts[0];
    for (const AutoHost& h : hosts) {
      if (h.consoles > best->consoles ||
          (h.consoles == best->consoles && ntohl(h.addr.sin_addr.s_addr) < ntohl(best->addr.sin_addr.s_addr))) {
        best = &h;
      }
    }
    cfg->mode = Mode::Join;
    cfg->host_addr = AddrStr(best->addr);
    LAN_INFO("auto: joining the host at %s", cfg->host_addr.c_str());
    return;
  }
  // No host yet. Among the devices deciding now, the lowest address hosts.
  uint32_t lowest = 0;
  for (uint32_t d : deciding) {
    if (!lowest || ntohl(d) < ntohl(lowest)) lowest = d;
  }
  uint32_t me = 0;
  for (uint32_t o : own) {
    if (!me || ntohl(o) < ntohl(me)) me = o;
  }
  if (lowest && me && ntohl(lowest) < ntohl(me)) {
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = lowest;
    a.sin_port = htons(cfg->port);
    cfg->mode = Mode::Join;
    cfg->host_addr = AddrStr(a);
    LAN_INFO("auto: no host yet; %s starts one, joining it", cfg->host_addr.c_str());
    return;
  }
  cfg->mode = Mode::Host;
  LAN_INFO("auto: no host found, hosting");
}

void* RelayThread(void* arg) {
  static_cast<Relay*>(arg)->Run();
  return nullptr;
}

}  // namespace

Mode ParseMode(const std::string& value) {
  if (value == "host") return Mode::Host;
  if (value == "join") return Mode::Join;
  if (value == "auto") return Mode::Auto;
  return Mode::Off;
}

bool Start(const Config& config, std::string* qemu_bind, std::string* qemu_remote) {
  if (g_relay || config.mode == Mode::Off) return false;
  Config cfg = config;
  if (cfg.mode == Mode::Auto) ResolveAuto(&cfg);
  Relay* relay = new Relay(cfg);
  if (!relay->Init(qemu_bind, qemu_remote)) {
    delete relay;
    return false;
  }
  if (pthread_create(&g_thread, nullptr, RelayThread, relay) != 0) {
    Post(Event::StartFailed, 0);
    delete relay;
    return false;
  }
  pthread_setname_np(g_thread, "xemu-lan");
  g_relay = relay;
  return true;
}

void Stop() {
  if (!g_relay) return;
  g_relay->Wake();
  pthread_join(g_thread, nullptr);
  delete g_relay;
  g_relay = nullptr;
}

bool PollEvent(Event* event, int* arg) {
  std::lock_guard<std::mutex> lock(g_events_mutex);
  if (g_events.empty()) return false;
  *event = g_events.front().first;
  *arg = g_events.front().second;
  g_events.pop_front();
  return true;
}

}  // namespace xemu_lan

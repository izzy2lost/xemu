// System Link (LAN) relay for the Android build.
//
// QEMU's "socket,udp=" netdev only talks to a single peer, which is enough
// for two consoles but not for a System Link party. This relay sits between
// QEMU and the LAN: QEMU sends and receives raw Ethernet frames over a
// loopback UDP pair, and the relay either
//   - hosts: acts as a learning Ethernet switch for every device that joins,
//     including this one's guest, or
//   - joins: forwards everything to a host and keeps the host's peer entry
//     alive.
//
// XLink Kai: the relay is an emulator client of a Kai Engine (on a PC or
// Raspberry Pi on the LAN, which tunnels System Link over the internet). It
// speaks the engine's UDP protocol on port 34523, as Dolphin's XLink Kai
// adapter does: "connect;<name>;<emulator>;" and "disconnect;...", each
// frame as "e;e;<frame>" both ways, the engine's "keepalive;" echoed.
//
// On the LAN each datagram is one raw Ethernet frame on port 9368, the same
// format desktop xemu's UDP backend uses, so a desktop xemu pointed at the
// host (or that the phone joins) interoperates for two-player sessions.
// Relay control packets are kLanCtlSize bytes; nvnet drops anything shorter
// than 6 bytes, so they are harmless if they reach a desktop guest.
#pragma once

#include <cstdint>
#include <string>

namespace xemu_lan {

constexpr uint16_t kDefaultPort = 9368;
constexpr uint16_t kDiscoveryPort = 9369;
constexpr uint16_t kKaiPort = 34523;

// Auto: look for a host on the LAN for a couple of seconds and join it, else
// host (Start decides; see ResolveAuto in xemu_lan.cpp).
// Kai: XLink Kai, host_addr the engine's address.
enum class Mode { Off, Host, Join, Auto, Kai };

Mode ParseMode(const std::string& value);

struct Config {
  Mode mode = Mode::Off;
  std::string host_addr;  // Join, Kai: "host" or "host:port"
  uint16_t port = kDefaultPort;
  std::string name;  // advertised in discovery replies (Host)
  std::string game;  // advertised in discovery replies (Host)
};

// Starts the relay thread. On success returns the "ip:port" strings QEMU's
// UDP socket netdev must use: it binds |qemu_bind| and sends to |qemu_remote|.
// On failure a StartFailed / HostUnresolved event is queued for the UI.
bool Start(const Config& cfg, std::string* qemu_bind, std::string* qemu_remote);
void Stop();

// Things the UI should tell the player about. The numeric values cross JNI.
enum class Event : int {
  StartFailed = 1,     // arg: the port that could not be opened
  HostUnresolved = 2,  // join address did not resolve
  Connected = 3,       // join: the host answered
  ConnectionLost = 4,  // join: the host stopped answering
  PeerJoined = 5,      // host: arg = consoles connected besides this one
  PeerLeft = 6,        // host: arg = consoles connected besides this one
  DuplicateMac = 7,    // two consoles share an EEPROM MAC address
  Hosting = 8,         // host: the relay is listening, arg = port
  KaiConnected = 9,    // the XLink Kai Engine accepted this console
  KaiDisconnected = 10,  // the engine dropped it (it reconnects when it can)
  KaiNoEngine = 11,    // no answer from the engine yet (arg: seconds tried)
};

// Pops the oldest pending event. Safe to call from any thread.
bool PollEvent(Event* event, int* arg);

}  // namespace xemu_lan

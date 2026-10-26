/*
 * The hakuX core's internet: libslirp (as xemu's "user" network) behind the
 * core's Ethernet controller. The controller sends each frame as one
 * datagram to a loopback port (native/dev/nvnet.c, as for System Link) and
 * takes the frames for it from another; this thread feeds slirp the first
 * and sends what slirp answers to the second. The guest sees xemu's
 * network: 10.0.2.0/24, gateway 10.0.2.2, DHCP from 10.0.2.15, DNS 10.0.2.3.
 *
 * Android has no /etc/resolv.conf for slirp's DNS forwarding, so queries to
 * 10.0.2.3 go to a DNS server of the app's choosing (Insignia's), their
 * answers coming back as from 10.0.2.3.
 *
 * Xbox Live's names are answered here, whatever server the guest asks, as
 * insigniaDNS (github.com/insignia-live/insigniaDNS) does: Insignia's zone
 * list (insignia.live/dns_zones.json, fetched by the app over HTTPS) names
 * the host each one stands for, looked up with the phone's resolver. A
 * network that answers every DNS query itself (a router or ISP "hijacking"
 * port 53) otherwise gives the guest Microsoft's addresses, and the
 * Setup Assistant reports the hijack.
 */
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <jni.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include "libslirp.h"

#define TAG "x1box-nat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define MAX_FDS 256
#define MAX_TIMERS 16
#define FRAME_MAX 2048
#define MAX_ZONES 32

typedef struct Zone {
    char name[96], host[96];    /* the name asked, the host it stands for */
    uint32_t addr;              /* host's IPv4 (network order), 0: not looked up */
    int64_t addr_ns;            /* when */
} Zone;

typedef struct Timer {
    bool used, armed;
    SlirpTimerId id;
    void *cb_opaque;
    int64_t expire_ms;
} Timer;

static struct {
    Slirp *slirp;
    int sock;                   /* bound to our port; frames from the core */
    struct sockaddr_in core;    /* the core's port: frames for it */
    struct in_addr vdns;        /* 10.0.2.3 */
    struct in_addr dns;         /* where its queries go */
    struct pollfd fds[MAX_FDS];
    int nfds;
    Timer timers[MAX_TIMERS];
    char spec[64];
    pthread_mutex_t lock;
    uint64_t to_guest, from_guest;
    Zone zones[MAX_ZONES];
    int nzones;
    uint16_t vdns_ports[32];    /* the guest's ports whose queries went to 10.0.2.3 */
    unsigned vdns_next;
} g_nat = { .sock = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

static void trace(const char *dir, const uint8_t *f, int len);

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* RFC 1624: a checksum after a 32-bit field changed from old to new */
static uint16_t csum_fix(uint16_t sum, uint32_t old_be, uint32_t new_be)
{
    uint32_t s = (uint16_t)~ntohs(sum);
    s += (uint16_t)~(ntohl(old_be) >> 16) + (uint16_t)~(ntohl(old_be) & 0xFFFF);
    s += (ntohl(new_be) >> 16) + (ntohl(new_be) & 0xFFFF);
    while (s >> 16) {
        s = (s & 0xFFFF) + (s >> 16);
    }
    return htons((uint16_t)~s);
}

/* An IPv4 UDP frame's address (dst: the destination, else the source) and
 * port at that end, or false. */
static bool udp_end(uint8_t *f, int len, bool dst, uint32_t **addr, uint16_t *port, uint8_t **ip,
                    uint8_t **udp)
{
    if (len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00) {
        return false;
    }
    uint8_t *h = f + 14;
    const int ihl = (h[0] & 0x0F) * 4;
    if ((h[0] >> 4) != 4 || ihl < 20 || h[9] != 17 || len < 14 + ihl + 8 ||
        (ntohs(*(uint16_t *)(h + 6)) & 0x3FFF)) {    /* not a fragment */
        return false;
    }
    uint8_t *u = h + ihl;
    *addr = (uint32_t *)(h + (dst ? 16 : 12));
    *port = ntohs(*(uint16_t *)(u + (dst ? 2 : 0)));
    *ip = h;
    *udp = u;
    return true;
}

static void readdress(uint8_t *ip, uint8_t *udp, uint32_t *addr, uint32_t to)
{
    const uint32_t from = *addr;
    *addr = to;
    uint16_t *ipsum = (uint16_t *)(ip + 10);
    *ipsum = csum_fix(*ipsum, from, to);
    uint16_t *usum = (uint16_t *)(udp + 6);
    if (*usum) {
        *usum = csum_fix(*usum, from, to);
        if (!*usum) {
            *usum = 0xFFFF;
        }
    }
}

static bool vdns_port(uint16_t port)
{
    for (unsigned i = 0; i < 32; i++) {
        if (g_nat.vdns_ports[i] == port) {
            return true;
        }
    }
    return false;
}

/* slirp -> the guest */
static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque)
{
    uint8_t f[FRAME_MAX];
    if (len > sizeof(f)) {
        return (slirp_ssize_t)len;
    }
    memcpy(f, buf, len);
    uint32_t *addr;
    uint16_t port;
    uint8_t *ip, *udp;
    if (udp_end(f, (int)len, false, &addr, &port, &ip, &udp) && port == 53 &&
        *addr == g_nat.dns.s_addr && vdns_port(ntohs(*(uint16_t *)(udp + 2)))) {
        /* an answer to a query the guest sent to 10.0.2.3 (one it sent the
         * server itself keeps the server's address) */
        readdress(ip, udp, addr, g_nat.vdns.s_addr);
    }
    trace("to it:", f, (int)len);
    sendto(g_nat.sock, f, len, MSG_DONTWAIT, (const struct sockaddr *)&g_nat.core, sizeof(g_nat.core));
    g_nat.to_guest++;
    return (slirp_ssize_t)len;
}

static void guest_error(const char *msg, void *opaque)
{
    LOGW("guest error: %s", msg);
}

static int64_t clock_get_ns(void *opaque)
{
    return now_ns();
}

static void *timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_nat.timers[i].used) {
            g_nat.timers[i] = (Timer){ .used = true, .id = id, .cb_opaque = cb_opaque };
            return &g_nat.timers[i];
        }
    }
    LOGW("out of timers");
    return NULL;
}

static void timer_free(void *timer, void *opaque)
{
    if (timer) {
        ((Timer *)timer)->used = false;
    }
}

static void timer_mod(void *timer, int64_t expire_ms, void *opaque)
{
    if (timer) {
        ((Timer *)timer)->expire_ms = expire_ms;
        ((Timer *)timer)->armed = true;
    }
}

static void register_poll_fd(int fd, void *opaque) {}
static void unregister_poll_fd(int fd, void *opaque) {}
static void notify(void *opaque) {}

static int add_poll(int fd, int events, void *opaque)
{
    if (g_nat.nfds >= MAX_FDS) {
        return -1;
    }
    short ev = 0;
    if (events & SLIRP_POLL_IN) ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    g_nat.fds[g_nat.nfds] = (struct pollfd){ .fd = fd, .events = ev };
    return g_nat.nfds++;
}

static int get_revents(int idx, void *opaque)
{
    if (idx < 0 || idx >= g_nat.nfds) {
        return 0;
    }
    const short r = g_nat.fds[idx].revents;
    int ev = 0;
    if (r & POLLIN) ev |= SLIRP_POLL_IN;
    if (r & POLLOUT) ev |= SLIRP_POLL_OUT;
    if (r & POLLPRI) ev |= SLIRP_POLL_PRI;
    if (r & POLLERR) ev |= SLIRP_POLL_ERR;
    if (r & POLLHUP) ev |= SLIRP_POLL_HUP;
    return ev;
}

/* A DNS query's name, lower case and dotted, and where its question ends;
 * false if it isn't one query. */
static bool dns_question(const uint8_t *q, int len, char *name, size_t size, int *end, uint16_t *qtype)
{
    if (len < 12 + 5 || (q[2] & 0x80) || q[4] != 0 || q[5] != 1) {
        return false;
    }
    int i = 12;
    size_t n = 0;
    while (i < len && q[i]) {
        const int l = q[i++];
        if (l > 63 || i + l > len || n + (size_t)l + 2 > size) {
            return false;
        }
        if (n) {
            name[n++] = '.';
        }
        for (int k = 0; k < l; k++) {
            const char c = (char)q[i + k];
            name[n++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
        i += l;
    }
    if (i + 5 > len) {
        return false;
    }
    name[n] = 0;
    *qtype = (uint16_t)(q[i + 1] << 8 | q[i + 2]);
    *end = i + 5;
    return true;
}

static uint32_t zone_addr(Zone *z)
{
    const int64_t now = now_ns();
    if (z->addr && now - z->addr_ns < 300000000000ll) {
        return z->addr;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    if (getaddrinfo(z->host, NULL, &hints, &res) == 0 && res) {
        z->addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        z->addr_ns = now;
        char a[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &z->addr, a, sizeof(a));
        LOGI("DNS: %s is %s (%s)", z->name, a, z->host);
        freeaddrinfo(res);
    } else {
        LOGW("DNS: can't look up %s (for %s)", z->host, z->name);
    }
    return z->addr;
}

static uint16_t ip_sum(const uint8_t *h, int len)
{
    uint32_t s = 0;
    for (int i = 0; i < len; i += 2) {
        s += (uint32_t)(h[i] << 8 | h[i + 1]);
    }
    while (s >> 16) {
        s = (s & 0xFFFF) + (s >> 16);
    }
    return htons((uint16_t)~s);
}

/* A query for one of Insignia's names (the guest's frame f): answered here,
 * to the guest. False: not one (it goes on to slirp). */
static bool dns_local(const uint8_t *f, int len)
{
    uint32_t *addr;
    uint16_t port, qtype;
    uint8_t *ip, *udp;
    char name[256];
    int qend;
    if (!g_nat.nzones || !udp_end((uint8_t *)f, len, true, &addr, &port, &ip, &udp) || port != 53) {
        return false;
    }
    const uint8_t *q = udp + 8;
    const int qlen = len - (int)(q - f);
    if (!dns_question(q, qlen, name, sizeof(name), &qend, &qtype)) {
        return false;
    }
    Zone *z = NULL;
    for (int i = 0; i < g_nat.nzones && !z; i++) {
        if (!strcmp(g_nat.zones[i].name, name)) {
            z = &g_nat.zones[i];
        }
    }
    if (!z) {
        return false;
    }
    const bool want_a = qtype == 1 || qtype == 255;
    const uint32_t a = want_a ? zone_addr(z) : 0;
    if (want_a && !a) {
        return false;
    }
    uint8_t r[FRAME_MAX];
    const int ihl = 20;
    uint8_t *rip = r + 14, *rudp = rip + ihl, *rq = rudp + 8;
    memcpy(r, f + 6, 6);                    /* to the guest's MAC */
    memcpy(r + 6, f, 6);                    /* from the one it sent to */
    r[12] = 0x08;
    r[13] = 0x00;
    memcpy(rq, q, (size_t)qend);            /* its header and question */
    rq[2] = 0x81 | (q[2] & 0x01);           /* a response, recursion desired as asked */
    rq[3] = 0x80;                           /* recursion available, no error */
    rq[6] = 0; rq[7] = a ? 1 : 0;           /* answers */
    rq[8] = rq[9] = rq[10] = rq[11] = 0;
    int dl = qend;
    if (a) {
        static const uint8_t rr[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0x01, 0x2C, 0, 4 };
        memcpy(rq + dl, rr, sizeof(rr));
        memcpy(rq + dl + sizeof(rr), &a, 4);
        dl += (int)sizeof(rr) + 4;
    }
    const uint16_t ulen = (uint16_t)(8 + dl), tlen = (uint16_t)(ihl + ulen);
    memset(rip, 0, (size_t)ihl);
    rip[0] = 0x45;
    *(uint16_t *)(rip + 2) = htons(tlen);
    rip[8] = 64;
    rip[9] = 17;
    memcpy(rip + 12, ip + 16, 4);           /* from the server it asked */
    memcpy(rip + 16, ip + 12, 4);
    *(uint16_t *)(rip + 10) = ip_sum(rip, ihl);
    memcpy(rudp, udp + 2, 2);               /* ports swapped */
    memcpy(rudp + 2, udp, 2);
    *(uint16_t *)(rudp + 4) = htons(ulen);
    *(uint16_t *)(rudp + 6) = 0;            /* no checksum (IPv4) */
    trace("to it:", r, 14 + tlen);
    static unsigned s_dumped;
    if (s_dumped++ < 2) {
        char hex[3 * FRAME_MAX / 8];
        int n = 0;
        for (int i = 0; i < 14 + tlen && n < (int)sizeof(hex) - 3; i++) {
            n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02x", r[i]);
        }
        LOGI("DNS answer frame: %s", hex);
        n = 0;
        for (int i = 0; i < len && n < (int)sizeof(hex) - 3; i++) {
            n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02x", f[i]);
        }
        LOGI("DNS query frame: %s", hex);
    }
    sendto(g_nat.sock, r, (size_t)(14 + tlen), MSG_DONTWAIT, (const struct sockaddr *)&g_nat.core,
           sizeof(g_nat.core));
    g_nat.to_guest++;
    return true;
}

/* The first frames each way, for the log: type, addresses, ports. */
static void trace(const char *dir, const uint8_t *f, int len)
{
    static unsigned s_logged;
    if (s_logged >= 200 || len < 14) {
        return;
    }
    s_logged++;
    const uint16_t type = (uint16_t)(f[12] << 8 | f[13]);
    if (type == 0x0806 && len >= 42) {
        LOGI("%s ARP op %u %u.%u.%u.%u -> %u.%u.%u.%u", dir, f[21], f[28], f[29], f[30], f[31], f[38],
             f[39], f[40], f[41]);
    } else if (type == 0x0800 && len >= 34) {
        const uint8_t *h = f + 14;
        const int ihl = (h[0] & 0x0F) * 4;
        const uint8_t *p = h + ihl;
        const unsigned sp = len >= 14 + ihl + 4 ? (unsigned)(p[0] << 8 | p[1]) : 0;
        const unsigned dp = len >= 14 + ihl + 4 ? (unsigned)(p[2] << 8 | p[3]) : 0;
        LOGI("%s IP proto %u %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len %d", dir, h[9], h[12], h[13], h[14],
             h[15], sp, h[16], h[17], h[18], h[19], dp, len);
    } else {
        LOGI("%s type %04x len %d", dir, type, len);
    }
}

/* the guest -> slirp */
static void from_guest(void)
{
    uint8_t f[FRAME_MAX];
    for (;;) {
        const ssize_t n = recv(g_nat.sock, f, sizeof(f), MSG_DONTWAIT);
        if (n <= 0) {
            return;
        }
        trace("console:", f, (int)n);
        g_nat.from_guest++;
        if (dns_local(f, (int)n)) {
            continue;
        }
        uint32_t *addr;
        uint16_t port;
        uint8_t *ip, *udp;
        if (udp_end(f, (int)n, true, &addr, &port, &ip, &udp) && port == 53 &&
            *addr == g_nat.vdns.s_addr) {
            const uint16_t sport = ntohs(*(uint16_t *)udp);
            if (!vdns_port(sport)) {
                g_nat.vdns_ports[g_nat.vdns_next++ % 32] = sport;
            }
            readdress(ip, udp, addr, g_nat.dns.s_addr);
        }
        slirp_input(g_nat.slirp, f, (int)n);
    }
}

static void *nat_thread(void *arg)
{
    int64_t last_log = 0;
    for (;;) {
        uint32_t timeout = 100;
        g_nat.nfds = 0;
        add_poll(g_nat.sock, SLIRP_POLL_IN, NULL);
        slirp_pollfds_fill(g_nat.slirp, &timeout, add_poll, NULL);
        const int64_t ms = now_ns() / 1000000;
        for (int i = 0; i < MAX_TIMERS; i++) {
            const Timer *t = &g_nat.timers[i];
            if (t->used && t->armed) {
                const int64_t left = t->expire_ms - ms;
                timeout = left <= 0 ? 0 : (uint32_t)(left < timeout ? left : timeout);
            }
        }
        const int r = poll(g_nat.fds, (nfds_t)g_nat.nfds, (int)timeout);
        if (r > 0 && (g_nat.fds[0].revents & POLLIN)) {
            from_guest();
        }
        slirp_pollfds_poll(g_nat.slirp, r < 0, get_revents, NULL);
        const int64_t now = now_ns() / 1000000;
        for (int i = 0; i < MAX_TIMERS; i++) {
            Timer *t = &g_nat.timers[i];
            if (t->used && t->armed && t->expire_ms <= now) {
                t->armed = false;
                slirp_handle_timer(g_nat.slirp, t->id, t->cb_opaque);
            }
        }
        if (now - last_log > 30000) {
            last_log = now;
            LOGI("frames: %llu from the console, %llu to it", (unsigned long long)g_nat.from_guest,
                 (unsigned long long)g_nat.to_guest);
        }
    }
    return NULL;
}

static int bind_loopback(uint16_t *port)
{
    const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        getsockname(s, (struct sockaddr *)&a, &al) < 0) {
        if (s >= 0) {
            close(s);
        }
        return -1;
    }
    *port = ntohs(a.sin_port);
    return s;
}

/* Starts the network (once a process): its spec for the core's controller,
 * "<the core's bind>,<our address>", or NULL. */
static const char *nat_start(const char *dns, const char *zones)
{
    pthread_mutex_lock(&g_nat.lock);
    if (g_nat.slirp) {
        pthread_mutex_unlock(&g_nat.lock);
        return g_nat.spec;
    }
    uint16_t ours, theirs;
    const int probe = bind_loopback(&theirs);   /* a free port for the core to bind */
    if (probe >= 0) {
        close(probe);
    }
    g_nat.sock = bind_loopback(&ours);
    if (probe < 0 || g_nat.sock < 0) {
        LOGW("no loopback ports: %s", strerror(errno));
        pthread_mutex_unlock(&g_nat.lock);
        return NULL;
    }
    g_nat.core = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(theirs),
                                       .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    inet_pton(AF_INET, "10.0.2.3", &g_nat.vdns);
    /* "name=host" lines */
    for (const char *z = zones; z && *z && g_nat.nzones < MAX_ZONES;) {
        const char *nl = strchr(z, '\n');
        const size_t l = nl ? (size_t)(nl - z) : strlen(z);
        const char *eq = memchr(z, '=', l);
        if (eq && eq > z && (size_t)(eq - z) < sizeof(g_nat.zones[0].name) &&
            l - (size_t)(eq - z) - 1 < sizeof(g_nat.zones[0].host) && l > (size_t)(eq - z) + 1) {
            Zone *d = &g_nat.zones[g_nat.nzones++];
            memcpy(d->name, z, (size_t)(eq - z));
            memcpy(d->host, eq + 1, l - (size_t)(eq - z) - 1);
            for (char *c = d->name; *c; c++) {
                *c = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
            }
        }
        z = nl ? nl + 1 : NULL;
    }
    if (!dns || inet_pton(AF_INET, dns, &g_nat.dns) != 1) {
        inet_pton(AF_INET, "8.8.8.8", &g_nat.dns);
    }

    SlirpConfig cfg = { .version = 4, .restricted = 0, .in_enabled = true, .in6_enabled = false,
                        .if_mtu = 1500, .if_mru = 1500, .disable_host_loopback = true };
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    cfg.vnameserver = g_nat.vdns;
    cfg.vhostname = "x1box";
    static const SlirpCb cb = {
        .send_packet = send_packet,
        .guest_error = guest_error,
        .clock_get_ns = clock_get_ns,
        .timer_free = timer_free,
        .timer_mod = timer_mod,
        .register_poll_fd = register_poll_fd,
        .unregister_poll_fd = unregister_poll_fd,
        .notify = notify,
        .timer_new_opaque = timer_new_opaque,
    };
    g_nat.slirp = slirp_new(&cfg, &cb, NULL);
    if (!g_nat.slirp) {
        LOGW("slirp_new failed");
        close(g_nat.sock);
        g_nat.sock = -1;
        pthread_mutex_unlock(&g_nat.lock);
        return NULL;
    }
    pthread_t t;
    pthread_create(&t, NULL, nat_thread, NULL);
    pthread_detach(t);
    snprintf(g_nat.spec, sizeof(g_nat.spec), "127.0.0.1:%u,127.0.0.1:%u", theirs, ours);
    LOGI("started: the console's frames on %s, DNS 10.0.2.3 -> %s, %d Insignia names", g_nat.spec,
         dns ? dns : "8.8.8.8", g_nat.nzones);
    pthread_mutex_unlock(&g_nat.lock);
    return g_nat.spec;
}

JNIEXPORT jstring JNICALL
Java_com_izzy2lost_x1box_hakux_HakuxCoreActivity_nativeStartNat(JNIEnv *env, jclass cls, jstring jdns,
                                                                jstring jzones)
{
    const char *dns = jdns ? (*env)->GetStringUTFChars(env, jdns, NULL) : NULL;
    const char *zones = jzones ? (*env)->GetStringUTFChars(env, jzones, NULL) : NULL;
    const char *spec = nat_start(dns, zones);
    if (dns) {
        (*env)->ReleaseStringUTFChars(env, jdns, dns);
    }
    if (zones) {
        (*env)->ReleaseStringUTFChars(env, jzones, zones);
    }
    return spec ? (*env)->NewStringUTF(env, spec) : NULL;
}

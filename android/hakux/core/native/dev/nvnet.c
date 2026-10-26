/*
 * hakuX native core — the MCPX Ethernet controller (nForce MAC) at
 * 0xFEF00000, IRQ 4: System Link.
 *
 * xemu's device (hw/xbox/mcpx/nvnet/nvnet.c, LGPL-2.1+: Copyright (c) 2013
 * espes, 2015-2025 Matt Borgerson) on the core's MMIO and interrupt lines:
 * the same registers, descriptor rings, receive filter and PHY over MDIO.
 * The title's network stack (XNet, in its XBE) drives it straight; the
 * kernel only reports the link (PhyGetLinkState: nc_dev_nvnet_link_up).
 *
 * The wire is a loopback UDP socket to the app's System Link relay
 * (android/app/src/main/cpp/xemu_lan.cpp, as xemu's "socket,udp=" netdev
 * uses it): one raw Ethernet frame per datagram. Without one ("lan=" not
 * given) the controller is there with its cable unplugged.
 *
 * QEMU ran the device under its big lock; here frames arrive on the receive
 * thread while the guest's CPU touches the registers, so one mutex guards
 * it. A frame the guest has no buffer for waits in a short queue until it
 * hands buffers back (QEMU queued them in its net layer).
 */
#include "native/dev/dev.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"
#include "native/core/vm.h"
#include "native/dev/nvnet_regs.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <time.h>
#include <unistd.h>

#define NVNET_BASE 0xFEF00000u
#define NVNET_IRQ  4
#define MMIO_SIZE  0x400
#define PHY_ADDR   1
#define RXQ_MAX    64

/* hw/net/mii.h's, what the PHY uses */
#define MII_BMCR 0
#define MII_BMSR 1
#define MII_ANAR 4
#define MII_ANLPAR 5
#define MII_BMCR_AUTOEN (1 << 12)
#define MII_BMCR_FD (1 << 8)
#define MII_BMSR_AN_COMP (1 << 5)
#define MII_BMSR_AUTONEG (1 << 3)
#define MII_BMSR_LINK_ST (1 << 2)
#define MII_ANLPAR_ACK (1 << 14)
#define MII_ANLPAR_T4 (1 << 9)
#define MII_ANLPAR_TXFD (1 << 8)
#define MII_ANLPAR_TX (1 << 7)
#define MII_ANLPAR_10FD (1 << 6)
#define MII_ANLPAR_10 (1 << 5)

#define GET_MASK(v, mask) (((v) & (mask)) >> __builtin_ctz(mask))

struct RingDesc {
    uint32_t buffer_addr;
    uint16_t length;
    uint16_t flags;
} __attribute__((packed));

typedef struct Frame {
    uint16_t len;
    uint8_t data[RX_ALLOC_BUFSIZE];
} Frame;

typedef struct NvNet {
    pthread_mutex_t lock;
    NcVm *vm;
    uint8_t regs[MMIO_SIZE];
    uint32_t phy_regs[6];
    uint32_t tx_dma_buf_offset;
    uint8_t tx_dma_buf[TX_ALLOC_BUFSIZE];
    bool irq;
    /* the wire: the relay's loopback socket (-1: no cable) */
    int sock;
    struct sockaddr_in remote;
    Frame rxq[RXQ_MAX];
    unsigned rxq_head, rxq_n;
    uint64_t tx_frames, rx_frames, rx_dropped, kicks, kicks_idle;
} NvNet;

static NvNet g_net;
/* debug: debug.hakux.nvnet.trace 1 (or NC_NVNET_TRACE): the guest's register accesses, the first few thousand */
static int g_trace;
#define TRACE(...) do { if (g_trace > 0) { g_trace--; nc_log("hakuX-native-nvnet", __VA_ARGS__); } } while (0)
static char g_lan[96];                  /* "bind_ip:port,remote_ip:port" (nc_dev_nvnet_set_lan) */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------ registers */

static uint32_t get_reg_ext(NvNet *s, uint32_t addr, unsigned size)
{
    uint32_t v = 0;
    memcpy(&v, &s->regs[addr], size);
    return v;
}

static uint32_t get_reg(NvNet *s, uint32_t addr)
{
    return get_reg_ext(s, addr, 4);
}

static void set_reg_ext(NvNet *s, uint32_t addr, uint32_t val, unsigned size)
{
    memcpy(&s->regs[addr], &val, size);
}

static void set_reg(NvNet *s, uint32_t addr, uint32_t val)
{
    set_reg_ext(s, addr, val, 4);
}

static void or_reg(NvNet *s, uint32_t addr, uint32_t val)
{
    set_reg(s, addr, get_reg(s, addr) | val);
}

static void and_reg(NvNet *s, uint32_t addr, uint32_t val)
{
    set_reg(s, addr, get_reg(s, addr) & val);
}

static void set_reg_with_mask(NvNet *s, uint32_t addr, uint32_t val, uint32_t w_mask)
{
    set_reg(s, addr, ((get_reg(s, addr) & (val | ~w_mask)) | (val & w_mask)));
}

static void update_irq(NvNet *s)
{
    const bool level = (get_reg(s, NVNET_IRQ_MASK) & get_reg(s, NVNET_IRQ_STATUS)) != 0;
    if (level != s->irq) {
        s->irq = level;
        nc_irq_set(NVNET_IRQ, level);
    }
}

static void set_intr_status(NvNet *s, uint32_t status)
{
    or_reg(s, NVNET_IRQ_STATUS, status);
    update_irq(s);
}

/* ------------------------------------------------------------- DMA */

static bool dma_read(NvNet *s, uint32_t pa, void *buf, uint32_t len)
{
    if ((uint64_t)pa + len > s->vm->ram_size) {
        memset(buf, 0, len);
        return false;
    }
    memcpy(buf, s->vm->phys + pa, len);
    return true;
}

static bool dma_write(NvNet *s, uint32_t pa, const void *buf, uint32_t len)
{
    if ((uint64_t)pa + len > s->vm->ram_size) {
        return false;
    }
    memcpy(s->vm->phys + pa, buf, len);
    return true;
}

static uint16_t get_tx_ring_size(NvNet *s)
{
    return GET_MASK(get_reg(s, NVNET_RING_SIZE), NVNET_RING_SIZE_TX) + 1;
}

static uint16_t get_rx_ring_size(NvNet *s)
{
    return GET_MASK(get_reg(s, NVNET_RING_SIZE), NVNET_RING_SIZE_RX) + 1;
}

static void reset_descriptor_ring_pointers(NvNet *s)
{
    uint32_t base = get_reg(s, NVNET_TX_RING_PHYS_ADDR);
    set_reg(s, NVNET_TX_RING_CURRENT_DESC_PHYS_ADDR, base);
    set_reg(s, NVNET_TX_RING_NEXT_DESC_PHYS_ADDR, base);
    base = get_reg(s, NVNET_RX_RING_PHYS_ADDR);
    set_reg(s, NVNET_RX_RING_CURRENT_DESC_PHYS_ADDR, base);
    set_reg(s, NVNET_RX_RING_NEXT_DESC_PHYS_ADDR, base);
}

static bool link_up(NvNet *s)
{
    return s->phy_regs[MII_BMSR] & MII_BMSR_LINK_ST;
}

static bool dma_enabled(NvNet *s)
{
    return (get_reg(s, NVNET_TX_RX_CONTROL) & NVNET_TX_RX_CONTROL_BIT2) == 0;
}

static void set_dma_idle(NvNet *s, bool idle)
{
    if (idle) {
        or_reg(s, NVNET_TX_RX_CONTROL, NVNET_TX_RX_CONTROL_IDLE);
    } else {
        and_reg(s, NVNET_TX_RX_CONTROL, ~NVNET_TX_RX_CONTROL_IDLE);
    }
}

static bool rx_enabled(NvNet *s)
{
    return get_reg(s, NVNET_RECEIVER_CONTROL) & NVNET_RECEIVER_CONTROL_START;
}

static uint32_t update_current_rx_ring_desc_addr(NvNet *s)
{
    const uint32_t base = get_reg(s, NVNET_RX_RING_PHYS_ADDR);
    const uint32_t max = base + get_rx_ring_size(s) * sizeof(struct RingDesc);
    uint32_t cur = get_reg(s, NVNET_RX_RING_NEXT_DESC_PHYS_ADDR);
    if (cur < base || cur + sizeof(struct RingDesc) > max) {
        cur = base;
    }
    set_reg(s, NVNET_RX_RING_CURRENT_DESC_PHYS_ADDR, cur);
    return cur;
}

static void advance_next_rx_ring_desc_addr(NvNet *s)
{
    const uint32_t base = get_reg(s, NVNET_RX_RING_PHYS_ADDR);
    const uint32_t max = base + get_rx_ring_size(s) * sizeof(struct RingDesc);
    uint32_t next = get_reg(s, NVNET_RX_RING_CURRENT_DESC_PHYS_ADDR) + sizeof(struct RingDesc);
    if (next >= max) {
        next = base;
    }
    set_reg(s, NVNET_RX_RING_NEXT_DESC_PHYS_ADDR, next);
}

static struct RingDesc load_ring_desc(NvNet *s, uint32_t addr)
{
    struct RingDesc d;
    dma_read(s, addr, &d, sizeof(d));
    return d;                           /* the Xbox and the host are both little-endian */
}

static void store_ring_desc(NvNet *s, uint32_t addr, struct RingDesc d)
{
    dma_write(s, addr, &d, sizeof(d));
}

static bool rx_buf_available(NvNet *s)
{
    return load_ring_desc(s, update_current_rx_ring_desc_addr(s)).flags & NV_RX_AVAIL;
}

static bool can_receive(NvNet *s)
{
    return rx_enabled(s) && dma_enabled(s) && link_up(s) && rx_buf_available(s);
}

/* A frame into the guest's next receive buffer: false when it has none. */
static bool dma_packet_to_guest(NvNet *s, const uint8_t *buf, size_t size)
{
    if (!can_receive(s)) {
        return false;
    }
    set_dma_idle(s, false);
    const uint32_t cur = update_current_rx_ring_desc_addr(s);
    struct RingDesc desc = load_ring_desc(s, cur);
    bool ok = false;
    if (desc.flags & NV_RX_AVAIL) {
        if ((size_t)desc.length + 1 < size) {
            size = (size_t)desc.length + 1;        /* xemu asserts; a short buffer gets what fits */
        }
        dma_write(s, desc.buffer_addr, buf, (uint32_t)size);
        desc.length = (uint16_t)size;
        desc.flags = NV_RX_BIT4 | NV_RX_DESCRIPTORVALID;
        store_ring_desc(s, cur, desc);
        set_intr_status(s, NVNET_IRQ_STATUS_RX);
        advance_next_rx_ring_desc_addr(s);
        ok = true;
    }
    set_dma_idle(s, true);
    return ok;
}

static bool tx_enabled(NvNet *s)
{
    return get_reg(s, NVNET_TRANSMITTER_CONTROL) & NVNET_TRANSMITTER_CONTROL_START;
}

static uint32_t update_current_tx_ring_desc_addr(NvNet *s)
{
    const uint32_t base = get_reg(s, NVNET_TX_RING_PHYS_ADDR);
    const uint32_t max = base + get_tx_ring_size(s) * sizeof(struct RingDesc);
    uint32_t cur = get_reg(s, NVNET_TX_RING_NEXT_DESC_PHYS_ADDR);
    if (cur < base || cur + sizeof(struct RingDesc) > max) {
        cur = base;
    }
    set_reg(s, NVNET_TX_RING_CURRENT_DESC_PHYS_ADDR, cur);
    return cur;
}

static void advance_next_tx_ring_desc_addr(NvNet *s)
{
    const uint32_t base = get_reg(s, NVNET_TX_RING_PHYS_ADDR);
    const uint32_t max = base + get_tx_ring_size(s) * sizeof(struct RingDesc);
    uint32_t next = get_reg(s, NVNET_TX_RING_CURRENT_DESC_PHYS_ADDR) + sizeof(struct RingDesc);
    if (next >= max) {
        next = base;
    }
    set_reg(s, NVNET_TX_RING_NEXT_DESC_PHYS_ADDR, next);
}

static void send_packet(NvNet *s, const uint8_t *buf, size_t size)
{
    s->tx_frames++;
    if (s->sock >= 0) {
        sendto(s->sock, buf, size, MSG_DONTWAIT, (const struct sockaddr *)&s->remote, sizeof(s->remote));
    }
}

static void dma_packet_from_guest(NvNet *s)
{
    s->kicks++;
    if (!(tx_enabled(s) && dma_enabled(s) && link_up(s))) {
        if (s->kicks_idle++ < 5) {
            nc_log("hakuX-native-nvnet", "transmit kicked, not sent: tx %d dma %d link %d (ctl %08x txc %08x)",
                   tx_enabled(s), dma_enabled(s), link_up(s), get_reg(s, NVNET_TX_RX_CONTROL),
                   get_reg(s, NVNET_TRANSMITTER_CONTROL));
        }
        return;
    }
    bool sent = false;
    set_dma_idle(s, false);
    for (int i = 0; i < get_tx_ring_size(s); i++) {
        const uint32_t cur = update_current_tx_ring_desc_addr(s);
        struct RingDesc desc = load_ring_desc(s, cur);
        const uint32_t length = (uint32_t)desc.length + 1;
        if (!(desc.flags & NV_TX_VALID)) {
            break;
        }
        if (s->tx_dma_buf_offset + length > sizeof(s->tx_dma_buf)) {
            s->tx_dma_buf_offset = 0;          /* xemu asserts: a runaway frame is dropped */
        }
        dma_read(s, desc.buffer_addr, &s->tx_dma_buf[s->tx_dma_buf_offset], length);
        s->tx_dma_buf_offset += length;
        const bool last = desc.flags & NV_TX_LASTPACKET;
        if (last) {
            send_packet(s, s->tx_dma_buf, s->tx_dma_buf_offset);
            s->tx_dma_buf_offset = 0;
            sent = true;
        }
        desc.flags &= ~(NV_TX_VALID | NV_TX_RETRYERROR | NV_TX_DEFERRED | NV_TX_CARRIERLOST | NV_TX_LATECOLLISION |
                        NV_TX_UNDERFLOW | NV_TX_ERROR);
        store_ring_desc(s, cur, desc);
        advance_next_tx_ring_desc_addr(s);
        if (last) {
            break;
        }
    }
    set_dma_idle(s, true);
    if (sent) {
        set_intr_status(s, NVNET_IRQ_STATUS_TX);
    }
}

static bool is_broadcast(const uint8_t *a)
{
    return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xFF;
}

static bool receive_filter(NvNet *s, const uint8_t *buf, size_t size)
{
    if (size < 6) {
        return false;                   /* the relay's control packets are 4 bytes */
    }
    const uint32_t rctl = get_reg(s, NVNET_PACKET_FILTER);
    if (is_broadcast(buf)) {
        return true;
    }
    if (!(rctl & NVNET_PACKET_FILTER_MYADDR)) {
        return true;
    }
    uint32_t addr[2] = {get_reg(s, NVNET_MULTICAST_ADDR_A), get_reg(s, NVNET_MULTICAST_ADDR_B)};
    if (!is_broadcast((const uint8_t *)addr)) {
        uint32_t dest[2] = {0, 0};
        memcpy(dest, buf, 6);
        dest[0] &= get_reg(s, NVNET_MULTICAST_MASK_A);
        dest[1] &= get_reg(s, NVNET_MULTICAST_MASK_B);
        if (!memcmp(dest, addr, 6)) {
            return true;
        }
    }
    addr[0] = get_reg(s, NVNET_MAC_ADDR_A);
    addr[1] = get_reg(s, NVNET_MAC_ADDR_B);
    return !memcmp(buf, addr, 6);
}

/* Frames waiting for a guest buffer, oldest first, while it has one. */
static void drain_rxq(NvNet *s)
{
    while (s->rxq_n) {
        Frame *f = &s->rxq[s->rxq_head];
        if (!dma_packet_to_guest(s, f->data, f->len)) {
            return;
        }
        s->rx_frames++;
        s->rxq_head = (s->rxq_head + 1) % RXQ_MAX;
        s->rxq_n--;
    }
}

static void receive(NvNet *s, const uint8_t *buf, size_t size)
{
    if (size > RX_ALLOC_BUFSIZE || !receive_filter(s, buf, size)) {
        return;
    }
    drain_rxq(s);
    if (!s->rxq_n && dma_packet_to_guest(s, buf, size)) {
        s->rx_frames++;
        return;
    }
    if (s->rxq_n == RXQ_MAX) {
        s->rxq_head = (s->rxq_head + 1) % RXQ_MAX;     /* the oldest goes, as a full NIC drops */
        s->rxq_n--;
        s->rx_dropped++;
    }
    Frame *f = &s->rxq[(s->rxq_head + s->rxq_n) % RXQ_MAX];
    f->len = (uint16_t)size;
    memcpy(f->data, buf, size);
    s->rxq_n++;
}

/* ----------------------------------------------------------------- PHY */

static void update_regs_on_link_up(NvNet *s)
{
    s->phy_regs[MII_BMSR] |= MII_BMSR_LINK_ST;
    or_reg(s, NVNET_ADAPTER_CONTROL, NVNET_ADAPTER_CONTROL_LINKUP);
}

static void update_regs_on_link_down(NvNet *s)
{
    s->phy_regs[MII_BMSR] &= ~(MII_BMSR_LINK_ST | MII_BMSR_AN_COMP);
    s->phy_regs[MII_ANLPAR] &= ~MII_ANLPAR_ACK;
    and_reg(s, NVNET_ADAPTER_CONTROL, ~NVNET_ADAPTER_CONTROL_LINKUP);
}

static void phy_reg_write(NvNet *s, uint8_t reg, uint16_t value)
{
    if (reg < sizeof(s->phy_regs) / sizeof(s->phy_regs[0])) {
        s->phy_regs[reg] = value;
    }
}

static void mdio_read(NvNet *s)
{
    const uint32_t a = get_reg(s, NVNET_MDIO_ADDR);
    const uint8_t phy = GET_MASK(a, NVNET_MDIO_ADDR_PHYADDR), reg = GET_MASK(a, NVNET_MDIO_ADDR_PHYREG);
    uint32_t data = (uint32_t)-1;
    if (phy == PHY_ADDR) {
        data = reg < sizeof(s->phy_regs) / sizeof(s->phy_regs[0]) ? s->phy_regs[reg] : 0;
    }
    set_reg(s, NVNET_MDIO_DATA, data);
    and_reg(s, NVNET_MDIO_ADDR, ~NVNET_MDIO_ADDR_INUSE);
}

static void mdio_write(NvNet *s)
{
    const uint32_t a = get_reg(s, NVNET_MDIO_ADDR);
    if (GET_MASK(a, NVNET_MDIO_ADDR_PHYADDR) == PHY_ADDR) {
        phy_reg_write(s, GET_MASK(a, NVNET_MDIO_ADDR_PHYREG), (uint16_t)get_reg(s, NVNET_MDIO_DATA));
    }
    and_reg(s, NVNET_MDIO_ADDR, ~NVNET_MDIO_ADDR_INUSE);
}

static void reset(NvNet *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    or_reg(s, NVNET_TX_RX_CONTROL, NVNET_TX_RX_CONTROL_IDLE);
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->phy_regs[MII_BMCR] = MII_BMCR_FD | MII_BMCR_AUTOEN;
    s->phy_regs[MII_BMSR] = MII_BMSR_AUTONEG | MII_BMSR_AN_COMP | MII_BMSR_LINK_ST;
    s->phy_regs[MII_ANAR] = s->phy_regs[MII_ANLPAR] =
        MII_ANLPAR_10 | MII_ANLPAR_10FD | MII_ANLPAR_TX | MII_ANLPAR_TXFD | MII_ANLPAR_T4;
    s->tx_dma_buf_offset = 0;
    /* negotiated already, as xemu's reset leaves it; no wire: the cable is out */
    if (s->sock >= 0) {
        update_regs_on_link_up(s);
    } else {
        update_regs_on_link_down(s);
    }
}

/* ---------------------------------------------------------------- MMIO */

static uint64_t nvnet_read(void *opaque, uint32_t off, unsigned size)
{
    NvNet *s = opaque;
    if (off + size > MMIO_SIZE || size > 4) {
        return 0;
    }
    pthread_mutex_lock(&s->lock);
    const uint32_t v = get_reg_ext(s, off, size);
    TRACE("rd%u %03x -> %08x", size, off, v);
    pthread_mutex_unlock(&s->lock);
    return v;
}

static void nvnet_write(void *opaque, uint32_t off, uint64_t val64, unsigned size)
{
    NvNet *s = opaque;
    if (off + size > MMIO_SIZE || size > 4) {
        return;
    }
    const uint32_t val = (uint32_t)val64;
    static bool started;
    if (!started) {
        started = true;
        nc_log("hakuX-native-nvnet", "the title's network stack drives the controller (%03x <- %08x)", off, val);
    }
    pthread_mutex_lock(&s->lock);
    TRACE("wr%u %03x <- %08x", size, off, val);
    switch (off) {
    case NVNET_MDIO_ADDR:
        set_reg_ext(s, off, val, size);
        if (val & NVNET_MDIO_ADDR_WRITE) {
            mdio_write(s);
        } else {
            mdio_read(s);
        }
        break;
    case NVNET_TX_RX_CONTROL:
        set_reg_with_mask(s, off, val, ~NVNET_TX_RX_CONTROL_IDLE);
        if (val & NVNET_TX_RX_CONTROL_KICK) {
            dma_packet_from_guest(s);
        }
        if (val & NVNET_TX_RX_CONTROL_RESET) {
            reset_descriptor_ring_pointers(s);
            s->tx_dma_buf_offset = 0;
        }
        if (val & NVNET_TX_RX_CONTROL_BIT1) {
            set_reg(s, NVNET_IRQ_STATUS, 0);
        } else if (val == 0) {
            set_reg(s, NVNET_UNKNOWN_SETUP_REG5, NVNET_UNKNOWN_SETUP_REG5_BIT31);
        }
        break;
    case NVNET_IRQ_STATUS:
    case NVNET_MII_STATUS:
        set_reg_ext(s, off, get_reg_ext(s, off, size) & ~val, size);
        update_irq(s);
        break;
    case NVNET_IRQ_MASK:
        set_reg_ext(s, off, val, size);
        update_irq(s);
        break;
    default:
        set_reg_ext(s, off, val, size);
        break;
    }
    /* the guest may have handed receive buffers back */
    drain_rxq(s);
    pthread_mutex_unlock(&s->lock);
}

/* ---------------------------------------------------------------- wire */

static bool parse_addr(const char *s, size_t n, struct sockaddr_in *out)
{
    char host[64];
    const char *colon = memchr(s, ':', n);
    if (!colon || (size_t)(colon - s) >= sizeof(host)) {
        return false;
    }
    memcpy(host, s, (size_t)(colon - s));
    host[colon - s] = 0;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)atoi(colon + 1));
    return inet_pton(AF_INET, host, &out->sin_addr) == 1;
}

static int open_wire(NvNet *s)
{
    const char *comma = strchr(g_lan, ',');
    struct sockaddr_in bind_addr;
    if (!comma || !parse_addr(g_lan, (size_t)(comma - g_lan), &bind_addr) ||
        !parse_addr(comma + 1, strlen(comma + 1), &s->remote)) {
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        nc_log("hakuX-native-nvnet", "can't bind %s: %s", g_lan, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void *rx_thread(void *arg)
{
    NvNet *s = arg;
    static uint8_t buf[RX_ALLOC_BUFSIZE + 64];
    uint64_t t_log = now_ns(), logged = 0;     /* the counters last logged (30 s apart, when they moved) */
    for (;;) {
        struct pollfd p = {s->sock, POLLIN, 0};
        const int r = poll(&p, 1, 2);       /* 2 ms: queued frames retried */
        pthread_mutex_lock(&s->lock);
        if (r > 0) {
            for (;;) {
                const ssize_t n = recv(s->sock, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0) {
                    break;
                }
                receive(s, buf, (size_t)n);
            }
        }
        drain_rxq(s);
        const uint64_t t = now_ns(), seen = s->tx_frames + s->rx_frames + s->rx_dropped;
        if (t - t_log >= 30000000000ull && seen != logged) {
            t_log = t;
            logged = seen;
            nc_log("hakuX-native-nvnet", "frames: %llu sent, %llu received, %llu dropped (no buffer); %llu kicks; "
                   "irq mask %08x status %08x, rx ctl %08x tx ctl %08x, txrx %08x, mac %08x%04x",
                   (unsigned long long)s->tx_frames, (unsigned long long)s->rx_frames,
                   (unsigned long long)s->rx_dropped, (unsigned long long)s->kicks, get_reg(s, NVNET_IRQ_MASK),
                   get_reg(s, NVNET_IRQ_STATUS), get_reg(s, NVNET_RECEIVER_CONTROL),
                   get_reg(s, NVNET_TRANSMITTER_CONTROL), get_reg(s, NVNET_TX_RX_CONTROL),
                   get_reg(s, NVNET_MAC_ADDR_A), get_reg(s, NVNET_MAC_ADDR_B) & 0xFFFF);
        }
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

void nc_dev_nvnet_set_lan(const char *spec)
{
    snprintf(g_lan, sizeof(g_lan), "%s", spec ? spec : "");
}

bool nc_dev_nvnet_link_up(void)
{
    return g_net.sock >= 0;
}

void nc_dev_nvnet_init(NcVm *vm)
{
    NvNet *s = &g_net;
    pthread_mutex_init(&s->lock, NULL);
    char v[92] = "";
#ifdef __ANDROID__
    __system_property_get("debug.hakux.nvnet.trace", v);
#endif
    const char *e = getenv("NC_NVNET_TRACE");
    g_trace = (v[0] == '1' || (e && *e == '1')) ? 4000 : 0;
    s->vm = vm;
    s->sock = g_lan[0] ? open_wire(s) : -1;
    reset(s);
    if (s->sock >= 0) {
        nc_log("hakuX-native-nvnet", "System Link: frames over %s", g_lan);
        pthread_t t;
        pthread_create(&t, NULL, rx_thread, s);
        pthread_detach(t);
    } else {
        nc_log("hakuX-native-nvnet", "no System Link: the network cable is out");
    }
    const NcMmioDev dev = {"nvnet", NVNET_BASE, MMIO_SIZE, nvnet_read, nvnet_write, s};
    nc_mmio_register(&dev);
}

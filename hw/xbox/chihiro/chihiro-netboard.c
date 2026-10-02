/*
 * Chihiro Type-3 network board (Sega 837-14341 / 171-8226C)
 *
 * The board is an AMD Alchemy Au1500 system on chip with 8 MB of SDRAM
 * (one Samsung K4S643232H), a 2 MB ST M29W160ET flash holding the
 * "Netfirm" firmware (VxWorks 5.5 for the pb1500 board support package),
 * an LSI L80227 Ethernet PHY on MAC0, a Microchip 24LC04 EEPROM hung on
 * two GPIO2 pins, and a PCI bus on which the media board answers as a
 * Sega device (vendor 0x11DB, device 0x18CB or 0x18D3). Addresses and
 * register layouts: the Au1500 Data Book (30361D), cross-checked against the
 * Linux and NetBSD Alchemy headers; their use, read in the firmware itself.
 *
 * The bridge is the part nobody documented. Its registers were read off
 * the firmware's PCI interrupt handler and command tasks: a status word
 * at BAR0+0x000 whose bits 16-18 the media board raises (write one to
 * clear), a mask at +0x004, a host-interrupt register at +0x040 (bit n
 * rings the Xbox) with its pending bits at +0x044, a 128 MB window onto
 * the DIMM memory at PCI 0x40000000 whose base is set at +0x060, and a
 * FIFO at BAR1. The command packet itself is not in the bridge: the media
 * board writes it, as a PCI master, into this board's SDRAM at 0x600200,
 * and reads the answer from 0x600000.
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "net/net.h"
#include "net/hub.h"
#include "chihiro-mips.h"
#include "chihiro-netboard.h"
#include "chihiro-log.h"

/* ── The board ───────────────────────────────────────────────────────── */

#define NB_RAM_SIZE     (8u << 20)          /* one K4S643232H */
#define NB_FLASH_SIZE   (2u << 20)          /* M29W160ET */
#define NB_FLASH_BASE   0x1FC00000u

/* Au1500-333: CPU = 12 MHz x cpupll; 28 gives 336 MHz, the nearest the
 * PLL can do. CP0 Count runs at the core clock (Data Book 2.7.8). */
#define NB_CPUPLL       28u
#define NB_CLOCKS_PER_US (12u * NB_CPUPLL)
#define NB_CLOCKS_PER_MS (NB_CLOCKS_PER_US * 1000u)
#define NB_SLICE_CLOCKS  (NB_CLOCKS_PER_MS / 10)
#define NB_MAX_ARREARS   ((int64_t)NB_CLOCKS_PER_MS * 200)
/* The TOY and RTC counters tick at 32.768 kHz (Data Book 3.4). */
#define NB_TOY_DIVIDER   (NB_CLOCKS_PER_US * 1000000ull / 32768ull)

/* SYS counter control (0x11900014), bit for bit as Linux names them
 * (arch/mips/include/asm/mach-au1x00/au1000.h): E0 (bit 8) turns the 32 kHz
 * oscillator on and 32S (bit 5) says it runs; twelve read-only bits say a
 * write to a counter register has not landed yet -- C0S, M00, M10, M20, T0S
 * and E0S for the TOY, C1S, M01, M11, M21, T1S and E1S for the RTC. The
 * firmware waits on 32S to rise and on C0S, M20 and E0S to fall. */
#define SYS_CNTRL_32S          (1u << 5)
#define SYS_CNTRL_E0           (1u << 8)
#define SYS_CNTRL_WRITE_STATUS 0x009F009Fu

/* Au1500 physical addresses (Data Book Appendix A) */
#define AU_IC0      0x10400000u
#define AU_UART0    0x11100000u
#define AU_UART3    0x11400000u
#define AU_MAC0     0x11500000u
#define AU_MAC1     0x11510000u
#define AU_MACEN    0x11520000u
#define AU_GPIO2    0x11700000u
#define AU_IC1      0x11800000u
#define AU_SYS      0x11900000u
#define AU_SDRAM    0x14000000u
#define AU_STATIC   0x14001000u
#define AU_DMA      0x14002000u
#define AU_MACDMA   0x14004000u
#define AU_PCI      0x14005000u
#define AU_PCI_MEM  0x400000000ull          /* 36-bit: PCI memory space */
#define AU_PCI_CFG  0x600000000ull

/* IC0 sources (Data Book table 6-2) */
#define IC0_UART0     0
#define IC0_PCI_INTA  1
#define IC0_UART3     3
#define IC0_TOY_MATCH2 17
#define IC0_RTC_MATCH2 21
#define IC0_MAC0_DMA  28

/* The bridge, as the firmware programs it */
/* 0x18CB is the Triforce's: the firmware byte-swaps for it */
#define BR_VENDOR_DEVICE  0x18D311DBu
#define BR_BAR0_DEFAULT   0x3FFFE000u
#define BR_BAR1_DEFAULT   0x3FFFF000u
#define BR_INT_CMD        0x00010000u       /* a command packet was deposited */
#define BR_WINDOW_SIZE    0x08000000u

typedef struct {
    uint32_t cfg0, cfg1, cfg2, src, assign, wake, mask;
    uint32_t level;      /* the lines as the devices hold them */
    uint32_t latched;    /* edges seen, until cleared */
} AuIntc;

typedef struct {
    uint32_t inten, fifoctrl, linectrl, mdmctrl, clkdiv, enable;
    char line[256];
    int  line_len;
    const char *name;
    int  irq;            /* its IC0 source */
} AuUart;

typedef struct {
    /* 24LC04 on GPIO2: SCL pin 7, SDA pin 6 (open drain, both sides) */
    uint8_t  mem[512];
    bool     scl, sda_host;   /* what the master drives */
    bool     sda_slave;       /* what we drive (false = pulling low) */
    int      bits;            /* bits clocked in the current byte */
    uint8_t  shift;
    enum {
        I2C_IDLE, I2C_ADDR, I2C_WORD, I2C_WRITE, I2C_READ, I2C_ACK_OUT,
        I2C_ACK_IN
    } state;
    enum { I2C_WORD_NEXT, I2C_WRITE_NEXT, I2C_READ_NEXT } after_ack;
    uint16_t addr;
    bool     started;
} AuEeprom;

typedef struct {
    uint32_t control, addr_high, addr_low, hash_high, hash_low;
    uint32_t mii_control, mii_data, flow, vlan1, vlan2;
    uint32_t tx_stat[4], tx_addr[4], tx_len[4];
    uint32_t rx_stat[4], rx_addr[4];
    uint16_t phy[32];
} AuMac;

typedef struct {
    MipsState cpu;
    uint8_t  *ram;
    uint8_t  *flash;
    uint32_t  flash_have;      /* bytes the image supplied */

    AuIntc   ic0, ic1;
    AuUart   uart0, uart3;
    AuEeprom eeprom;
    AuMac    mac0;
    uint32_t macen[2];
    uint32_t gpio2_dir, gpio2_out, gpio2_inten, gpio2_enable;
    uint32_t sys[0x80];        /* SYS block, word index */
    uint32_t sdram[0x10], staticbus[0x40], pcictl[0x60], dma[0x100];
    uint64_t toy_base_cycles;  /* TOYREAD = (cycles - base) / divider + toy_write */
    uint32_t toy_write, rtc_write;
    uint64_t rtc_base_cycles;
    uint32_t toy_last, rtc_last;

    /* The bridge */
    uint32_t br_cmd, br_bar0, br_bar1, br_cfg[0x40];
    uint32_t br_win_mask, br_win_base;
    uint32_t br_status, br_mask, br_r030, br_r034, br_host_pending, br_window;

    /* Thread */
    QemuThread thread;
    QemuMutex  lock;
    QemuCond   kick;
    bool       exiting, running;
    int64_t    last_us;
    uint64_t   paid;
    bool       said_illegal;
    bool       said_behind;
    void     (*host_interrupt)(void);

    /* MAC0 on xemu's hub 0: sent frames are queued here for the main loop;
     * received ones land in the RX descriptors. */
    NICState  *nic;
    NICConf    nic_conf;
    MemReentrancyGuard nic_guard;
    QEMUBH    *tx_bh, *rx_bh;
    struct { uint8_t data[2048]; int len; } tx_ring[8];
    unsigned   tx_head, tx_tail;
    unsigned   rx_next;
} NetBoard;

static NetBoard nb;
static bool nb_present;

/* ── Interrupt controllers ───────────────────────────────────────────── */

/* Recomputes the two request lines of a controller from its sources
 * (Data Book figure 6-1: type from cfg2:cfg1:cfg0, then mask, then the
 * assign bit picks request 0 or 1). */
static uint32_t intc_requests(const AuIntc *ic, bool req0)
{
    uint32_t active = 0;
    for (int n = 0; n < 32; n++) {
        uint32_t b = 1u << n;
        uint32_t c2 = ic->cfg2 & b, c1 = ic->cfg1 & b, c0 = ic->cfg0 & b;
        bool on;
        if (c2) {
            /* level: 1:0:1 high, 1:1:0 low */
            bool lvl = (ic->level & b) != 0;
            on = c0 ? lvl : (c1 ? !lvl : false);
        } else if (c1 || c0) {
            on = (ic->latched & b) != 0;      /* edge, held until cleared */
        } else {
            on = false;                        /* 0:0:0 disabled */
        }
        if (on) active |= b;
    }
    active &= ic->mask;
    return req0 ? (active & ic->assign) : (active & ~ic->assign);
}

static void intc_update(void)
{
    uint32_t ip = 0;
    if (intc_requests(&nb.ic0, true))  ip |= 1u << 10;   /* IP2 */
    if (intc_requests(&nb.ic0, false)) ip |= 1u << 11;   /* IP3 */
    if (intc_requests(&nb.ic1, true))  ip |= 1u << 12;   /* IP4 */
    if (intc_requests(&nb.ic1, false)) ip |= 1u << 13;   /* IP5 */
    mips_set_irq(&nb.cpu, 0x3Cu << 8, false);
    mips_set_irq(&nb.cpu, ip, true);
}

/* A device line: level for level sources, an edge for the latch. */
static void intc_set_line(AuIntc *ic, int n, bool level)
{
    uint32_t b = 1u << n;
    bool was = (ic->level & b) != 0;
    if (level && !was && (ic->cfg0 & b))
        ic->latched |= b;                      /* rising edge (0:0:1, 0:1:1) */
    if (!level && was && (ic->cfg1 & b) && !(ic->cfg2 & b))
        ic->latched |= b;                      /* falling edge (0:1:0, 0:1:1) */
    if (level) ic->level |= b; else ic->level &= ~b;
    intc_update();
}

static uint32_t intc_read(AuIntc *ic, uint32_t off)
{
    switch (off) {
    case 0x40: return ic->cfg0;
    case 0x48: return ic->cfg1;
    case 0x50: return ic->cfg2;
    case 0x54: return intc_requests(ic, true);
    case 0x58: return ic->src;
    case 0x5C: return intc_requests(ic, false);
    case 0x60: return ic->assign;
    case 0x68: return ic->wake;
    case 0x70: return ic->mask;
    case 0x78: return ic->latched;             /* risingrd */
    case 0x7C: return ic->latched;             /* fallingrd */
    default:   return 0;
    }
}

static void nb_wake_board(void);

static void intc_write(AuIntc *ic, uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x40: ic->cfg0 |= v; break;
    case 0x44: ic->cfg0 &= ~v; break;
    case 0x48: ic->cfg1 |= v; break;
    case 0x4C: ic->cfg1 &= ~v; break;
    case 0x50: ic->cfg2 |= v; break;
    case 0x54: ic->cfg2 &= ~v; break;
    case 0x58: ic->src |= v; break;
    case 0x5C: ic->src &= ~v; break;
    case 0x60: ic->assign |= v; break;
    case 0x64: ic->assign &= ~v; break;
    case 0x68: ic->wake |= v; break;
    case 0x6C: ic->wake &= ~v; break;
    case 0x70: ic->mask |= v; break;
    case 0x74: ic->mask &= ~v; break;
    case 0x78: ic->latched &= ~v; break;       /* risingclr */
    case 0x7C: ic->latched &= ~v; break;       /* fallingclr */
    case 0x80: break;                          /* testbit */
    default: break;
    }
    intc_update();
}

/* ── UARTs ───────────────────────────────────────────────────────────── */

static void uart_putc(AuUart *u, uint8_t c)
{
    if (c == '\r') return;
    if (c == '\n' || u->line_len >= (int)sizeof(u->line) - 1) {
        u->line[u->line_len] = '\0';
        CHIHIRO_LOGF(NET, "netboard %s: %s\n", u->name, u->line);
        u->line_len = 0;
        return;
    }
    u->line[u->line_len++] = (char)c;
}

static uint32_t uart_read(AuUart *u, uint32_t off)
{
    switch (off) {
    case 0x00: return 0;                       /* rxdata: nothing typed */
    case 0x08: return u->inten;
    case 0x0C:
        /* The transmitter is always ready: with TIE that is the pending
         * cause (IID 1), otherwise nothing is pending (IP set). */
        return (u->inten & 2) ? 0x2 : 0x1;
    case 0x10: return u->fifoctrl;
    case 0x14: return u->linectrl;
    case 0x18: return u->mdmctrl;
    case 0x1C: return 0x60;                    /* linestat: TX FIFO and shifter empty */
    case 0x20: return 0;                       /* mdmstat */
    case 0x28: return u->clkdiv;
    case 0x100: return u->enable;
    default:   return 0;
    }
}

static void uart_write(AuUart *u, uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x04: uart_putc(u, v & 0xFF); break;
    case 0x08: u->inten = v; intc_set_line(&nb.ic0, u->irq, (v & 2) != 0); break;
    case 0x10: u->fifoctrl = v; break;
    case 0x14: u->linectrl = v; break;
    case 0x18: u->mdmctrl = v; break;
    case 0x28: u->clkdiv = v; break;
    case 0x100: u->enable = v; break;
    default: break;
    }
}

/* ── 24LC04 on GPIO2 pins 6 (SDA) and 7 (SCL) ───────────────────────── */

static void eeprom_sample(AuEeprom *e, bool scl, bool sda)
{
    bool scl_was = e->scl, sda_was = e->sda_host;
    e->scl = scl;
    e->sda_host = sda;
    /* START: SDA falls while SCL high. STOP: SDA rises while SCL high. */
    if (scl && scl_was) {
        if (sda_was && !sda) {
            e->state = I2C_ADDR; e->bits = 0; e->shift = 0; e->started = true;
            e->sda_slave = true;
            return;
        }
        if (!sda_was && sda) {
            e->state = I2C_IDLE; e->started = false; e->sda_slave = true;
            return;
        }
    }
    if (!e->started) return;
    if (scl && !scl_was) {
        /* rising SCL: the master's bit is valid, or we hold the line */
        switch (e->state) {
        case I2C_ADDR:
        case I2C_WORD:
        case I2C_WRITE:
            e->shift = (e->shift << 1) | (sda ? 1 : 0);
            if (++e->bits == 8) {
                if (e->state == I2C_ADDR) {
                    /* 1010 A2 A1 A0 R/W: the 24LC04 uses A0 as block select */
                    if ((e->shift & 0xF0) != 0xA0) {
                        e->state = I2C_IDLE;
                        e->started = false;
                        break;
                    }
                    e->addr = (e->addr & 0xFF) | ((e->shift & 0x02) ? 0x100 : 0);
                    /* R/W set: data out from the current address; clear: a
                     * word address follows before any data */
                    e->after_ack = (e->shift & 1) ? I2C_READ_NEXT : I2C_WORD_NEXT;
                } else if (e->state == I2C_WORD) {
                    e->addr = (e->addr & 0x100) | e->shift;
                    e->after_ack = I2C_WRITE_NEXT;
                } else {
                    e->mem[e->addr & 0x1FF] = e->shift;
                    e->addr = (e->addr & 0x1F0) | ((e->addr + 1) & 0x0F);
                    e->after_ack = I2C_WRITE_NEXT;
                }
                e->state = I2C_ACK_OUT;
                e->bits = 0;
            }
            break;
        case I2C_ACK_IN:
            /* the master's ACK (low) after a byte we sent: more to read */
            if (!sda) {
                e->state = I2C_READ;
                e->bits = 0;
                e->shift = e->mem[e->addr & 0x1FF];
                e->addr++;
            }
            else { e->state = I2C_IDLE; }
            break;
        default:
            break;
        }
    } else if (!scl && scl_was) {
        /* falling SCL: we set up what the master will see on the next rise */
        switch (e->state) {
        case I2C_ACK_OUT:
            e->sda_slave = false;                 /* ACK: pull low */
            e->state = (e->after_ack == I2C_READ_NEXT) ? I2C_READ :
                       (e->after_ack == I2C_WRITE_NEXT) ? I2C_WRITE : I2C_WORD;
            if (e->state == I2C_READ) {
                e->shift = e->mem[e->addr & 0x1FF];
                e->addr++;
                e->bits = -1;
            }
            else e->bits = -1;                    /* the ACK clock comes first */
            break;
        case I2C_READ:
            if (e->bits < 0) { e->bits = 0; }     /* ACK clock done */
            if (e->bits < 8) {
                e->sda_slave = (e->shift & 0x80) != 0;
                e->shift <<= 1;
                e->bits++;
                if (e->bits == 8) { e->state = I2C_ACK_IN; }
            }
            break;
        case I2C_WRITE:
        case I2C_WORD:
            /* release after ACK */
            if (e->bits < 0) { e->bits = 0; e->sda_slave = true; }
            break;
        default:
            e->sda_slave = true;
            break;
        }
    }
}

static bool eeprom_sda_level(const AuEeprom *e)
{
    return e->sda_host && e->sda_slave;         /* wired-AND */
}

/* ── GPIO2 ───────────────────────────────────────────────────────────── */

#define GPIO2_BOARD_READY (1u << 1)            /* the media board's line */
#define GPIO2_SDA         (1u << 6)
#define GPIO2_SCL         (1u << 7)

static uint32_t gpio2_pinstate(void)
{
    uint32_t v = nb.gpio2_out & nb.gpio2_dir;   /* outputs read back */
    v |= GPIO2_BOARD_READY;
    /* An input, or an output left high, reads the wire: open drain. */
    bool scl = !((nb.gpio2_dir & GPIO2_SCL) && !(nb.gpio2_out & GPIO2_SCL));
    bool sda = eeprom_sda_level(&nb.eeprom);
    v = (v & ~(GPIO2_SCL | GPIO2_SDA)) | (scl ? GPIO2_SCL : 0) | (sda ? GPIO2_SDA : 0);
    return v;
}

static void gpio2_changed(void)
{
    bool scl = !((nb.gpio2_dir & GPIO2_SCL) && !(nb.gpio2_out & GPIO2_SCL));
    bool sda = !((nb.gpio2_dir & GPIO2_SDA) && !(nb.gpio2_out & GPIO2_SDA));
    eeprom_sample(&nb.eeprom, scl, sda);
}

static uint32_t gpio2_read(uint32_t off)
{
    switch (off) {
    case 0x00: return nb.gpio2_dir;
    case 0x08: return nb.gpio2_out;
    case 0x0C: return gpio2_pinstate();
    case 0x10: return nb.gpio2_inten;
    case 0x14: return nb.gpio2_enable;
    default:   return 0;
    }
}

static void gpio2_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00: nb.gpio2_dir = v; gpio2_changed(); break;
    case 0x08: {
        /* bits 31:16 say which pins the write touches, 15:0 the values */
        uint32_t en = v >> 16, val = v & 0xFFFF;
        nb.gpio2_out = (nb.gpio2_out & ~en) | (val & en);
        gpio2_changed();
        break; }
    case 0x10: nb.gpio2_inten = v; break;
    case 0x14: nb.gpio2_enable = v; break;
    default: break;
    }
}

/* ── MAC0 and its PHY ────────────────────────────────────────────────── */

static void phy_reset(AuMac *m)
{
    memset(m->phy, 0, sizeof(m->phy));
    m->phy[0] = 0x3100;    /* BMCR: autoneg, 100 Mbit, full duplex */
    m->phy[1] = 0x782D;    /* BMSR: 100/10 FD/HD, autoneg done, link up */
    m->phy[2] = 0x0016;    /* ID: LSI L80227 */
    m->phy[3] = 0xF870;
    m->phy[4] = 0x01E1;    /* advertise 100/10 FD/HD */
    m->phy[5] = 0x45E1;    /* partner: the same */
    m->phy[6] = 0x0001;
}

static uint32_t mac_read(AuMac *m, uint32_t off)
{
    switch (off) {
    case 0x00: return m->control;
    case 0x04: return m->addr_high;
    case 0x08: return m->addr_low;
    case 0x0C: return m->hash_high;
    case 0x10: return m->hash_low;
    case 0x14: return m->mii_control & ~1u;    /* never busy */
    case 0x18: return m->mii_data;
    case 0x1C: return m->flow & ~1u;
    case 0x20: return m->vlan1;
    case 0x24: return m->vlan2;
    default:   return 0;
    }
}

static void mac_write(AuMac *m, uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00: m->control = v; break;
    case 0x04: m->addr_high = v; break;
    case 0x08: m->addr_low = v; break;
    case 0x0C: m->hash_high = v; break;
    case 0x10: m->hash_low = v; break;
    case 0x14: {
        int reg = (v >> 6) & 31;
        m->mii_control = v;
        if (v & 2) m->phy[reg] = m->mii_data & 0xFFFF;   /* write */
        else m->mii_data = m->phy[reg];                  /* read */
        if (reg == 0 && (v & 2) && (m->phy[0] & 0x8000))
            phy_reset(m);                                /* BMCR reset */
        break; }
    case 0x18: m->mii_data = v & 0xFFFF; break;
    case 0x1C: m->flow = v; break;
    case 0x20: m->vlan1 = v; break;
    case 0x24: m->vlan2 = v; break;
    default: break;
    }
}

static uint32_t macdma_read(uint32_t off)
{
    if (off < 0x100) {
        int i = (off >> 4) & 3;
        switch (off & 0xC) {
        case 0x0: return nb.mac0.tx_stat[i];
        case 0x4: return nb.mac0.tx_addr[i];
        case 0x8: return nb.mac0.tx_len[i];
        }
    } else if (off < 0x200) {
        int i = (off >> 4) & 3;
        switch (off & 0xC) {
        case 0x0: return nb.mac0.rx_stat[i];
        case 0x4: return nb.mac0.rx_addr[i];
        }
    }
    return 0;
}

/* MAC0 DMA done (IC0 source 28) is up while any descriptor has its DN bit;
 * the driver clears the bit when it has seen it. */
static void macdma_update_irq(void)
{
    bool done = false;
    for (int i = 0; i < 4; i++)
        if ((nb.mac0.tx_addr[i] & 2) || (nb.mac0.rx_addr[i] & 2)) done = true;
    intc_set_line(&nb.ic0, IC0_MAC0_DMA, done);
}

static void macdma_write(uint32_t off, uint32_t v)
{
    if (off < 0x100) {
        int i = (off >> 4) & 3;
        switch (off & 0xC) {
        case 0x0: nb.mac0.tx_stat[i] = v; break;
        case 0x4:
            nb.mac0.tx_addr[i] = v;
            if (v & 1) {
                /* Buffer at the address (bits 31:2), length in len; queued
                 * for the main loop, the descriptor completes at once. */
                uint32_t addr = v & ~3u, len = nb.mac0.tx_len[i] & 0x3FFF;
                unsigned slot = nb.tx_head & 7;
                if (len && len <= 2048 && addr <= NB_RAM_SIZE - len &&
                    nb.tx_head - nb.tx_tail < 8) {
                    memcpy(nb.tx_ring[slot].data, nb.ram + addr, len);
                    nb.tx_ring[slot].len = len;
                    nb.tx_head++;
                    if (nb.tx_bh) qemu_bh_schedule(nb.tx_bh);
                }
                nb.mac0.tx_addr[i] = (v & ~1u) | 2u;    /* done */
                nb.mac0.tx_stat[i] = 0;
            }
            break;
        case 0x8: nb.mac0.tx_len[i] = v; break;
        }
    } else if (off < 0x200) {
        int i = (off >> 4) & 3;
        switch (off & 0xC) {
        case 0x0: nb.mac0.rx_stat[i] = v; break;
        case 0x4:
            nb.mac0.rx_addr[i] = v;
            /* A buffer handed back: frames the hub queued while none was
             * free are delivered from the main loop. */
            if ((v & 1) && nb.rx_bh) qemu_bh_schedule(nb.rx_bh);
            break;
        }
    }
    macdma_update_irq();
}

/* ── SYS: clocks, TOY and RTC counters ──────────────────────────────── */

static uint32_t sys_toy(void)
{
    return nb.toy_write +
           (uint32_t)((nb.cpu.cycles - nb.toy_base_cycles) / NB_TOY_DIVIDER);
}

static uint32_t sys_rtc(void)
{
    return nb.rtc_write +
           (uint32_t)((nb.cpu.cycles - nb.rtc_base_cycles) / NB_TOY_DIVIDER);
}

static uint32_t sys_read(uint32_t off)
{
    switch (off) {
    case 0x14: {
        /* E0 starts the 32 kHz crystal and 32S says it runs (the firmware
         * polls it: 0x80019F30 from 0x8001A030). It settles in milliseconds
         * and our counter writes land at once: 32S follows E0, and the
         * write-status bits always read settled. */
        uint32_t v = nb.sys[0x14 / 4] & ~(SYS_CNTRL_32S | SYS_CNTRL_WRITE_STATUS);
        if (v & SYS_CNTRL_E0)
            v |= SYS_CNTRL_32S;
        return v;
    }
    case 0x38: return 1;                          /* endian: little */
    case 0x40: return sys_toy();
    case 0x58: return sys_rtc();
    case 0x60: return NB_CPUPLL;
    case 0x64: return nb.sys[0x64 / 4] ? nb.sys[0x64 / 4] : 8;
    default:
        if (off < sizeof(nb.sys)) return nb.sys[off / 4];
        return 0;
    }
}

static void sys_write(uint32_t off, uint32_t v)
{
    if (off < sizeof(nb.sys)) nb.sys[off / 4] = v;
    switch (off) {
    case 0x04: nb.toy_write = v; nb.toy_base_cycles = nb.cpu.cycles; break;
    case 0x48: nb.rtc_write = v; nb.rtc_base_cycles = nb.cpu.cycles; break;
    default: break;
    }
}

/* The match registers fire an edge on their interrupt when the counter
 * reaches them. Polled between slices, which is finer than a 32 kHz tick. */
static void sys_service(void)
{
    uint32_t toy = sys_toy(), rtc = sys_rtc();
    uint32_t m2 = nb.sys[0x10 / 4], r2 = nb.sys[0x54 / 4];
    if (toy != nb.toy_last) {
        if ((int32_t)(toy - m2) >= 0 && (int32_t)(nb.toy_last - m2) < 0) {
            intc_set_line(&nb.ic0, IC0_TOY_MATCH2, true);
            intc_set_line(&nb.ic0, IC0_TOY_MATCH2, false);
        }
        nb.toy_last = toy;
    }
    if (rtc != nb.rtc_last) {
        if ((int32_t)(rtc - r2) >= 0 && (int32_t)(nb.rtc_last - r2) < 0) {
            intc_set_line(&nb.ic0, IC0_RTC_MATCH2, true);
            intc_set_line(&nb.ic0, IC0_RTC_MATCH2, false);
        }
        nb.rtc_last = rtc;
    }
}

/* ── The PCI side: host controller, configuration space, the bridge ── */

static uint32_t pcictl_read(uint32_t off)
{
    switch (off) {
    case 0x04: return nb.pcictl[1] & ~0x0FC00000u;   /* config: no error bits */
    case 0x100: return 0x11291000u;                  /* the Au1500's own ID */
    default:
        if (off < sizeof(nb.pcictl)) return nb.pcictl[off / 4];
        return 0;
    }
}

static void pcictl_write(uint32_t off, uint32_t v)
{
    if (off < sizeof(nb.pcictl)) nb.pcictl[off / 4] = v;
}

/* Type 0 configuration: AD24 is the bridge's IDSEL, the only device. */
static uint32_t pcicfg_read(uint32_t bus_addr)
{
    if (!(bus_addr & (1u << 24))) return 0xFFFFFFFFu;
    uint32_t reg = bus_addr & 0xFC;
    switch (reg) {
    case 0x00: return BR_VENDOR_DEVICE;
    case 0x04: return nb.br_cmd | (0x0200u << 16);
    case 0x08: return 0x06800000u;                   /* class: bridge, other */
    case 0x0C: return nb.br_cfg[3];
    case 0x10: return nb.br_bar0;
    case 0x14: return nb.br_bar1;
    case 0x40: return nb.br_win_mask;
    case 0x44: return nb.br_win_base;
    default:   return nb.br_cfg[reg / 4];
    }
}

/* Configuration writes come as a word, a halfword or a byte; the smaller
 * ones change only their part of the register. */
static void pcicfg_write(uint32_t bus_addr, uint32_t val, int size)
{
    if (!(bus_addr & (1u << 24))) return;
    uint32_t reg = bus_addr & 0xFC;
    uint32_t v = val;

    if (size < 4) {
        int shift = (bus_addr & 3) * 8;
        uint32_t part = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        v = (pcicfg_read(bus_addr & ~3u) & ~part) | ((val << shift) & part);
    }
    switch (reg) {
    case 0x04: nb.br_cmd = v & 0xFFFF; break;           /* status is read-only */
    case 0x10: nb.br_bar0 = v & ~0xFFFu; break;
    case 0x14: nb.br_bar1 = v & ~0xFFFu; break;
    case 0x40:
        /* The window's size register: reading back after writing zero
         * gives ~(size - 1), the way a BAR does; the firmware then writes
         * the mask it was told. */
        nb.br_win_mask = v ? v : ~(BR_WINDOW_SIZE - 1);
        break;
    case 0x44: nb.br_win_base = v; break;
    default: nb.br_cfg[reg / 4] = v; break;
    }
}

static uint32_t bridge_read(uint32_t off)
{
    switch (off) {
    case 0x000: return nb.br_status;
    case 0x004: return nb.br_mask;
    case 0x030: return nb.br_r030;
    case 0x034: return nb.br_r034;
    case 0x040: return 0;
    case 0x044: return nb.br_host_pending;
    case 0x060: return nb.br_window;
    default:    return 0;
    }
}

/* PCI_INTA# is a low-level line (Data Book table 6-2): high at rest. */
static void bridge_update_irq(void)
{
    intc_set_line(&nb.ic0, IC0_PCI_INTA, (nb.br_status & nb.br_mask) == 0);
}

static void bridge_write(uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x000: nb.br_status &= ~v; bridge_update_irq(); break;
    case 0x004: nb.br_mask = v; bridge_update_irq(); break;
    case 0x030: nb.br_r030 = v; break;
    case 0x034: nb.br_r034 = v; break;
    case 0x040:
        nb.br_host_pending |= v;
        if (chihiro_log_mask & CHIHIRO_LOG_NET) {
            const uint8_t *p = nb.ram + 0x600000;
            unsigned cmd = (p[3] << 8) | p[2];
            if (cmd != 0x8607 && cmd != 0x8001) {    /* not the heartbeats */
                CHIHIRO_LOGF(NET, "netboard: board resp %04X seq %02X%02X args "
                             "%02X%02X%02X%02X %02X%02X%02X%02X\n", cmd, p[1], p[0],
                             p[7], p[6], p[5], p[4], p[11], p[10], p[9], p[8]);
            }
        }
        if (nb.host_interrupt) nb.host_interrupt();
        break;
    case 0x060: nb.br_window = v; break;
    default: break;
    }
}

/* ── The bus ─────────────────────────────────────────────────────────── */

static uint32_t nb_read(void *opaque, uint64_t pa, int size)
{
    uint32_t v;

    if (pa >= NB_FLASH_BASE && pa < NB_FLASH_BASE + NB_FLASH_SIZE) {
        return ldn_le_p(nb.flash + (pa - NB_FLASH_BASE), size);
    }
    if (pa >= AU_PCI_CFG && pa < AU_PCI_CFG + 0x100000000ull)
        return pcicfg_read((uint32_t)pa) >> ((pa & 3) * 8);
    if (pa >= AU_PCI_MEM && pa < AU_PCI_MEM + 0x100000000ull) {
        uint32_t bus = (uint32_t)pa;
        if (bus >= nb.br_bar0 && bus < nb.br_bar0 + 0x1000)
            return bridge_read(bus - nb.br_bar0);
        if (bus >= nb.br_bar1 && bus < nb.br_bar1 + 0x1000)
            /* the FIFO: nothing to read yet */
            return 0;
        /* the DIMM window: not modelled */
        return 0xFFFFFFFFu;
    }
    if (pa >= 0x100000000ull) return 0xFFFFFFFFu;

    uint32_t a = (uint32_t)pa;
    uint32_t off = a & 0xFFFC;                 /* registers are words */
    switch (a & 0xFFFF0000u) {
    case AU_IC0:    v = intc_read(&nb.ic0, off); break;
    case AU_IC1:    v = intc_read(&nb.ic1, off); break;
    case AU_UART0:  v = uart_read(&nb.uart0, off); break;
    case AU_UART3:  v = uart_read(&nb.uart3, off); break;
    case AU_MAC0:   v = mac_read(&nb.mac0, off); break;
    case AU_MAC1:   v = 0; break;
    case AU_MACEN:  v = off < 8 ? nb.macen[off / 4] : 0; break;
    case AU_GPIO2:  v = gpio2_read(off); break;
    case AU_SYS:    v = sys_read(off); break;
    case AU_SDRAM:                             /* 4 KB controller blocks */
        off &= 0xFFC;
        switch (a & 0xFFFFF000u) {
        case AU_SDRAM:  v = off < sizeof(nb.sdram) ? nb.sdram[off / 4] : 0; break;
        case AU_STATIC: v = off < sizeof(nb.staticbus) ? nb.staticbus[off / 4] : 0; break;
        case AU_DMA:    v = off < sizeof(nb.dma) ? nb.dma[off / 4] : 0; break;
        case AU_MACDMA: v = macdma_read(off); break;
        case AU_PCI:    v = pcictl_read(off); break;
        default:        v = 0; break;
        }
        break;
    default:
        /* Pb1500 board registers the BSP still pokes (0xAE000000 ...) and
         * anything else the Sega board does not have: an open bus. */
        v = 0;
        break;
    }
    return v >> ((a & 3) * 8);
}

static void nb_write(void *opaque, uint64_t pa, int size, uint32_t val)
{
    if (pa >= NB_FLASH_BASE && pa < NB_FLASH_BASE + NB_FLASH_SIZE) {
        /* Flash commands (the firmware can reprogram itself): not done. */
        return;
    }
    if (pa >= AU_PCI_CFG && pa < AU_PCI_CFG + 0x100000000ull) {
        pcicfg_write((uint32_t)pa, val, size);
        return;
    }
    if (pa >= AU_PCI_MEM && pa < AU_PCI_MEM + 0x100000000ull) {
        uint32_t bus = (uint32_t)pa;
        if (bus >= nb.br_bar0 && bus < nb.br_bar0 + 0x1000)
            bridge_write(bus - nb.br_bar0, val);
        /* The FIFO behind BAR1 and the DIMM window: not modelled. */
        return;
    }
    if (pa >= 0x100000000ull) return;

    uint32_t a = (uint32_t)pa;
    uint32_t off = a & 0xFFFC;
    val <<= (a & 3) * 8;                       /* a byte lands in its lane */
    switch (a & 0xFFFF0000u) {
    case AU_IC0:    intc_write(&nb.ic0, off, val); break;
    case AU_IC1:    intc_write(&nb.ic1, off, val); break;
    case AU_UART0:  uart_write(&nb.uart0, off, val); break;
    case AU_UART3:  uart_write(&nb.uart3, off, val); break;
    case AU_MAC0:   mac_write(&nb.mac0, off, val); break;
    case AU_MAC1:   break;
    case AU_MACEN:  if (off < 8) nb.macen[off / 4] = val; break;
    case AU_GPIO2:  gpio2_write(off, val); break;
    case AU_SYS:    sys_write(off, val); break;
    case AU_SDRAM:                             /* 4 KB controller blocks */
        off &= 0xFFC;
        switch (a & 0xFFFFF000u) {
        case AU_SDRAM:  if (off < sizeof(nb.sdram)) nb.sdram[off / 4] = val; break;
        case AU_STATIC:
            if (off < sizeof(nb.staticbus)) nb.staticbus[off / 4] = val;
            break;
        case AU_DMA:    if (off < sizeof(nb.dma)) nb.dma[off / 4] = val; break;
        case AU_MACDMA: macdma_write(off, val); break;
        case AU_PCI:    pcictl_write(off, val); break;
        default:        break;
        }
        break;
    default:
        break;
    }
}

/* ── The wire ────────────────────────────────────────────────────────── */

static void nb_tx_bh(void *opaque)
{
    for (;;) {
        qemu_mutex_lock(&nb.lock);
        if (nb.tx_tail == nb.tx_head) { qemu_mutex_unlock(&nb.lock); return; }
        unsigned slot = nb.tx_tail & 7;
        uint8_t frame[2048];
        int len = nb.tx_ring[slot].len;
        memcpy(frame, nb.tx_ring[slot].data, len);
        nb.tx_tail++;
        qemu_mutex_unlock(&nb.lock);
        if (nb.nic)
            qemu_send_packet(qemu_get_queue(nb.nic), frame, len);
    }
}

static void nb_rx_bh(void *opaque)
{
    if (nb.nic) qemu_flush_queued_packets(qemu_get_queue(nb.nic));
}

static bool nb_can_receive(NetClientState *nc)
{
    /* A board still booting drops the frame; its arrival starts the firmware. */
    if (!nb.running) return true;
    for (int i = 0; i < 4; i++)
        if ((nb.mac0.rx_addr[i] & 3) == 1) return true;
    return false;
}

/* The MAC's address filter (Data Book 8.4.2): its own station address
 * from MAC_ADDRHIGH/LOW, every multicast and broadcast frame, or all of
 * them in promiscuous mode. The multicast hash is not applied: a superset. */
static bool mac_accepts(const AuMac *m, const uint8_t *frame)
{
    if (m->control & (1u << 4))                         /* PR: promiscuous */
        return true;
    if (frame[0] & 1)                                   /* multicast, broadcast */
        return true;
    uint8_t mac[6];
    for (int i = 0; i < 4; i++) mac[i] = m->addr_low >> (8 * i);
    mac[4] = m->addr_high;
    mac[5] = m->addr_high >> 8;
    return memcmp(frame, mac, 6) == 0;
}

/* The next receive descriptor the driver has enabled, or -1. */
static int rx_free_descriptor(void)
{
    for (int k = 0; k < 4; k++) {
        int i = (nb.rx_next + k) & 3;
        if ((nb.mac0.rx_addr[i] & 3) == 1) return i;
    }
    return -1;
}

static ssize_t nb_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    /* Shorter than an Ethernet header: nothing the MAC would take. */
    if (size < 14) {
        return size;
    }
    /* Another cabinet is calling: a real board is always listening, so this
     * is a solicitation like any other. */
    nb_wake_board();
    qemu_mutex_lock(&nb.lock);

    bool receiving = (nb.mac0.control & (1u << 2)) != 0;   /* RE */
    if (!receiving || !mac_accepts(&nb.mac0, buf)) {
        qemu_mutex_unlock(&nb.lock);
        /* dropped, as the silicon drops it */
        return size;
    }
    int i = rx_free_descriptor();
    if (i < 0) {
        /* No buffer yet: the frame waits in the hub's queue until a
         * descriptor write flushes it. */
        qemu_mutex_unlock(&nb.lock);
        return 0;
    }
    uint32_t d = nb.mac0.rx_addr[i];
    uint32_t addr = d & ~3u;
    size_t len = MIN(size, (size_t)2044);
    if (addr <= NB_RAM_SIZE - 4 - len) {
        memcpy(nb.ram + addr, buf, len);
        memset(nb.ram + addr + len, 0, 4);              /* the FCS the MAC would keep */
        /* PF, Ethernet */
        nb.mac0.rx_stat[i] = (uint32_t)(len + 4) | (1u << 30) | (1u << 18);
        if (buf[0] & 1) nb.mac0.rx_stat[i] |= (buf[0] == 0xFF) ? (1u << 28) : (1u << 27);
        nb.mac0.rx_addr[i] = (d & ~1u) | 2u;            /* done, not enabled */
        nb.rx_next = (i + 1) & 3;
        macdma_update_irq();
        qemu_cond_signal(&nb.kick);
    }
    qemu_mutex_unlock(&nb.lock);
    return size;
}

static NetClientInfo nb_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = nb_can_receive,
    .receive = nb_receive,
};

/* Puts MAC0 on hub 0, the hub xemu's Network menu feeds. Main loop only. */
void chihiro_netboard_attach_net(void)
{
    if (!nb_present || nb.nic) return;
    NetClientState *port = net_hub_add_port(0, "chihiro-netboard", NULL);
    memset(&nb.nic_conf, 0, sizeof(nb.nic_conf));
    memcpy(nb.nic_conf.macaddr.a, nb.eeprom.mem + 0x10, 6);
    nb.nic_conf.peers.ncs[0] = port;
    nb.nic_conf.peers.queues = 1;
    nb.nic = qemu_new_nic(&nb_net_info, &nb.nic_conf, "chihiro-netboard",
                          "netboard", &nb.nic_guard, &nb);
    nb.tx_bh = qemu_bh_new(nb_tx_bh, NULL);
    nb.rx_bh = qemu_bh_new(nb_rx_bh, NULL);
    CHIHIRO_LOGF(NET, "netboard: MAC0 on hub 0 as %02X:%02X:%02X:%02X:%02X:%02X\n",
            nb.nic_conf.macaddr.a[0], nb.nic_conf.macaddr.a[1], nb.nic_conf.macaddr.a[2],
            nb.nic_conf.macaddr.a[3], nb.nic_conf.macaddr.a[4], nb.nic_conf.macaddr.a[5]);
}

/* ── The host's side of the bridge ──────────────────────────────────── */

bool chihiro_netboard_present(void)
{
    return nb_present;
}

void chihiro_netboard_set_host_interrupt(void (*fn)(void))
{
    nb.host_interrupt = fn;
}

void chihiro_netboard_host_command(const void *data, int len)
{
    if (!nb_present) return;
    nb_wake_board();
    if (len > 512) len = 512;
    qemu_mutex_lock(&nb.lock);
    memcpy(nb.ram + 0x600200, data, len);   /* both sides little-endian */
    qemu_mutex_unlock(&nb.lock);
}

void chihiro_netboard_host_command_slot(void *data, int len)
{
    if (len > 512) len = 512;
    if (!nb_present) { memset(data, 0, len); return; }
    qemu_mutex_lock(&nb.lock);
    memcpy(data, nb.ram + 0x600200, len);
    qemu_mutex_unlock(&nb.lock);
}

void chihiro_netboard_host_ring(void)
{
    if (!nb_present) return;
    nb_wake_board();
    if (chihiro_log_mask & CHIHIRO_LOG_NET) {
        const uint8_t *p = nb.ram + 0x600200;
        unsigned cmd = (p[3] << 8) | p[2];
        if (cmd != 0x0607 && cmd != 0x0001) {   /* not the heartbeats */
            char args[64];
            size_t n = 0;
            for (int a = 1; a < 8; a++) {
                n += snprintf(args + n, sizeof(args) - n, " %02X%02X%02X%02X",
                              p[a * 4 + 3], p[a * 4 + 2], p[a * 4 + 1],
                              p[a * 4]);
            }
            CHIHIRO_LOGF(NET, "netboard: host cmd %04X seq %02X%02X args%s\n",
                         cmd, p[1], p[0], args);
        }
    }
    qemu_mutex_lock(&nb.lock);
    nb.br_status |= BR_INT_CMD;
    bridge_update_irq();
    qemu_cond_signal(&nb.kick);
    qemu_mutex_unlock(&nb.lock);
}

void chihiro_netboard_host_window_write(uint32_t off, const void *data, int len)
{
    if (!nb_present || off >= CHIHIRO_NETBOARD_WINDOW_SIZE) return;
    nb_wake_board();
    if (off + len > CHIHIRO_NETBOARD_WINDOW_SIZE)
        len = CHIHIRO_NETBOARD_WINDOW_SIZE - off;
    qemu_mutex_lock(&nb.lock);
    memcpy(nb.ram + 0x600000 + off, data, len);
    qemu_mutex_unlock(&nb.lock);
}

void chihiro_netboard_host_window_read(uint32_t off, void *data, int len)
{
    if (!nb_present || off >= CHIHIRO_NETBOARD_WINDOW_SIZE) {
        memset(data, 0, len);
        return;
    }
    nb_wake_board();
    if (off + len > CHIHIRO_NETBOARD_WINDOW_SIZE)
        len = CHIHIRO_NETBOARD_WINDOW_SIZE - off;
    qemu_mutex_lock(&nb.lock);
    memcpy(data, nb.ram + 0x600000 + off, len);
    qemu_mutex_unlock(&nb.lock);
}

void chihiro_netboard_host_response(void *data, int len)
{
    if (len > 512) len = 512;
    if (!nb_present) { memset(data, 0, len); return; }
    qemu_mutex_lock(&nb.lock);
    memcpy(data, nb.ram + 0x600000, len);
    qemu_mutex_unlock(&nb.lock);
}

void chihiro_netboard_host_release(void)
{
    if (!nb_present) return;
    qemu_mutex_lock(&nb.lock);
    memset(nb.ram + 0x600000, 0, 32);
    qemu_mutex_unlock(&nb.lock);
}

void chihiro_netboard_host_ack(void)
{
    if (!nb_present) return;
    qemu_mutex_lock(&nb.lock);
    nb.br_host_pending = 0;
    qemu_mutex_unlock(&nb.lock);
}

/* ── Running ─────────────────────────────────────────────────────────── */

static void nb_service(void)
{
    sys_service();
    if (nb.cpu.illegal && !nb.said_illegal) {
        nb.said_illegal = true;
        fprintf(stderr, "Netboard: the MIPS core met an opcode it does not "
                "know, %08X at %08X; the firmware took a reserved-instruction "
                "exception\n", nb.cpu.illegal_op, nb.cpu.illegal_pc);
    }
}

static void nb_catch_up(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    int64_t delta = now - nb.last_us;

    nb.last_us = now;
    if (delta > 0) nb.paid += (uint64_t)delta * NB_CLOCKS_PER_US;

    if (nb.paid > nb.cpu.cycles + NB_MAX_ARREARS) {
        uint64_t lost = nb.paid - nb.cpu.cycles - NB_MAX_ARREARS;
        nb.paid -= lost;
        if (!nb.said_behind) {
            /* Its answers come late by as much: Maximum Tune only counts a
             * cabinet whose ping returns within a frame. The firmware's own
             * boot does it on most hosts. */
            nb.said_behind = true;
            CHIHIRO_LOGF(NET, "the host fell behind the network board; linked "
                         "cabinets may see it answer late\n");
        }
    }

    while (nb.cpu.cycles < nb.paid && nb.running && !nb.exiting) {
        uint64_t left = nb.paid - nb.cpu.cycles;
        uint64_t slice = MIN(left, (uint64_t)NB_SLICE_CLOCKS);
        mips_run(&nb.cpu, slice);
        nb_service();
        qemu_mutex_unlock(&nb.lock);
        qemu_mutex_lock(&nb.lock);
    }
}

static void *nb_thread(void *arg)
{
    rcu_register_thread();
    qemu_mutex_lock(&nb.lock);
    while (!nb.exiting) {
        if (nb.running) {
            nb_catch_up();
        } else {
            nb.last_us = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
            nb.paid = nb.cpu.cycles;
        }
        qemu_cond_timedwait(&nb.kick, &nb.lock, 1);
    }
    qemu_mutex_unlock(&nb.lock);
    rcu_unregister_thread();
    return NULL;
}

/* The firmware boots on first contact: the doorbell, the window, or a frame
 * from another cabinet, so a title that never uses the board pays nothing.
 * Callers hold the big lock. */
static void nb_wake_board(void)
{
    if (!nb_present || nb.running) {
        return;
    }
    qemu_mutex_lock(&nb.lock);
    nb.running = true;
    nb.last_us = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    nb.paid = nb.cpu.cycles;
    qemu_mutex_unlock(&nb.lock);
    /* Linux keeps thread names of up to fifteen characters. */
    qemu_thread_create(&nb.thread, "chihiro.netbd", nb_thread, &nb,
                       QEMU_THREAD_JOINABLE);
    CHIHIRO_LOGF(NET, "netboard: the cabinet spoke to it, its firmware starts\n");
}

static void nb_reset(void)
{
    mips_reset(&nb.cpu);
    nb.cpu.ram = nb.ram;
    nb.cpu.ram_size = NB_RAM_SIZE;
    memset(&nb.ic0, 0, sizeof(nb.ic0));
    memset(&nb.ic1, 0, sizeof(nb.ic1));
    memset(&nb.uart0, 0, sizeof(nb.uart0));
    nb.uart0.name = "UART0";
    nb.uart0.irq = IC0_UART0;
    memset(&nb.uart3, 0, sizeof(nb.uart3));
    nb.uart3.name = "UART3";
    nb.uart3.irq = IC0_UART3;
    memset(&nb.mac0, 0, sizeof(nb.mac0));
    phy_reset(&nb.mac0);
    nb.macen[0] = nb.macen[1] = 0;
    nb.gpio2_dir = nb.gpio2_out = nb.gpio2_inten = nb.gpio2_enable = 0;
    memset(nb.sys, 0, sizeof(nb.sys));
    memset(nb.sdram, 0, sizeof(nb.sdram));
    memset(nb.staticbus, 0, sizeof(nb.staticbus));
    memset(nb.pcictl, 0, sizeof(nb.pcictl));
    memset(nb.dma, 0, sizeof(nb.dma));
    nb.toy_base_cycles = nb.rtc_base_cycles = 0;
    nb.toy_write = nb.rtc_write = 0;
    nb.toy_last = nb.rtc_last = 0;
    nb.br_cmd = 0; nb.br_bar0 = BR_BAR0_DEFAULT; nb.br_bar1 = BR_BAR1_DEFAULT;
    memset(nb.br_cfg, 0, sizeof(nb.br_cfg));
    nb.br_win_mask = ~(BR_WINDOW_SIZE - 1); nb.br_win_base = 0;
    nb.br_status = nb.br_mask = nb.br_r030 = nb.br_r034 = 0;
    nb.br_host_pending = nb.br_window = 0;
    nb.eeprom.sda_slave = true;
    nb.eeprom.scl = nb.eeprom.sda_host = true;
    nb.said_illegal = false;
    /* The low-level sources (table 6-2: PCI_INTA#..INTD#, USB host) sit
     * high when idle; nothing here ever pulls INTB-D or USB low. */
    nb.ic0.level = (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 26);
}

bool chihiro_netboard_init(const char *firmware_path, const char serial[16],
                           uint32_t ip)
{
    if (nb_present) return true;
    if (!firmware_path || !firmware_path[0]) return false;

    FILE *f = qemu_fopen(firmware_path, "rb");
    if (!f) return false;

    memset(&nb, 0, sizeof(nb));
    nb.ram = g_malloc0(NB_RAM_SIZE);
    nb.flash = g_malloc(NB_FLASH_SIZE);
    memset(nb.flash, 0xFF, NB_FLASH_SIZE);
    nb.flash_have = (uint32_t)fread(nb.flash, 1, NB_FLASH_SIZE, f);
    fclose(f);
    if (nb.flash_have < 0x1000) {
        g_free(nb.ram); g_free(nb.flash);
        memset(&nb, 0, sizeof(nb));
        return false;
    }
    /* The 24LC04: serial and network configuration, as the firmware's
     * net-config buffer mirrors it (MAC at 0x10, mode at 0x18, IP, mask,
     * gateway, DNS at 0x20..0x30). The board gets 10.0.0.<cabinet ID>
     * (chihiro_cabinet_ip), on a /24, and a MAC in Sega's block. */
    memset(nb.eeprom.mem, 0xFF, sizeof(nb.eeprom.mem));
    memcpy(nb.eeprom.mem, serial, 16);
    /* Sega's OUI, then the cabinet's address: two cabinets of one owner
     * share a flash image and a serial, but never a cabinet ID. */
    nb.eeprom.mem[0x10] = 0x00; nb.eeprom.mem[0x11] = 0xD0; nb.eeprom.mem[0x12] = 0xF1;
    nb.eeprom.mem[0x13] = (ip >> 8) & 0xFF; nb.eeprom.mem[0x14] = (ip >> 16) & 0xFF;
    nb.eeprom.mem[0x15] = (ip >> 24) & 0xFF;
    memset(nb.eeprom.mem + 0x18, 0, 4);                     /* static */
    for (int i = 0; i < 4; i++) {
        nb.eeprom.mem[0x20 + i] = (ip >> (8 * i)) & 0xFF;   /* eIP */
        /* eMask 255.255.255.0 */
        nb.eeprom.mem[0x24 + i] = (0x00FFFFFFu >> (8 * i)) & 0xFF;
        nb.eeprom.mem[0x28 + i] = (ip >> (8 * i)) & 0xFF;   /* eGateway */
        nb.eeprom.mem[0x2C + i] = 0;                        /* eDNS1 */
        nb.eeprom.mem[0x30 + i] = 0;                        /* eDNS2 */
    }

    mips_init(&nb.cpu, &nb, nb_read, nb_write);
    nb_reset();

    qemu_mutex_init(&nb.lock);
    qemu_cond_init(&nb.kick);
    nb.running = false;          /* its firmware waits to be spoken to */
    nb.last_us = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    nb.paid = 0;
    nb_present = true;
    CHIHIRO_LOGF(NET, "netboard: Au1500 ready, %u KB of firmware in a %u KB flash\n",
            nb.flash_have >> 10, NB_FLASH_SIZE >> 10);
    return true;
}

void chihiro_netboard_exit(void)
{
    if (!nb_present) return;
    bool was_running;
    qemu_mutex_lock(&nb.lock);
    was_running = nb.running;
    nb.exiting = true;
    qemu_cond_signal(&nb.kick);
    qemu_mutex_unlock(&nb.lock);
    if (was_running)
        qemu_thread_join(&nb.thread);
    if (nb.nic) { qemu_del_nic(nb.nic); nb.nic = NULL; }
    if (nb.tx_bh) { qemu_bh_delete(nb.tx_bh); nb.tx_bh = NULL; }
    if (nb.rx_bh) { qemu_bh_delete(nb.rx_bh); nb.rx_bh = NULL; }
    g_free(nb.ram); nb.ram = NULL;
    g_free(nb.flash); nb.flash = NULL;
    nb_present = false;
}

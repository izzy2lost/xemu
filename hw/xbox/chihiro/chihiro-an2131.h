/*
 * Cypress AN2131 (EZ-USB) register mapping for Chihiro baseboard.
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
 *
 * Provides the hardware register layer between the generic 8051 CPU core
 * and the AN2131-specific USB, I2C, and port I/O peripherals.
 * The ic10/pc20 firmware runs on the 8051 and accesses these registers
 * via XDATA (MOVX) and SFR instructions.
 *
 * Register map from Cypress EZ-USB Technical Reference Manual v1.9/1.10.
 */

#ifndef CHIHIRO_AN2131_H
#define CHIHIRO_AN2131_H

#include "chihiro-8051.h"

/* ── Memory layout ──────────────────────────────────────────────────── */

#define AN2131_RAM_SIZE     8192    /* 8KB on-chip code/data RAM */
#define AN2131_EP_COUNT     8       /* EP0-EP7 */
#define AN2131_EP_BUFSZ     64      /* Bytes per endpoint buffer */

/*
 * Endpoint buffer base addresses (canonical 0x7xxx form).
 *   INnBUF  = 0x7F00 - n * 0x80
 *   OUTnBUF = 0x7EC0 - n * 0x80
 * Aliases at 0x1xxx (subtract 0x6000) point to same physical RAM.
 */
#define AN_IN0BUF      0x7F00
#define AN_OUT0BUF     0x7EC0

/* ── XDATA register addresses (0x7F40-0x7FFF) ──────────────────────── */

/* CPU control */
#define AN_CPUCS       0x7F92

/* Port configuration */
#define AN_PORTACFG    0x7F93
#define AN_PORTBCFG    0x7F94
#define AN_PORTCCFG    0x7F95
#define AN_OUTA        0x7F96
#define AN_OUTB        0x7F97
#define AN_OUTC        0x7F98
#define AN_PINSA       0x7F99
#define AN_PINSB       0x7F9A
#define AN_PINSC       0x7F9B
#define AN_OEA         0x7F9C
#define AN_OEB         0x7F9D
#define AN_OEC         0x7F9E

/* I2C */
#define AN_I2CS        0x7FA5
#define AN_I2DAT       0x7FA6

/* Interrupts */
#define AN_IVEC        0x7FA8
#define AN_IN07IRQ     0x7FA9
#define AN_OUT07IRQ    0x7FAA
#define AN_USBIRQ      0x7FAB
#define AN_IN07IEN     0x7FAC
#define AN_OUT07IEN    0x7FAD
#define AN_USBIEN      0x7FAE
#define AN_USBBAV      0x7FAF

/* EP0 control/byte count */
#define AN_EP0CS       0x7FB4
#define AN_IN0BC       0x7FB5
/* EP1-7 IN:  CS = 0x7FB6 + (n-1)*2,  BC = 0x7FB7 + (n-1)*2 */
#define AN_OUT0BC      0x7FC5
/* EP1-7 OUT: CS = 0x7FC6 + (n-1)*2,  BC = 0x7FC7 + (n-1)*2 */

/* USB global */
#define AN_SUDPTRH     0x7FD4
#define AN_SUDPTRL     0x7FD5
#define AN_USBCS       0x7FD6
#define AN_TOGCTL      0x7FD7
#define AN_FNADDR      0x7FDB
#define AN_USBPAIR     0x7FDD
#define AN_IN07VAL     0x7FDE
#define AN_OUT07VAL    0x7FDF

/* Autopointer */
#define AN_FASTXFR     0x7FE2
#define AN_AUTOPTRH    0x7FE3
#define AN_AUTOPTRL    0x7FE4
#define AN_AUTODATA    0x7FE5

/* SETUP data buffer (8 bytes, read-only from firmware) */
#define AN_SETUPDAT    0x7FE8

/* ── Bit definitions ────────────────────────────────────────────────── */

/* USBIRQ / USBIEN bits */
#define USBIRQ_SUDAV   0x01
#define USBIRQ_SOF     0x02
#define USBIRQ_SUTOK   0x04
#define USBIRQ_SUSP    0x08
#define USBIRQ_URES    0x10

/* I2CS bits */
#define I2CS_START     0x80
#define I2CS_STOP      0x40
#define I2CS_LASTRD    0x20
#define I2CS_ACK       0x02
#define I2CS_DONE      0x01

/* EP0CS bits */
#define EP0CS_OUTBSY   0x08
#define EP0CS_INBSY    0x04

/* EPnCS bits (IN and OUT, n=1..7) */
#define EPCS_BSY       0x02

/* USBBAV bits */
#define USBBAV_AVEN    0x01

/* CPUCS bits */
#define CPUCS_8051RES  0x01

/* ── USB autovector values (written to IVEC and code[0x0045]) ───────── */

#define AVEC_SUDAV     0x00
#define AVEC_SOF       0x04
#define AVEC_SUTOK     0x08
#define AVEC_SUSPEND   0x0C
#define AVEC_USBRESET  0x10
#define AVEC_EP0IN     0x18
#define AVEC_EP0OUT    0x1C
/* EPn IN  = 0x18 + n * 8 */
/* EPn OUT = 0x1C + n * 8 */

/* ── 8051 interrupt vectors ─────────────────────────────────────────── */

#define INT2_VECTOR    0x0043   /* USB (all sources via autovector) */
#define INT3_VECTOR    0x004B   /* I2C DONE */

/* ── AN2131-specific SFR addresses ──────────────────────────────────── */

#define SFR_DPS        0x86    /* Data Pointer Select (bit 0) */
#define SFR_CKCON      0x8E    /* Clock Control; bit 3: timer 0 at CLK/4, not /12 */
#define SFR_EXIF       0x91    /* External Interrupt Flags */
#define SFR_MPAGE      0x92    /* MOVX @Ri page; stored, the core pages with P2 */
#define SFR_SCON1      0xC0    /* Serial Port 1 Control */
#define SFR_EIE        0xE8    /* Extended Interrupt Enable */
#define SFR_EIP        0xF8    /* Extended Interrupt Priority */

/* ── I2C state machine phases ───────────────────────────────────────── */

typedef enum {
    I2C_IDLE,       /* No transaction */
    I2C_ADDR,       /* Expecting slave address byte after START */
    I2C_MEM_ADDR,   /* Expecting EEPROM memory address byte(s) */
    I2C_DATA        /* Data transfer (read or write) */
} I2CPhase;

/* ── AN2131 state ───────────────────────────────────────────────────── */

typedef struct AN2131State {
    Cpu8051State cpu;

    /*
     * Shared code/data RAM (8KB on-chip).
     *   0x0000-0x1B3F  code + general purpose XDATA
     *   0x1B40-0x1F3F  endpoint buffers (64B each, EP7 at bottom)
     *   0x1F40-0x1FFF  register alias area (registers stored separately)
     */
    uint8_t ram[AN2131_RAM_SIZE];

    /* ── Endpoint registers ─────────────────────────────────────── */
    struct {
        uint8_t cs_in;      /* INnCS: bit 1=BSY, bit 0=STALL */
        uint8_t bc_in;      /* INnBC: byte count for IN transfer */
        uint8_t cs_out;     /* OUTnCS */
        uint8_t bc_out;     /* OUTnBC: byte count of received OUT data */
        bool    in_armed;   /* Firmware wrote INnBC → buffer ready to send */
    } ep[AN2131_EP_COUNT];
    uint8_t ep0cs;          /* EP0CS special register */

    /* ── Interrupt registers ────────────────────────────────────── */
    uint8_t ivec;           /* Autovector source (bits 6:2) */
    uint8_t in07irq;        /* EP0-7 IN interrupt requests */
    uint8_t out07irq;       /* EP0-7 OUT interrupt requests */
    uint8_t usbirq;         /* USB global interrupt requests */
    uint8_t in07ien;        /* EP0-7 IN interrupt enables */
    uint8_t out07ien;       /* EP0-7 OUT interrupt enables */
    uint8_t usbien;         /* USB global interrupt enables */
    uint8_t usbbav;         /* Breakpoint/autovector control */

    /* ── I2C bus master ─────────────────────────────────────────── */
    uint8_t i2cs;           /* I2C control/status register */
    bool    i2c_irq_pending;
    bool    i2c_lastrd;     /* LASTRD flag for final read byte */
    struct {
        I2CPhase phase;
        uint16_t mem_addr;          /* Current EEPROM byte address */
        bool     reading;           /* true = read from slave */
        bool     first_read;        /* EZ-USB dummy read after RESTART */
        int      addr_bytes_sent;   /* Address bytes received so far */
        int      addr_bytes_needed; /* 1 for 24LC024, 2 for 24LC64 */
        uint8_t *eeprom;            /* Pointer to selected EEPROM buffer */
        int      eeprom_size;
    } i2c;

    /* I2C EEPROM backing storage (owned by caller) */
    uint8_t *ic10_eeprom;   /* 8KB: ic10 (QC) or pc20 (SC) */
    int      ic10_size;
    uint8_t *ic11_eeprom;   /* baseboard config: the 128-byte dump */
    int      ic11_size;

    /* I2C RTC (address 0x32) */
    uint8_t  rtc_regs[16];

    /* The baseboard SRAM (ST M68AF127B, 128 KB on a battery), owned by the
     * caller: two 64 KB halves. On the QC, port C bit 3 (set up at 0x055A)
     * picks one: half 0 is the firmware's work RAM, half 1 the game's backup,
     * reached only for backup requests 0x18 and 0x1F (0x0D48, 0x0DBF). */
    uint8_t *extmem;

    /* ── USB global registers ───────────────────────────────────── */
    uint8_t  cpucs;
    uint8_t  usbcs;
    uint8_t  fnaddr;
    uint8_t  togctl;
    uint8_t  usbpair;
    uint8_t  in07val;
    uint8_t  out07val;
    uint16_t sudptr;
    uint8_t  fastxfr;
    uint16_t autoptr;
    uint8_t  setupdat[8];

    /* ── Port I/O ───────────────────────────────────────────────── */
    uint8_t outa, outb, outc;
    uint8_t pinsa, pinsb, pinsc;
    uint8_t oea, oeb, oec;
    uint8_t portacfg, portbcfg, portccfg;

    /* ── AN2131-specific SFRs ───────────────────────────────────── */
    uint8_t exif;           /* 0x91: external interrupt flags */
    uint8_t eie;            /* 0xE8: extended interrupt enable */
    uint8_t eip;            /* 0xF8: extended interrupt priority */
    uint8_t mpage;          /* 0x92: MOVX @Ri page register (stored only) */
    uint8_t dps;            /* 0x86: data pointer select */

    /* ── JVS serial (SBUF1 at SFR 0xC1) ──────────────────────────── */
    uint8_t jvs_tx_buf[256];
    int     jvs_tx_len;
    int     jvs_tx_expected;   /* total bytes expected in current frame */
    bool    jvs_tx_escape;     /* next TX byte is escaped (follows 0xD0) */
    uint8_t jvs_rx_buf[512];
    int     jvs_rx_len;
    int     jvs_rx_pos;        /* next byte to feed back via SBUF1 RX */

    /* ── QC/SC identity ────────────────────────────────────────── */
    bool is_qc;             /* true = JVS/RS-485, false = MIDI/drive board */

    /* ── MIDI RX (drive board response injection) ─────────────── */
    bool     midi_response_ready;
    uint64_t midi_response_set_cycles;
    uint64_t card_resp_cycles[2];      /* card RX pacing: last byte time (0=idle) */
    bool     card_delivering[2];       /* a response byte has been delivered */
    uint64_t card_ti_cycles[2];        /* pending TX-complete: SBUF write time (0=none) */
    bool     in_setup;                 /* an SC 0x1A/0x1B IN request is being served */
    /* The battery-backed half of the baseboard SRAM, or the settings EEPROM,
     * holds something the game wrote since the save file last did. ic10
     * and RTC writes set it too, which only costs a needless save. */
    bool     backup_dirty;

    /* ── Runtime state ──────────────────────────────────────────── */
    bool cpu_running;       /* true after CPUCS release */
    bool jvs_response_ready;  /* JVS response generated, waiting for TX drain before RX */
    bool jvs_rx_pending;      /* RI1 cleared, next byte deferred until after RETI */
    uint64_t total_cycles;             /* cumulative CPU cycles (advances during bursts) */
    uint64_t jvs_response_set_cycles;  /* total_cycles when JVS response was generated */

    /* The firmware's main loop at rest, recognised while it runs (see
     * an2131_idle_turn). Not migrated: a restored chip learns it again. */
    struct {
        uint16_t pc;        /* the turn's starting point, sampled */
        int      sample;    /* steps since the last candidate was taken */
        uint64_t hash;      /* what the chip looked like there last time */
        uint64_t cycles;    /* total_cycles then */
        uint64_t steps;     /* steps then */
        uint32_t events;    /* cpu.events then */
    } idle;
    uint64_t steps;         /* instructions an2131_run has stepped */


} AN2131State;

/* ── Public API ──────────────────────────────────────────────────────── */

void an2131_init(AN2131State *s);
void an2131_relink(AN2131State *s);
void an2131_b2_boot(AN2131State *s, const uint8_t *eeprom, int eeprom_size);

void an2131_anchor_load(AN2131State *s, uint16_t addr,
                        const uint8_t *data, int len);

int  an2131_setup_packet(AN2131State *s, const uint8_t setup[8],
                         const uint8_t *out_data, int out_len,
                         uint8_t *resp_buf, int resp_max);

int  an2131_ep_in_poll(AN2131State *s, int ep_nr);
int  an2131_ep_in_read(AN2131State *s, int ep_nr,
                       uint8_t *buf, int max_len);
void an2131_ep_out_write(AN2131State *s, int ep_nr,
                         const uint8_t *data, int len);

int  an2131_run(AN2131State *s, int max_cycles);

#endif /* CHIHIRO_AN2131_H */

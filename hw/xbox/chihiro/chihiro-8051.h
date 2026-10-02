/*
 * 8051 CPU core for Chihiro AN2131 (Cypress EZ-USB) emulation
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
#ifndef CHIHIRO_8051_H
#define CHIHIRO_8051_H

#include <stdint.h>
#include <stdbool.h>

/* PSW bits. P (bit 0) is not computed: neither firmware reads it. */
#define PSW_CY  0x80
#define PSW_AC  0x40
#define PSW_OV  0x04

/* Standard SFR addresses */
#define SFR_SP   0x81
#define SFR_DPL  0x82
#define SFR_DPH  0x83
#define SFR_TCON 0x88
#define SFR_TMOD 0x89
#define SFR_TL0  0x8A
#define SFR_TL1  0x8B
#define SFR_TH0  0x8C
#define SFR_TH1  0x8D
#define SFR_SCON 0x98
#define SFR_SBUF 0x99
#define SFR_P2   0xA0
#define SFR_IE   0xA8
#define SFR_PSW  0xD0
#define SFR_ACC  0xE0
#define SFR_B    0xF0

typedef struct Cpu8051State Cpu8051State;

/* Callbacks for external memory (XDATA) — AN2131 registers plug in here */
typedef uint8_t (*Cpu8051XdataRead)(Cpu8051State *s, uint16_t addr);
typedef void    (*Cpu8051XdataWrite)(Cpu8051State *s, uint16_t addr, uint8_t val);

/* Callback for SFR access — lets AN2131 layer intercept chip-specific SFRs */
typedef uint8_t (*Cpu8051SfrRead)(Cpu8051State *s, uint8_t addr);
typedef void    (*Cpu8051SfrWrite)(Cpu8051State *s, uint8_t addr, uint8_t val);

struct Cpu8051State {
    /* Core registers */
    uint16_t pc;
    uint8_t  sp;
    uint8_t  acc;
    uint8_t  b;
    uint16_t dptr;
    uint16_t dptr_alt;  /* Second DPTR for dual-DPTR 8051 variants */
    uint8_t  psw;

    /* Internal RAM: 256 bytes
     *   0x00-0x7F: directly + indirectly addressable
     *   0x80-0xFF: indirectly addressable only (direct goes to SFR) */
    uint8_t iram[256];

    /* SFR space: 128 bytes (0x80-0xFF), direct addressing only.
     * Standard SFRs stored here; AN2131-specific ones forwarded via callback. */
    uint8_t sfr[128];

    /* Code memory — the chip's RAM (AN2131State.ram), loaded from the ic10
     * (QC) or pc20 (SC) EEPROM and by the host's Anchor loads */
    const uint8_t *code;
    uint32_t code_size;

    /* Callbacks for all XDATA access (the AN2131's RAM and registers) */
    Cpu8051XdataRead  xdata_read;
    Cpu8051XdataWrite xdata_write;

    /* Callbacks for SFR access (AN2131-specific registers) */
    Cpu8051SfrRead  sfr_read_cb;
    Cpu8051SfrWrite sfr_write_cb;

    /* Interrupt state */
    bool     in_interrupt;  /* Set by cpu8051_interrupt, cleared by RETI */
    /* Set whenever something could have changed the interrupt state, so the
     * owner knows it must re-evaluate. Sources are enumerated and closed:
     * SFR write, timer overflow, RETI, interrupt entry, external event, and
     * on the AN2131 a register write. */
    bool     irq_recheck;
    /* What changes the chip other than an instruction's effect on its
     * registers and internal RAM. The core counts the interrupts it enters
     * and the timer flags it raises; the chip around it adds its own (see
     * an2131_idle_turn). */
    uint32_t events;

    /* Timer 0 CLK/12 prescaler (one tick per 3 instructions when CKCON.3=0) */
    uint8_t timer0_prescale;

    /* Opaque pointer for AN2131 layer */
    void *opaque;
};

void     cpu8051_init(Cpu8051State *s);
void     cpu8051_reset(Cpu8051State *s);
int      cpu8051_step(Cpu8051State *s);
void     cpu8051_interrupt(Cpu8051State *s, uint8_t vector);

#endif /* CHIHIRO_8051_H */

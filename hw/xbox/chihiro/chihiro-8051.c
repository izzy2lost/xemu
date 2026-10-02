/*
 * 8051 CPU core for Chihiro AN2131 (Cypress EZ-USB) emulation.
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
 * Clean-room implementation from the public 8051 instruction set reference.
 * AN2131-specific register mapping is handled externally via callbacks.
 */

#include "qemu/osdep.h"
#include "chihiro-8051.h"

/* ── memory access helpers ─────────────────────────────────────────── */

static inline uint8_t code_read(Cpu8051State *s, uint16_t addr)
{
    if (addr < s->code_size) return s->code[addr];
    return 0xFF;
}

static inline uint8_t fetch(Cpu8051State *s)
{
    return code_read(s, s->pc++);
}

static inline void fetch16_hi(Cpu8051State *s, uint16_t *out)
{
    uint8_t hi = fetch(s);
    uint8_t lo = fetch(s);
    *out = (hi << 8) | lo;
}

/* Direct addressing: 0x00-0x7F → iram, 0x80-0xFF → SFR */
static inline uint8_t direct_read(Cpu8051State *s, uint8_t addr)
{
    if (addr < 0x80) return s->iram[addr];
    if (addr == SFR_ACC) return s->acc;
    if (addr == SFR_B)   return s->b;
    if (addr == SFR_PSW) return s->psw;
    if (addr == SFR_SP)  return s->sp;
    if (addr == SFR_DPL) return (uint8_t)(s->dptr);
    if (addr == SFR_DPH) return (uint8_t)(s->dptr >> 8);
    if (s->sfr_read_cb) return s->sfr_read_cb(s, addr);
    return s->sfr[addr - 0x80];
}

static inline void direct_write(Cpu8051State *s, uint8_t addr, uint8_t val)
{
    if (addr < 0x80) { s->iram[addr] = val; return; }
    switch (addr) {
    case SFR_ACC: s->acc = val; return;
    case SFR_B:   s->b = val; return;
    case SFR_PSW: s->psw = val; return;
    case SFR_SP:  s->sp = val; return;
    case SFR_DPL: s->dptr = (s->dptr & 0xFF00) | val; return;
    case SFR_DPH: s->dptr = (s->dptr & 0x00FF) | (val << 8); return;
    default: break;
    }
    s->irq_recheck = true;
    if (s->sfr_write_cb) { s->sfr_write_cb(s, addr, val); return; }
    s->sfr[addr - 0x80] = val;
}

/* Indirect addressing: 0x00-0xFF → iram (both halves) */
static inline uint8_t indirect_read(Cpu8051State *s, uint8_t addr)
{
    return s->iram[addr];
}

static inline void indirect_write(Cpu8051State *s, uint8_t addr, uint8_t val)
{
    s->iram[addr] = val;
}

/* XDATA (MOVX) */
static inline uint8_t xdata_read(Cpu8051State *s, uint16_t addr)
{
    if (s->xdata_read) return s->xdata_read(s, addr);
    return 0xFF;
}

static inline void xdata_write(Cpu8051State *s, uint16_t addr, uint8_t val)
{
    if (s->xdata_write) s->xdata_write(s, addr, val);
}

/* Register bank */
static inline uint8_t *reg_ptr(Cpu8051State *s, uint8_t n)
{
    return &s->iram[((s->psw >> 3) & 3) * 8 + n];
}

/* ── bit access ────────────────────────────────────────────────────── */

static inline void bit_decode(uint8_t bitaddr, uint8_t *byte_addr, uint8_t *mask)
{
    if (bitaddr < 0x80) {
        *byte_addr = 0x20 + (bitaddr >> 3);
        *mask = 1 << (bitaddr & 7);
    } else {
        *byte_addr = bitaddr & 0xF8;
        *mask = 1 << (bitaddr & 7);
    }
}

static inline bool bit_read(Cpu8051State *s, uint8_t bitaddr)
{
    uint8_t ba, mask;
    bit_decode(bitaddr, &ba, &mask);
    return (direct_read(s, ba) & mask) != 0;
}

static inline void bit_write(Cpu8051State *s, uint8_t bitaddr, bool val)
{
    uint8_t ba, mask;
    bit_decode(bitaddr, &ba, &mask);
    uint8_t v = direct_read(s, ba);
    if (val) v |= mask; else v &= ~mask;
    direct_write(s, ba, v);
}

/* ── stack ─────────────────────────────────────────────────────────── */

static inline void push8(Cpu8051State *s, uint8_t val)
{
    s->sp++;
    s->iram[s->sp] = val;
}

static inline uint8_t pop8(Cpu8051State *s)
{
    uint8_t val = s->iram[s->sp];
    s->sp--;
    return val;
}

/* ── timer tick (called once per instruction) ─────────────────────── */

/* Timers 0 and 1 in modes 1 and 2 only: GATE, modes 0 and 3 and Timer 2 are
 * not modelled, and Timer 0 always counts the clock (C/T ignored). The UARTs
 * here do not run off the timers. */
static void timer_tick(Cpu8051State *s)
{
    uint8_t tcon = s->sfr[SFR_TCON - 0x80];
    uint8_t tmod = s->sfr[SFR_TMOD - 0x80];

    /* Timer 0 — only if TR0 is set (TCON.4) */
    if (tcon & 0x10) {
        /* CKCON (0x8E, an AN2131 register) bit 3, T0M: 0 = CLK/12, one tick
         * every 3 instructions here (HEURISTIC: silicon counts machine
         * cycles); 1 = CLK/4, one tick per instruction. */
        bool t0_fast = s->sfr[0x8E - 0x80] & 0x08;
        bool t0_tick = true;
        if (!t0_fast) {
            s->timer0_prescale++;
            if (s->timer0_prescale < 3) t0_tick = false;
            else s->timer0_prescale = 0;
        }
        if (t0_tick) {
            uint8_t mode0 = tmod & 0x03;
            if (mode0 == 1) {
                /* Mode 1: 16-bit timer */
                uint16_t t0 = s->sfr[SFR_TL0 - 0x80] | ((uint16_t)s->sfr[SFR_TH0 - 0x80] << 8);
                t0++;
                if (t0 == 0) {
                    tcon |= 0x20;  /* TF0 — overflow flag */
                    s->irq_recheck = true;
                }
                s->sfr[SFR_TL0 - 0x80] = (uint8_t)t0;
                s->sfr[SFR_TH0 - 0x80] = (uint8_t)(t0 >> 8);
            } else if (mode0 == 2) {
                /* Mode 2: 8-bit auto-reload */
                uint8_t tl = s->sfr[SFR_TL0 - 0x80];
                tl++;
                if (tl == 0) {
                    tl = s->sfr[SFR_TH0 - 0x80];
                    tcon |= 0x20;
                    s->irq_recheck = true;
                }
                s->sfr[SFR_TL0 - 0x80] = tl;
            }
        }
    }

    /* Timer 1 — only if TR1 is set (TCON.6) and C/T#=0 (internal clock) */
    if ((tcon & 0x40) && !(tmod & 0x40)) {
        uint8_t mode1 = (tmod >> 4) & 0x03;
        if (mode1 == 1) {
            uint16_t t1 = s->sfr[SFR_TL1 - 0x80] | ((uint16_t)s->sfr[SFR_TH1 - 0x80] << 8);
            t1++;
            if (t1 == 0) {
                tcon |= 0x80;  /* TF1 */
                s->irq_recheck = true;
            }
            s->sfr[SFR_TL1 - 0x80] = (uint8_t)t1;
            s->sfr[SFR_TH1 - 0x80] = (uint8_t)(t1 >> 8);
        } else if (mode1 == 2) {
            uint8_t tl = s->sfr[SFR_TL1 - 0x80];
            tl++;
            if (tl == 0) {
                tl = s->sfr[SFR_TH1 - 0x80];
                tcon |= 0x80;
                s->irq_recheck = true;
            }
            s->sfr[SFR_TL1 - 0x80] = tl;
        }
    }

    /* Stored only when a flag was raised: this runs every instruction. */
    if (s->sfr[SFR_TCON - 0x80] != tcon) {
        s->sfr[SFR_TCON - 0x80] = tcon;
        s->events++;
    }
}

/* ── instruction execution ─────────────────────────────────────────── */

int cpu8051_step(Cpu8051State *s)
{
    timer_tick(s);

    uint8_t op = fetch(s);
    uint8_t a, b8, d, tmp, carry;
    uint16_t w;
    int16_t rel;
    uint16_t result;

    switch (op) {

    /* ── NOP ── */
    case 0x00: return 1;

    /* ── AJMP/ACALL (pages) ── */
    case 0x01: case 0x21: case 0x41: case 0x61:
    case 0x81: case 0xA1: case 0xC1: case 0xE1:
        a = fetch(s);
        s->pc = (s->pc & 0xF800) | ((uint16_t)(op & 0xE0) << 3) | a;
        return 2;

    case 0x11: case 0x31: case 0x51: case 0x71:
    case 0x91: case 0xB1: case 0xD1: case 0xF1:
        a = fetch(s);
        push8(s, (uint8_t)(s->pc));
        push8(s, (uint8_t)(s->pc >> 8));
        s->pc = (s->pc & 0xF800) | ((uint16_t)(op & 0xE0) << 3) | a;
        return 2;

    /* ── LJMP addr16 ── */
    case 0x02:
        fetch16_hi(s, &w);
        s->pc = w;
        return 2;

    /* ── RR A ── */
    case 0x03:
        s->acc = (s->acc >> 1) | (s->acc << 7);
        return 1;

    /* ── INC A ── */
    case 0x04: s->acc++; return 1;

    /* ── INC direct ── */
    case 0x05:
        d = fetch(s);
        direct_write(s, d, direct_read(s, d) + 1);
        return 1;

    /* ── INC @R0, @R1 ── */
    case 0x06: case 0x07:
        a = *reg_ptr(s, op & 1);
        indirect_write(s, a, indirect_read(s, a) + 1);
        return 1;

    /* ── INC R0-R7 ── */
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        (*reg_ptr(s, op & 7))++;
        return 1;

    /* ── LCALL addr16 ── */
    case 0x12:
        fetch16_hi(s, &w);
        push8(s, (uint8_t)(s->pc));
        push8(s, (uint8_t)(s->pc >> 8));
        s->pc = w;
        return 2;

    /* ── RRC A ── */
    case 0x13:
        carry = (s->psw & PSW_CY) ? 1 : 0;
        if (s->acc & 1) s->psw |= PSW_CY; else s->psw &= ~PSW_CY;
        s->acc = (s->acc >> 1) | (carry << 7);
        return 1;

    /* ── DEC A ── */
    case 0x14: s->acc--; return 1;

    /* ── DEC direct ── */
    case 0x15:
        d = fetch(s);
        direct_write(s, d, direct_read(s, d) - 1);
        return 1;

    /* ── DEC @R0, @R1 ── */
    case 0x16: case 0x17:
        a = *reg_ptr(s, op & 1);
        indirect_write(s, a, indirect_read(s, a) - 1);
        return 1;

    /* ── DEC R0-R7 ── */
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        (*reg_ptr(s, op & 7))--;
        return 1;

    /* ── JB bit,rel ── */
    case 0x20:
        d = fetch(s); rel = (int8_t)fetch(s);
        if (bit_read(s, d)) s->pc += rel;
        return 2;

    /* ── RL A ── */
    case 0x23:
        s->acc = (s->acc << 1) | (s->acc >> 7);
        return 1;

    /* ── ADD A,#imm ── */
    case 0x24:
        b8 = fetch(s);
        goto do_add;

    /* ── ADD A,direct ── */
    case 0x25:
        b8 = direct_read(s, fetch(s));
        goto do_add;

    /* ── ADD A,@R0 / @R1 ── */
    case 0x26: case 0x27:
        b8 = indirect_read(s, *reg_ptr(s, op & 1));
        goto do_add;

    /* ── ADD A,R0-R7 ── */
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        b8 = *reg_ptr(s, op & 7);
    do_add:
        result = (uint16_t)s->acc + b8;
        s->psw &= ~(PSW_CY | PSW_AC | PSW_OV);
        if (result > 0xFF) s->psw |= PSW_CY;
        if (((s->acc & 0x0F) + (b8 & 0x0F)) > 0x0F) s->psw |= PSW_AC;
        if (((s->acc ^ b8 ^ 0x80) & (s->acc ^ result) & 0x80)) s->psw |= PSW_OV;
        s->acc = (uint8_t)result;
        return 1;

    /* ── JNB bit,rel ── */
    case 0x30:
        d = fetch(s); rel = (int8_t)fetch(s);
        if (!bit_read(s, d)) s->pc += rel;
        return 2;

    /* ── RLC A ── */
    case 0x33:
        carry = (s->psw & PSW_CY) ? 1 : 0;
        if (s->acc & 0x80) s->psw |= PSW_CY; else s->psw &= ~PSW_CY;
        s->acc = (s->acc << 1) | carry;
        return 1;

    /* ── ADDC A,#imm ── */
    case 0x34:
        b8 = fetch(s);
        goto do_addc;

    /* ── ADDC A,direct ── */
    case 0x35:
        b8 = direct_read(s, fetch(s));
        goto do_addc;

    /* ── ADDC A,@R0 / @R1 ── */
    case 0x36: case 0x37:
        b8 = indirect_read(s, *reg_ptr(s, op & 1));
        goto do_addc;

    /* ── ADDC A,R0-R7 ── */
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        b8 = *reg_ptr(s, op & 7);
    do_addc:
        carry = (s->psw & PSW_CY) ? 1 : 0;
        result = (uint16_t)s->acc + b8 + carry;
        s->psw &= ~(PSW_CY | PSW_AC | PSW_OV);
        if (result > 0xFF) s->psw |= PSW_CY;
        if (((s->acc & 0x0F) + (b8 & 0x0F) + carry) > 0x0F) s->psw |= PSW_AC;
        if (((s->acc ^ b8 ^ 0x80) & (s->acc ^ result) & 0x80)) s->psw |= PSW_OV;
        s->acc = (uint8_t)result;
        return 1;

    /* ── JC rel ── */
    case 0x40:
        rel = (int8_t)fetch(s);
        if (s->psw & PSW_CY) s->pc += rel;
        return 2;

    /* ── ORL direct,A ── */
    case 0x42:
        d = fetch(s);
        direct_write(s, d, direct_read(s, d) | s->acc);
        return 1;

    /* ── ORL direct,#imm ── */
    case 0x43:
        d = fetch(s); b8 = fetch(s);
        direct_write(s, d, direct_read(s, d) | b8);
        return 2;

    /* ── ORL A,#imm ── */
    case 0x44:
        s->acc |= fetch(s);
        return 1;

    /* ── ORL A,direct ── */
    case 0x45:
        s->acc |= direct_read(s, fetch(s));
        return 1;

    /* ── ORL A,@R0 / @R1 ── */
    case 0x46: case 0x47:
        s->acc |= indirect_read(s, *reg_ptr(s, op & 1));
        return 1;

    /* ── ORL A,R0-R7 ── */
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        s->acc |= *reg_ptr(s, op & 7);
        return 1;

    /* ── JNC rel ── */
    case 0x50:
        rel = (int8_t)fetch(s);
        if (!(s->psw & PSW_CY)) s->pc += rel;
        return 2;

    /* ── ANL direct,A ── */
    case 0x52:
        d = fetch(s);
        direct_write(s, d, direct_read(s, d) & s->acc);
        return 1;

    /* ── ANL direct,#imm ── */
    case 0x53:
        d = fetch(s); b8 = fetch(s);
        direct_write(s, d, direct_read(s, d) & b8);
        return 2;

    /* ── ANL A,#imm ── */
    case 0x54:
        s->acc &= fetch(s);
        return 1;

    /* ── ANL A,direct ── */
    case 0x55:
        s->acc &= direct_read(s, fetch(s));
        return 1;

    /* ── ANL A,@R0 / @R1 ── */
    case 0x56: case 0x57:
        s->acc &= indirect_read(s, *reg_ptr(s, op & 1));
        return 1;

    /* ── ANL A,R0-R7 ── */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        s->acc &= *reg_ptr(s, op & 7);
        return 1;

    /* ── JZ rel ── */
    case 0x60:
        rel = (int8_t)fetch(s);
        if (s->acc == 0) s->pc += rel;
        return 2;

    /* ── XRL direct,A ── */
    case 0x62:
        d = fetch(s);
        direct_write(s, d, direct_read(s, d) ^ s->acc);
        return 1;

    /* ── XRL direct,#imm ── */
    case 0x63:
        d = fetch(s); b8 = fetch(s);
        direct_write(s, d, direct_read(s, d) ^ b8);
        return 2;

    /* ── XRL A,#imm ── */
    case 0x64:
        s->acc ^= fetch(s);
        return 1;

    /* ── XRL A,direct ── */
    case 0x65:
        s->acc ^= direct_read(s, fetch(s));
        return 1;

    /* ── XRL A,@R0 / @R1 ── */
    case 0x66: case 0x67:
        s->acc ^= indirect_read(s, *reg_ptr(s, op & 1));
        return 1;

    /* ── XRL A,R0-R7 ── */
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        s->acc ^= *reg_ptr(s, op & 7);
        return 1;

    /* ── JNZ rel ── */
    case 0x70:
        rel = (int8_t)fetch(s);
        if (s->acc != 0) s->pc += rel;
        return 2;

    /* ── ORL C,bit ── */
    case 0x72:
        d = fetch(s);
        if (bit_read(s, d)) s->psw |= PSW_CY;
        return 2;

    /* ── JMP @A+DPTR ── */
    case 0x73:
        s->pc = s->acc + s->dptr;
        return 2;

    /* ── MOV A,#imm ── */
    case 0x74:
        s->acc = fetch(s);
        return 1;

    /* ── MOV direct,#imm ── */
    case 0x75:
        d = fetch(s); b8 = fetch(s);
        direct_write(s, d, b8);
        return 2;

    /* ── MOV @R0/#imm, @R1/#imm ── */
    case 0x76: case 0x77:
        b8 = fetch(s);
        indirect_write(s, *reg_ptr(s, op & 1), b8);
        return 1;

    /* ── MOV R0-R7,#imm ── */
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        *reg_ptr(s, op & 7) = fetch(s);
        return 1;

    /* ── SJMP rel ── */
    case 0x80:
        rel = (int8_t)fetch(s);
        s->pc += rel;
        return 2;

    /* ── ANL C,bit ── */
    case 0x82:
        d = fetch(s);
        if (!bit_read(s, d)) s->psw &= ~PSW_CY;
        return 2;

    /* ── MOVC A,@A+PC ── */
    case 0x83:
        s->acc = code_read(s, s->pc + s->acc);
        return 2;

    /* ── DIV AB ── */
    case 0x84:
        s->psw &= ~(PSW_CY | PSW_OV);
        if (s->b == 0) {
            s->psw |= PSW_OV;
        } else {
            tmp = s->acc / s->b;
            s->b = s->acc % s->b;
            s->acc = tmp;
        }
        return 4;

    /* ── MOV direct,direct ── */
    case 0x85:
        a = fetch(s); d = fetch(s);
        direct_write(s, d, direct_read(s, a));
        return 2;

    /* ── MOV direct,@R0 / @R1 ── */
    case 0x86: case 0x87:
        d = fetch(s);
        direct_write(s, d, indirect_read(s, *reg_ptr(s, op & 1)));
        return 2;

    /* ── MOV direct,R0-R7 ── */
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        d = fetch(s);
        direct_write(s, d, *reg_ptr(s, op & 7));
        return 2;

    /* ── MOV DPTR,#imm16 ── */
    case 0x90:
        fetch16_hi(s, &s->dptr);
        return 2;

    /* ── MOV bit,C ── */
    case 0x92:
        d = fetch(s);
        bit_write(s, d, (s->psw & PSW_CY) != 0);
        return 2;

    /* ── MOVC A,@A+DPTR ── */
    case 0x93:
        s->acc = code_read(s, s->dptr + s->acc);
        return 2;

    /* ── SUBB A,#imm ── */
    case 0x94:
        b8 = fetch(s);
        goto do_subb;

    /* ── SUBB A,direct ── */
    case 0x95:
        b8 = direct_read(s, fetch(s));
        goto do_subb;

    /* ── SUBB A,@R0 / @R1 ── */
    case 0x96: case 0x97:
        b8 = indirect_read(s, *reg_ptr(s, op & 1));
        goto do_subb;

    /* ── SUBB A,R0-R7 ── */
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        b8 = *reg_ptr(s, op & 7);
    do_subb:
        carry = (s->psw & PSW_CY) ? 1 : 0;
        result = (uint16_t)s->acc - b8 - carry;
        s->psw &= ~(PSW_CY | PSW_AC | PSW_OV);
        if (result > 0xFF) s->psw |= PSW_CY;
        if (((s->acc & 0x0F) < ((b8 & 0x0F) + carry))) s->psw |= PSW_AC;
        if (((s->acc ^ b8) & (s->acc ^ result) & 0x80)) s->psw |= PSW_OV;
        s->acc = (uint8_t)result;
        return 1;

    /* ── ORL C,/bit ── */
    case 0xA0:
        d = fetch(s);
        if (!bit_read(s, d)) s->psw |= PSW_CY;
        return 2;

    /* ── MOV C,bit ── */
    case 0xA2:
        d = fetch(s);
        if (bit_read(s, d)) s->psw |= PSW_CY; else s->psw &= ~PSW_CY;
        return 1;

    /* ── INC DPTR ── */
    case 0xA3:
        s->dptr++;
        return 2;

    /* ── MUL AB ── */
    case 0xA4:
        w = (uint16_t)s->acc * s->b;
        s->acc = (uint8_t)w;
        s->b = (uint8_t)(w >> 8);
        s->psw &= ~PSW_CY;
        if (s->b) s->psw |= PSW_OV; else s->psw &= ~PSW_OV;
        return 4;

    /* ── reserved ── */
    case 0xA5:
        return 1;

    /* ── MOV @R0,direct / @R1,direct ── */
    case 0xA6: case 0xA7:
        d = fetch(s);
        indirect_write(s, *reg_ptr(s, op & 1), direct_read(s, d));
        return 2;

    /* ── MOV R0-R7,direct ── */
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        d = fetch(s);
        *reg_ptr(s, op & 7) = direct_read(s, d);
        return 2;

    /* ── ANL C,/bit ── */
    case 0xB0:
        d = fetch(s);
        if (bit_read(s, d)) s->psw &= ~PSW_CY;
        return 2;

    /* ── CPL bit ── */
    case 0xB2:
        d = fetch(s);
        bit_write(s, d, !bit_read(s, d));
        return 1;

    /* ── CPL C ── */
    case 0xB3:
        s->psw ^= PSW_CY;
        return 1;

    /* ── CJNE A,#imm,rel ── */
    case 0xB4:
        b8 = fetch(s); rel = (int8_t)fetch(s);
        s->psw &= ~PSW_CY;
        if (s->acc < b8) s->psw |= PSW_CY;
        if (s->acc != b8) s->pc += rel;
        return 2;

    /* ── CJNE A,direct,rel ── */
    case 0xB5:
        d = fetch(s); rel = (int8_t)fetch(s);
        b8 = direct_read(s, d);
        s->psw &= ~PSW_CY;
        if (s->acc < b8) s->psw |= PSW_CY;
        if (s->acc != b8) s->pc += rel;
        return 2;

    /* ── CJNE @R0/#imm,rel / @R1/#imm,rel ── */
    case 0xB6: case 0xB7:
        b8 = fetch(s); rel = (int8_t)fetch(s);
        a = indirect_read(s, *reg_ptr(s, op & 1));
        s->psw &= ~PSW_CY;
        if (a < b8) s->psw |= PSW_CY;
        if (a != b8) s->pc += rel;
        return 2;

    /* ── CJNE R0-R7,#imm,rel ── */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        b8 = fetch(s); rel = (int8_t)fetch(s);
        a = *reg_ptr(s, op & 7);
        s->psw &= ~PSW_CY;
        if (a < b8) s->psw |= PSW_CY;
        if (a != b8) s->pc += rel;
        return 2;

    /* ── PUSH direct ── */
    case 0xC0:
        d = fetch(s);
        push8(s, direct_read(s, d));
        return 2;

    /* ── CLR bit ── */
    case 0xC2:
        bit_write(s, fetch(s), false);
        return 1;

    /* ── CLR C ── */
    case 0xC3:
        s->psw &= ~PSW_CY;
        return 1;

    /* ── SWAP A ── */
    case 0xC4:
        s->acc = (s->acc >> 4) | (s->acc << 4);
        return 1;

    /* ── XCH A,direct ── */
    case 0xC5:
        d = fetch(s);
        tmp = direct_read(s, d);
        direct_write(s, d, s->acc);
        s->acc = tmp;
        return 1;

    /* ── XCH A,@R0 / @R1 ── */
    case 0xC6: case 0xC7:
        a = *reg_ptr(s, op & 1);
        tmp = indirect_read(s, a);
        indirect_write(s, a, s->acc);
        s->acc = tmp;
        return 1;

    /* ── XCH A,R0-R7 ── */
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        tmp = *reg_ptr(s, op & 7);
        *reg_ptr(s, op & 7) = s->acc;
        s->acc = tmp;
        return 1;

    /* ── POP direct ── */
    case 0xD0:
        d = fetch(s);
        direct_write(s, d, pop8(s));
        return 2;

    /* ── SETB bit ── */
    case 0xD2:
        bit_write(s, fetch(s), true);
        return 1;

    /* ── SETB C ── */
    case 0xD3:
        s->psw |= PSW_CY;
        return 1;

    /* ── DA A ── */
    case 0xD4:
        if ((s->acc & 0x0F) > 9 || (s->psw & PSW_AC)) {
            if ((uint16_t)s->acc + 6 > 0xFF) s->psw |= PSW_CY;
            s->acc += 6;
        }
        if ((s->acc & 0xF0) > 0x90 || (s->psw & PSW_CY)) {
            if ((uint16_t)s->acc + 0x60 > 0xFF) s->psw |= PSW_CY;
            s->acc += 0x60;
        }
        return 1;

    /* ── DJNZ direct,rel ── */
    case 0xD5:
        d = fetch(s); rel = (int8_t)fetch(s);
        tmp = direct_read(s, d) - 1;
        direct_write(s, d, tmp);
        if (tmp != 0) s->pc += rel;
        return 2;

    /* ── XCHD A,@R0 / @R1 ── */
    case 0xD6: case 0xD7:
        a = *reg_ptr(s, op & 1);
        tmp = indirect_read(s, a);
        indirect_write(s, a, (tmp & 0xF0) | (s->acc & 0x0F));
        s->acc = (s->acc & 0xF0) | (tmp & 0x0F);
        return 1;

    /* ── DJNZ R0-R7,rel ── */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        rel = (int8_t)fetch(s);
        tmp = --(*reg_ptr(s, op & 7));
        if (tmp != 0) s->pc += rel;
        return 2;

    /* ── MOVX A,@DPTR ── */
    case 0xE0:
        s->acc = xdata_read(s, s->dptr);
        return 2;

    /* ── MOVX A,@R0 / @R1 (8-bit address, P2 as high byte; the AN2131 pages
     * with MPAGE, but neither firmware writes MPAGE or P2) ── */
    case 0xE2: case 0xE3:
        a = *reg_ptr(s, op & 1);
        w = ((uint16_t)direct_read(s, SFR_P2) << 8) | a;
        s->acc = xdata_read(s, w);
        return 2;

    /* ── CLR A ── */
    case 0xE4:
        s->acc = 0;
        return 1;

    /* ── MOV A,direct ── */
    case 0xE5:
        s->acc = direct_read(s, fetch(s));
        return 1;

    /* ── MOV A,@R0 / @R1 ── */
    case 0xE6: case 0xE7:
        s->acc = indirect_read(s, *reg_ptr(s, op & 1));
        return 1;

    /* ── MOV A,R0-R7 ── */
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        s->acc = *reg_ptr(s, op & 7);
        return 1;

    /* ── MOVX @DPTR,A ── */
    case 0xF0:
        xdata_write(s, s->dptr, s->acc);
        return 2;

    /* ── MOVX @R0,A / @R1,A (8-bit address, P2 as high byte, as above) ── */
    case 0xF2: case 0xF3:
        a = *reg_ptr(s, op & 1);
        w = ((uint16_t)direct_read(s, SFR_P2) << 8) | a;
        xdata_write(s, w, s->acc);
        return 2;

    /* ── CPL A ── */
    case 0xF4:
        s->acc = ~s->acc;
        return 1;

    /* ── MOV direct,A ── */
    case 0xF5:
        direct_write(s, fetch(s), s->acc);
        return 1;

    /* ── MOV @R0,A / @R1,A ── */
    case 0xF6: case 0xF7:
        indirect_write(s, *reg_ptr(s, op & 1), s->acc);
        return 1;

    /* ── MOV R0-R7,A ── */
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        *reg_ptr(s, op & 7) = s->acc;
        return 1;

    /* ── RET ── */
    case 0x22:
        s->pc = (uint16_t)pop8(s) << 8;
        s->pc |= pop8(s);
        return 2;

    /* ── RETI ── */
    case 0x32:
        s->pc = (uint16_t)pop8(s) << 8;
        s->pc |= pop8(s);
        s->in_interrupt = false;
        s->irq_recheck = true;
        return 2;

    /* ── JBC bit,rel ── */
    case 0x10:
        d = fetch(s); rel = (int8_t)fetch(s);
        if (bit_read(s, d)) {
            bit_write(s, d, false);
            s->pc += rel;
        }
        return 2;

    default:
        return 1;
    }
}

/* ── interrupt dispatch ────────────────────────────────────────────── */

void cpu8051_interrupt(Cpu8051State *s, uint8_t vector)
{
    s->in_interrupt = true;
    s->irq_recheck = true;
    s->events++;
    push8(s, (uint8_t)(s->pc));
    push8(s, (uint8_t)(s->pc >> 8));
    s->pc = vector;
}

/* ── init / reset ──────────────────────────────────────────────────── */

void cpu8051_init(Cpu8051State *s)
{
    memset(s, 0, sizeof(*s));
}

void cpu8051_reset(Cpu8051State *s)
{
    s->pc = 0;
    s->sp = 0x07;
    s->acc = 0;
    s->b = 0;
    s->dptr = 0;
    s->dptr_alt = 0;
    s->psw = 0;
    s->in_interrupt = false;
    memset(s->iram, 0, sizeof(s->iram));
    memset(s->sfr, 0, sizeof(s->sfr));
}

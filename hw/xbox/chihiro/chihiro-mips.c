/*
 * MIPS32 core of the AMD Alchemy Au1500 (Chihiro Type-3 network board)
 *
 * An interpreter of the Au1 core as the Data Book (30361D, chapter 2)
 * describes it: MIPS32 release 1 integer set, the multiply-add and count
 * leading zero instructions, no floating point, a software-managed TLB of
 * 32 dual entries with 36-bit page frames, the CP0 Count/Compare timer on
 * IP7, and the standard exception vectors. Little-endian: the board's
 * firmware image is little-endian from its first instruction, and its
 * reset code confirms the byte order by writing SYS_ENDIAN. LWL/LWR/SWL/SWR
 * follow the MIPS32 little-endian definition, as QEMU's target/mips does.
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
#include "chihiro-mips.h"

#define TLB_V 0x2u
#define TLB_D 0x4u
#define TLB_G 0x1u

/* Data Book 2.7.14: Au1 core, revision as the pb1500 BSP reports it. */
#define AU1_PRID   0x01030200u
/* Config: M=1 (Config1 exists), K0=3 cached, MT=1 standard TLB. */
#define AU1_CONFIG  (0x80000000u | (1u << 7) | 3u)
/* Config1 (Data Book 2.7.16): MMU size 31 (32 entries), 16 KB 4-way caches
 * with 32-byte lines: IS=1 IL=4 IA=3 DS=1 DL=4 DA=3, no FPU. The watch
 * registers and EJTAG are not modelled (the silicon has them: WR=EP=1). The
 * firmware never reads Config1. */
#define AU1_CONFIG1 ((31u << 25) | (1u << 22) | (4u << 19) | (3u << 16) | \
                     (1u << 13) | (4u << 10) | (3u << 7))

void mips_init(MipsState *s, void *opaque,
               uint32_t (*read)(void *, uint64_t, int),
               void (*write)(void *, uint64_t, int, uint32_t))
{
    memset(s, 0, sizeof(*s));
    s->opaque = opaque;
    s->read = read;
    s->write = write;
    mips_reset(s);
}

void mips_reset(MipsState *s)
{
    memset(s->r, 0, sizeof(s->r));
    s->hi = s->lo = 0;
    s->pc = 0xBFC00000u;
    s->in_delay = false;
    s->cp0_index = s->cp0_entrylo0 = s->cp0_entrylo1 = s->cp0_context = 0;
    s->cp0_random = MIPS_TLB_ENTRIES - 1;
    s->cp0_pagemask = s->cp0_wired = s->cp0_badvaddr = 0;
    s->cp0_count = s->cp0_compare = 0;
    s->cp0_entryhi = 0;
    /* Reset: BEV and ERL set, everything else cleared. */
    s->cp0_status = MIPS_ST_BEV | MIPS_ST_ERL;
    s->cp0_cause = 0;
    s->cp0_epc = s->cp0_errorepc = 0;
    s->cp0_prid = AU1_PRID;
    s->cp0_config = AU1_CONFIG;
    s->cp0_config1 = AU1_CONFIG1;
    s->cp0_lladdr = s->cp0_scratch = 0;
    s->cp0_debug = s->cp0_desave = s->cp0_watchlo = s->cp0_watchhi = 0;
    memset(s->tlb, 0, sizeof(s->tlb));
    s->ll_bit = false;
    s->halted = false;
    s->illegal = false;
    s->irq_lines = 0;
}

/* ── Address translation ─────────────────────────────────────────────── */

/* The TLB entry that maps va for the current ASID, or -1. */
static int tlb_match(MipsState *s, uint32_t va)
{
    uint8_t asid = s->cp0_entryhi & 0xFF;

    for (int i = 0; i < MIPS_TLB_ENTRIES; i++) {
        const MipsTlbEntry *e = &s->tlb[i];
        uint32_t pm = e->pagemask | 0x1FFFu;     /* bits below the VPN2 */
        if (((va ^ e->entryhi) & ~pm) == 0 &&
            ((e->lo0 & e->lo1 & TLB_G) || (e->entryhi & 0xFF) == asid))
            return i;
    }
    return -1;
}

/* Returns 0 with *pa set, or the exception code for the access. */
static int mips_map(MipsState *s, uint32_t va, bool store, uint64_t *pa)
{
    if (va >= 0x80000000u && va < 0xC0000000u) {
        /* kseg0 (cached) and kseg1 (uncached): the low 512 MB, no TLB. */
        *pa = va & 0x1FFFFFFFu;
        return 0;
    }
    /* kuseg and kseg2/3 go through the TLB; with ERL set, kuseg is unmapped
     * (MIPS32 4.6). */
    if (va < 0x80000000u && (s->cp0_status & MIPS_ST_ERL)) {
        *pa = va;
        return 0;
    }
    int i = tlb_match(s, va);
    if (i < 0) {
        /* No entry: a refill. Reported like a miss on an invalid entry, but
         * through the refill vector (see mips_exception). */
        return store ? -MIPS_EXC_TLBS : -MIPS_EXC_TLBL;
    }
    const MipsTlbEntry *e = &s->tlb[i];
    uint32_t page = ((e->pagemask | 0x1FFFu) + 1) >> 1;   /* even/odd select */
    uint32_t lo = (va & page) ? e->lo1 : e->lo0;
    if (!(lo & TLB_V))
        return store ? MIPS_EXC_TLBS : MIPS_EXC_TLBL;
    if (store && !(lo & TLB_D))
        return MIPS_EXC_MOD;
    *pa = (((uint64_t)(lo >> 6) << 12) & ~(uint64_t)(page - 1)) | (va & (page - 1));
    return 0;
}

/* ── Exceptions ──────────────────────────────────────────────────────── */

static void mips_exception(MipsState *s, int code, bool refill)
{
    uint32_t vector;

    if (!(s->cp0_status & MIPS_ST_EXL)) {
        if (s->in_delay) {
            s->cp0_epc = s->pc - 4;
            s->cp0_cause |= MIPS_CAUSE_BD;
        } else {
            s->cp0_epc = s->pc;
            s->cp0_cause &= ~MIPS_CAUSE_BD;
        }
        if (refill)
            vector = 0x000;
        else if (code == MIPS_EXC_INT && (s->cp0_cause & MIPS_CAUSE_IV))
            vector = 0x200;
        else
            vector = 0x180;
    } else {
        vector = 0x180;
    }
    s->cp0_cause = (s->cp0_cause & ~MIPS_CAUSE_EXC_MASK) | ((uint32_t)code << 2);
    s->cp0_status |= MIPS_ST_EXL;
    vector += (s->cp0_status & MIPS_ST_BEV) ? 0xBFC00200u : 0x80000000u;
    s->pc = vector;
    s->in_delay = false;
    s->halted = false;
    s->idle_hint = false;
    s->loop_count = 0;
}

/* A conditional trap that fires: the instruction ends in the exception. */
static bool mips_trap(MipsState *s)
{
    mips_exception(s, MIPS_EXC_TR, false);
    return true;
}

static void mips_address_exception(MipsState *s, int code, uint32_t va)
{
    bool refill = code < 0;
    if (refill)
        code = -code;
    if (code == MIPS_EXC_TLBL || code == MIPS_EXC_TLBS || code == MIPS_EXC_MOD) {
        s->cp0_badvaddr = va;
        s->cp0_entryhi = (s->cp0_entryhi & 0xFFu) | (va & 0xFFFFE000u);
        s->cp0_context = (s->cp0_context & 0xFF800000u) | ((va >> 9) & 0x007FFFF0u);
    } else {
        s->cp0_badvaddr = va;
    }
    mips_exception(s, code, refill);
}

/* ── Memory ──────────────────────────────────────────────────────────── */

static inline uint32_t bus_read(MipsState *s, uint64_t pa, int size)
{
    if (s->ram && pa + size <= s->ram_size) {
        return ldn_le_p(s->ram + pa, size);
    }
    return s->read(s->opaque, pa, size);
}

static inline void bus_write(MipsState *s, uint64_t pa, int size, uint32_t val)
{
    if (s->ram && pa + size <= s->ram_size) {
        stn_le_p(s->ram + pa, size, val);
        return;
    }
    if (size < 4) {
        /* A byte or halfword store drives its lane only. */
        val &= (1u << (size * 8)) - 1;
    }
    s->write(s->opaque, pa, size, val);
}

/* Loads through the TLB. Returns false after raising the exception. */
static bool mem_load(MipsState *s, uint32_t va, int size, uint32_t *val)
{
    uint64_t pa;
    s->loop_loaded = true;
    if (va & (size - 1)) {
        mips_address_exception(s, MIPS_EXC_ADEL, va);
        return false;
    }
    int exc = mips_map(s, va, false, &pa);
    if (exc) {
        mips_address_exception(s, exc, va);
        return false;
    }
    *val = bus_read(s, pa, size);
    return true;
}

static bool mem_store(MipsState *s, uint32_t va, int size, uint32_t val)
{
    uint64_t pa;
    s->loop_stored = true;
    if (va & (size - 1)) {
        mips_address_exception(s, MIPS_EXC_ADES, va);
        return false;
    }
    int exc = mips_map(s, va, true, &pa);
    if (exc) {
        mips_address_exception(s, exc, va);
        return false;
    }
    bus_write(s, pa, size, val);
    return true;
}

/* ── CP0 ─────────────────────────────────────────────────────────────── */

static uint32_t cp0_read(MipsState *s, int reg, int sel)
{
    switch (reg) {
    case 0:  return s->cp0_index;
    case 1:  return s->cp0_random;
    case 2:  return s->cp0_entrylo0;
    case 3:  return s->cp0_entrylo1;
    case 4:  return s->cp0_context;
    case 5:  return s->cp0_pagemask;
    case 6:  return s->cp0_wired;
    case 8:  return s->cp0_badvaddr;
    case 9:  return s->cp0_count;
    case 10: return s->cp0_entryhi;
    case 11: return s->cp0_compare;
    case 12: return s->cp0_status;
    case 13: return s->cp0_cause | s->irq_lines;   /* IP2-7 are the lines */
    case 14: return s->cp0_epc;
    case 15: return s->cp0_prid;
    case 16: return sel == 1 ? s->cp0_config1 : s->cp0_config;
    case 17: return s->cp0_lladdr;
    case 18: return s->cp0_watchlo;
    case 19: return s->cp0_watchhi;
    case 22: return s->cp0_scratch;
    case 23: return s->cp0_debug;
    case 30: return s->cp0_errorepc;
    case 31: return s->cp0_desave;
    default: return 0;
    }
}

static void cp0_write(MipsState *s, int reg, int sel, uint32_t val)
{
    switch (reg) {
    case 0:  s->cp0_index = val & (MIPS_TLB_ENTRIES - 1); break;
    case 2:  s->cp0_entrylo0 = val & 0x3FFFFFFFu; break;
    case 3:  s->cp0_entrylo1 = val & 0x3FFFFFFFu; break;
    case 4:  s->cp0_context = (s->cp0_context & 0x007FFFF0u) | (val & 0xFF800000u); break;
    case 5:  s->cp0_pagemask = val & 0x01FFE000u; break;
    case 6:  s->cp0_wired = val & (MIPS_TLB_ENTRIES - 1);
             s->cp0_random = MIPS_TLB_ENTRIES - 1; break;
    case 9:  s->cp0_count = val; break;
    case 10: s->cp0_entryhi = val & 0xFFFFE0FFu; break;
    case 11: s->cp0_compare = val;
             s->cp0_cause &= ~(1u << 15);          /* the timer's IP7 */
             break;
    case 12: s->cp0_status = val; break;
    case 13: s->cp0_cause = (s->cp0_cause & ~((3u << 8) | MIPS_CAUSE_IV)) |
                            (val & ((3u << 8) | MIPS_CAUSE_IV));
             break;
    case 14: s->cp0_epc = val; break;
    case 16: if (sel == 0)
                 s->cp0_config = (s->cp0_config & ~7u) | (val & 7u);
             break;
    case 18: s->cp0_watchlo = val; break;
    case 19: s->cp0_watchhi = val; break;
    case 22: s->cp0_scratch = val; break;
    case 23: s->cp0_debug = val; break;
    case 30: s->cp0_errorepc = val; break;
    case 31: s->cp0_desave = val; break;
    default: break;
    }
}

static void tlb_write(MipsState *s, int idx)
{
    MipsTlbEntry *e = &s->tlb[idx & (MIPS_TLB_ENTRIES - 1)];
    e->pagemask = s->cp0_pagemask;
    e->entryhi = s->cp0_entryhi & ~(s->cp0_pagemask | 0x1F00u);
    e->lo0 = s->cp0_entrylo0;
    e->lo1 = s->cp0_entrylo1;
}

static void tlb_read(MipsState *s)
{
    const MipsTlbEntry *e = &s->tlb[s->cp0_index & (MIPS_TLB_ENTRIES - 1)];
    s->cp0_pagemask = e->pagemask;
    s->cp0_entryhi = e->entryhi;
    s->cp0_entrylo0 = e->lo0;
    s->cp0_entrylo1 = e->lo1;
}

static void tlb_probe(MipsState *s)
{
    int i = tlb_match(s, s->cp0_entryhi);

    s->cp0_index = i >= 0 ? (uint32_t)i : 0x80000000u;
}

/* ── Interrupts and the timer ────────────────────────────────────────── */

void mips_set_irq(MipsState *s, uint32_t ip_mask, bool level)
{
    ip_mask &= 0xFCu << 8;
    if (level)
        s->irq_lines |= ip_mask;
    else
        s->irq_lines &= ~ip_mask;
}

static inline bool interrupt_pending(MipsState *s)
{
    uint32_t ip = (s->cp0_cause & MIPS_CAUSE_IP) | s->irq_lines;
    if ((s->cp0_status & (MIPS_ST_IE | MIPS_ST_EXL | MIPS_ST_ERL)) != MIPS_ST_IE)
        return false;
    return (ip & s->cp0_status & MIPS_ST_IM) != 0;
}

/* ── Execution ───────────────────────────────────────────────────────── */

#define RS(op)   (((op) >> 21) & 31)
#define RT(op)   (((op) >> 16) & 31)
#define RD(op)   (((op) >> 11) & 31)
#define SA(op)   (((op) >> 6) & 31)
#define IMM(op)  ((int32_t)(int16_t)((op) & 0xFFFF))
#define UIMM(op) ((op) & 0xFFFFu)
/* A branch's displacement in bytes, added as unsigned (no signed overflow). */
#define BOFF(op) ((uint32_t)IMM(op) << 2)

static inline void set_reg(MipsState *s, int n, uint32_t v)
{
    if (n) s->r[n] = v;
}

/* A taken branch: the instruction after this one runs, then control goes
 * to the target. A branch-likely that is not taken skips its slot. */
#define branch(s, target) do { next_branch = (target); next_branch_set = true; } while (0)
#define skip_slot(s)      do { after += 4; } while (0)
/* A conditional branch; the likely forms annul their slot when not taken. */
#define branch_if(cond, likely)                                              \
    do {                                                                     \
        if (cond) branch(s, after + BOFF(op));                               \
        else if (likely) skip_slot(s);                                       \
    } while (0)
#define JTARGET(op) ((after & 0xF0000000u) | (((op) & 0x03FFFFFFu) << 2))

static bool mips_step(MipsState *s)
{
    uint32_t op, va, val;
    uint64_t pa;
    int exc;
    bool was_delay = s->in_delay;
    uint32_t after = s->pc + 4;
    uint32_t next_branch = 0;
    bool next_branch_set = false;

    /* An interrupt is taken between instructions, never inside a delay slot
     * pair (the slot runs first, so EPC is the branch target and BD stays
     * clear). */
    if (!was_delay && interrupt_pending(s)) {
        mips_exception(s, MIPS_EXC_INT, false);
        return true;
    }
    if (s->halted) {
        s->cycles++;
        s->cp0_count++;
        if (s->cp0_count == s->cp0_compare)
            s->cp0_cause |= 1u << 15;
        return true;
    }

    /* Fetch */
    if (s->pc & 3) {
        mips_address_exception(s, MIPS_EXC_ADEL, s->pc);
        return true;
    }
    exc = mips_map(s, s->pc, false, &pa);
    if (exc) {
        mips_address_exception(s, exc, s->pc);
        return true;
    }
    op = bus_read(s, pa, 4);

    s->cycles++;
    s->cp0_count++;
    if (s->cp0_count == s->cp0_compare)
        s->cp0_cause |= 1u << 15;
    if (s->cp0_random > s->cp0_wired)
        s->cp0_random--;
    else
        s->cp0_random = MIPS_TLB_ENTRIES - 1;

    /* A delay slot's successor is the branch target; in_delay stays set while
     * the slot runs, so an exception in it reports the branch. */
    if (was_delay)
        after = s->branch_to;

    uint32_t rs = s->r[RS(op)], rt = s->r[RT(op)];
    int rd = RD(op), rtn = RT(op);

    switch (op >> 26) {
    case 0x00: /* SPECIAL */
        switch (op & 0x3F) {
        case 0x00: set_reg(s, rd, rt << SA(op)); break;                      /* SLL */
        case 0x02: set_reg(s, rd, rt >> SA(op)); break;                      /* SRL */
        case 0x03: set_reg(s, rd, (uint32_t)((int32_t)rt >> SA(op))); break; /* SRA */
        case 0x04: set_reg(s, rd, rt << (rs & 31)); break;                   /* SLLV */
        case 0x06: set_reg(s, rd, rt >> (rs & 31)); break;                   /* SRLV */
        case 0x07: set_reg(s, rd, (uint32_t)((int32_t)rt >> (rs & 31))); break; /* SRAV */
        case 0x08: branch(s, rs); break;                                     /* JR */
        case 0x09: set_reg(s, rd, after + 4); branch(s, rs); break;          /* JALR */
        case 0x0A: if (rt == 0) set_reg(s, rd, rs); break;                   /* MOVZ */
        case 0x0B: if (rt != 0) set_reg(s, rd, rs); break;                   /* MOVN */
        case 0x0C: mips_exception(s, MIPS_EXC_SYS, false); return true;      /* SYSCALL */
        case 0x0D: mips_exception(s, MIPS_EXC_BP, false); return true;       /* BREAK */
        case 0x0F: break;                                                    /* SYNC */
        case 0x10: set_reg(s, rd, s->hi); break;                             /* MFHI */
        case 0x11: s->hi = rs; break;                                        /* MTHI */
        case 0x12: set_reg(s, rd, s->lo); break;                             /* MFLO */
        case 0x13: s->lo = rs; break;                                        /* MTLO */
        case 0x18: {                                                         /* MULT */
            int64_t p = (int64_t)(int32_t)rs * (int32_t)rt;
            s->lo = (uint32_t)p; s->hi = (uint32_t)(p >> 32); break; }
        case 0x19: {                                                         /* MULTU */
            uint64_t p = (uint64_t)rs * rt;
            s->lo = (uint32_t)p; s->hi = (uint32_t)(p >> 32); break; }
        case 0x1A:                                                           /* DIV */
            if (rt == 0) {
                /* UNPREDICTABLE in the architecture; leave hi/lo as they are. */
            } else if (rs == 0x80000000u && rt == 0xFFFFFFFFu) {
                s->lo = 0x80000000u; s->hi = 0;
            } else {
                s->lo = (uint32_t)((int32_t)rs / (int32_t)rt);
                s->hi = (uint32_t)((int32_t)rs % (int32_t)rt);
            }
            break;
        case 0x1B:                                                           /* DIVU */
            if (rt != 0) { s->lo = rs / rt; s->hi = rs % rt; }
            break;
        case 0x20: {                                                         /* ADD */
            uint32_t r = rs + rt;
            if (~(rs ^ rt) & (rs ^ r) & 0x80000000u) {
                mips_exception(s, MIPS_EXC_OV, false); return true;
            }
            set_reg(s, rd, r); break; }
        case 0x21: set_reg(s, rd, rs + rt); break;                           /* ADDU */
        case 0x22: {                                                         /* SUB */
            uint32_t r = rs - rt;
            if ((rs ^ rt) & (rs ^ r) & 0x80000000u) {
                mips_exception(s, MIPS_EXC_OV, false); return true;
            }
            set_reg(s, rd, r); break; }
        case 0x23: set_reg(s, rd, rs - rt); break;                           /* SUBU */
        case 0x24: set_reg(s, rd, rs & rt); break;                           /* AND */
        case 0x25: set_reg(s, rd, rs | rt); break;                           /* OR */
        case 0x26: set_reg(s, rd, rs ^ rt); break;                           /* XOR */
        case 0x27: set_reg(s, rd, ~(rs | rt)); break;                        /* NOR */
        case 0x2A: set_reg(s, rd, (int32_t)rs < (int32_t)rt); break;         /* SLT */
        case 0x2B: set_reg(s, rd, rs < rt); break;                           /* SLTU */
        case 0x30: if ((int32_t)rs >= (int32_t)rt) return mips_trap(s); break; /* TGE */
        case 0x31: if (rs >= rt) return mips_trap(s); break;   /* TGEU */
        case 0x32: if ((int32_t)rs < (int32_t)rt) return mips_trap(s); break;  /* TLT */
        case 0x33: if (rs < rt) return mips_trap(s); break;    /* TLTU */
        case 0x34: if (rs == rt) return mips_trap(s); break;   /* TEQ */
        case 0x36: if (rs != rt) return mips_trap(s); break;   /* TNE */
        default: goto illegal;
        }
        break;

    case 0x01: /* REGIMM */
        switch (RT(op)) {
        case 0x00: branch_if((int32_t)rs < 0, false); break;          /* BLTZ */
        case 0x01: branch_if((int32_t)rs >= 0, false); break;         /* BGEZ */
        case 0x02: branch_if((int32_t)rs < 0, true); break;  /* BLTZL */
        case 0x03: branch_if((int32_t)rs >= 0, true); break; /* BGEZL */
        case 0x08: if ((int32_t)rs >= IMM(op)) return mips_trap(s); break;  /* TGEI */
        case 0x09: if (rs >= (uint32_t)IMM(op)) return mips_trap(s); break; /* TGEIU */
        case 0x0A: if ((int32_t)rs < IMM(op)) return mips_trap(s); break;   /* TLTI */
        case 0x0B: if (rs < (uint32_t)IMM(op)) return mips_trap(s); break;  /* TLTIU */
        case 0x0C: if (rs == (uint32_t)IMM(op)) return mips_trap(s); break; /* TEQI */
        case 0x0E: if (rs != (uint32_t)IMM(op)) return mips_trap(s); break; /* TNEI */
        case 0x10: set_reg(s, 31, after + 4);                           /* BLTZAL */
                   branch_if((int32_t)rs < 0, false); break;
        case 0x11: set_reg(s, 31, after + 4);                           /* BGEZAL */
                   branch_if((int32_t)rs >= 0, false); break;
        case 0x12: set_reg(s, 31, after + 4);                           /* BLTZALL */
                   branch_if((int32_t)rs < 0, true); break;
        case 0x13: set_reg(s, 31, after + 4);                           /* BGEZALL */
                   branch_if((int32_t)rs >= 0, true); break;
        default: goto illegal;
        }
        break;

    case 0x02: branch(s, JTARGET(op)); break;                 /* J */
    case 0x03: set_reg(s, 31, after + 4); branch(s, JTARGET(op)); break; /* JAL */
    case 0x04: branch_if(rs == rt, false); break;                             /* BEQ */
    case 0x05: branch_if(rs != rt, false); break;                             /* BNE */
    case 0x06: branch_if((int32_t)rs <= 0, false); break;                     /* BLEZ */
    case 0x07: branch_if((int32_t)rs > 0, false); break;                      /* BGTZ */
    case 0x08: {                                                        /* ADDI */
        uint32_t im = (uint32_t)IMM(op), r = rs + im;
        if (~(rs ^ im) & (rs ^ r) & 0x80000000u) {
            mips_exception(s, MIPS_EXC_OV, false);
            return true;
        }
        set_reg(s, rtn, r); break; }
    case 0x09: set_reg(s, rtn, rs + (uint32_t)IMM(op)); break;          /* ADDIU */
    case 0x0A: set_reg(s, rtn, (int32_t)rs < IMM(op)); break;           /* SLTI */
    case 0x0B: set_reg(s, rtn, rs < (uint32_t)IMM(op)); break;          /* SLTIU */
    case 0x0C: set_reg(s, rtn, rs & UIMM(op)); break;                   /* ANDI */
    case 0x0D: set_reg(s, rtn, rs | UIMM(op)); break;                   /* ORI */
    case 0x0E: set_reg(s, rtn, rs ^ UIMM(op)); break;                   /* XORI */
    case 0x0F: set_reg(s, rtn, UIMM(op) << 16); break;                  /* LUI */

    case 0x10: /* COP0 */
        if (op & (1u << 25)) {
            switch (op & 0x3F) {
            case 0x01: tlb_read(s); break;                                   /* TLBR */
            case 0x02: tlb_write(s, s->cp0_index); break;                    /* TLBWI */
            case 0x06: tlb_write(s, s->cp0_random); break;                   /* TLBWR */
            case 0x08: tlb_probe(s); break;                                  /* TLBP */
            case 0x18:                                                       /* ERET */
                if (s->cp0_status & MIPS_ST_ERL) {
                    s->pc = s->cp0_errorepc;
                    s->cp0_status &= ~MIPS_ST_ERL;
                } else {
                    s->pc = s->cp0_epc;
                    s->cp0_status &= ~MIPS_ST_EXL;
                }
                s->ll_bit = false;
                s->in_delay = false;
                return true;
            case 0x20: s->halted = true; break;                              /* WAIT */
            default: goto illegal;
            }
        } else {
            switch (RS(op)) {
            case 0x00: set_reg(s, rtn, cp0_read(s, rd, op & 7)); break;      /* MFC0 */
            case 0x04: cp0_write(s, rd, op & 7, rt); break;                  /* MTC0 */
            default: goto illegal;
            }
        }
        break;

    case 0x11: /* COP1: no floating point unit on the Au1 */
        s->cp0_cause = (s->cp0_cause & ~MIPS_CAUSE_CE) | (1u << 28);
        mips_exception(s, MIPS_EXC_CPU, false);
        return true;

    case 0x14: branch_if(rs == rt, true); break;         /* BEQL */
    case 0x15: branch_if(rs != rt, true); break;         /* BNEL */
    case 0x16: branch_if((int32_t)rs <= 0, true); break; /* BLEZL */
    case 0x17: branch_if((int32_t)rs > 0, true); break;  /* BGTZL */

    case 0x1C: /* SPECIAL2 */
        switch (op & 0x3F) {
        case 0x00: {                                                         /* MADD */
            uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
            acc += (uint64_t)((int64_t)(int32_t)rs * (int32_t)rt);
            s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); break; }
        case 0x01: {                                                         /* MADDU */
            uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
            acc += (uint64_t)rs * rt;
            s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); break; }
        case 0x02: set_reg(s, rd, (uint32_t)rs * (uint32_t)rt); break;          /* MUL */
        case 0x04: {                                                         /* MSUB */
            uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
            acc -= (uint64_t)((int64_t)(int32_t)rs * (int32_t)rt);
            s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); break; }
        case 0x05: {                                                         /* MSUBU */
            uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
            acc -= (uint64_t)rs * rt;
            s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); break; }
        case 0x20: set_reg(s, rd, rs ? __builtin_clz(rs) : 32); break;       /* CLZ */
        case 0x21: set_reg(s, rd, ~rs ? __builtin_clz(~rs) : 32); break;     /* CLO */
        case 0x3F: mips_exception(s, MIPS_EXC_BP, false); return true;       /* SDBBP */
        default: goto illegal;
        }
        break;

    case 0x20: va = rs + IMM(op); if (!mem_load(s, va, 1, &val)) return true;   /* LB */
               set_reg(s, rtn, (uint32_t)(int32_t)(int8_t)val); break;
    case 0x21: va = rs + IMM(op); if (!mem_load(s, va, 2, &val)) return true;   /* LH */
               set_reg(s, rtn, (uint32_t)(int32_t)(int16_t)val); break;
    case 0x22: {                                                                /* LWL */
        va = rs + IMM(op);
        uint32_t r = rt, b;
        int lmask = (va & 3) ^ 3;
        if (!mem_load(s, va, 1, &b)) return true;
        r = (r & 0x00FFFFFFu) | (b << 24);
        if (lmask <= 2) {
            if (!mem_load(s, va - 1, 1, &b)) return true;
            r = (r & 0xFF00FFFFu) | (b << 16);
        }
        if (lmask <= 1) {
            if (!mem_load(s, va - 2, 1, &b)) return true;
            r = (r & 0xFFFF00FFu) | (b << 8);
        }
        if (lmask == 0) {
            if (!mem_load(s, va - 3, 1, &b)) return true;
            r = (r & 0xFFFFFF00u) | b;
        }
        set_reg(s, rtn, r); break; }
    case 0x23: va = rs + IMM(op); if (!mem_load(s, va, 4, &val)) return true;   /* LW */
               set_reg(s, rtn, val); break;
    case 0x24: va = rs + IMM(op); if (!mem_load(s, va, 1, &val)) return true;   /* LBU */
               set_reg(s, rtn, val); break;
    case 0x25: va = rs + IMM(op); if (!mem_load(s, va, 2, &val)) return true;   /* LHU */
               set_reg(s, rtn, val); break;
    case 0x26: {                                                                /* LWR */
        va = rs + IMM(op);
        uint32_t r = rt, b;
        int lmask = (va & 3) ^ 3;
        if (!mem_load(s, va, 1, &b)) return true;
        r = (r & 0xFFFFFF00u) | b;
        if (lmask >= 1) {
            if (!mem_load(s, va + 1, 1, &b)) return true;
            r = (r & 0xFFFF00FFu) | (b << 8);
        }
        if (lmask >= 2) {
            if (!mem_load(s, va + 2, 1, &b)) return true;
            r = (r & 0xFF00FFFFu) | (b << 16);
        }
        if (lmask == 3) {
            if (!mem_load(s, va + 3, 1, &b)) return true;
            r = (r & 0x00FFFFFFu) | (b << 24);
        }
        set_reg(s, rtn, r); break; }
    case 0x28: if (!mem_store(s, rs + IMM(op), 1, rt)) return true; break;/* SB */
    case 0x29: if (!mem_store(s, rs + IMM(op), 2, rt)) return true; break;/* SH */
    case 0x2A: {                                                                /* SWL */
        va = rs + IMM(op);
        int lmask = (va & 3) ^ 3;
        if (!mem_store(s, va, 1, rt >> 24)) return true;
        if (lmask <= 2 && !mem_store(s, va - 1, 1, rt >> 16)) return true;
        if (lmask <= 1 && !mem_store(s, va - 2, 1, rt >> 8)) return true;
        if (lmask == 0 && !mem_store(s, va - 3, 1, rt)) return true;
        break; }
    case 0x2B: if (!mem_store(s, rs + IMM(op), 4, rt)) return true; break;/* SW */
    case 0x2E: {                                                                /* SWR */
        va = rs + IMM(op);
        int lmask = (va & 3) ^ 3;
        if (!mem_store(s, va, 1, rt)) return true;
        if (lmask >= 1 && !mem_store(s, va + 1, 1, rt >> 8)) return true;
        if (lmask >= 2 && !mem_store(s, va + 2, 1, rt >> 16)) return true;
        if (lmask == 3 && !mem_store(s, va + 3, 1, rt >> 24)) return true;
        break; }
    case 0x2F: break;                                                   /* CACHE */
    case 0x30: va = rs + IMM(op); if (!mem_load(s, va, 4, &val)) return true;   /* LL */
               set_reg(s, rtn, val); s->ll_bit = true; break;
    case 0x33: break;                                                           /* PREF */
    case 0x38: va = rs + IMM(op);                                                /* SC */
               if (s->ll_bit) {
                   if (!mem_store(s, va, 4, rt)) return true;
                   set_reg(s, rtn, 1);
               }
               else set_reg(s, rtn, 0);
               s->ll_bit = false; break;
    default:
        goto illegal;
    }

    s->pc = after;
    s->in_delay = next_branch_set;
    s->branch_to = next_branch;
    if (next_branch_set) {
        /* A backward branch over at most four instructions. */
        if (next_branch <= s->pc && s->pc - next_branch <= 16) {
            if (next_branch == s->loop_target && !s->loop_stored && s->loop_loaded) {
                if (++s->loop_count >= 256)
                    s->idle_hint = true;
            } else {
                s->loop_target = next_branch;
                s->loop_end = s->pc;
                s->loop_count = 0;
                s->idle_hint = false;
            }
        } else {
            s->loop_count = 0;
            s->idle_hint = false;
        }
        s->loop_stored = false;
        s->loop_loaded = false;
    }
    return true;

illegal:
    s->illegal = true;
    s->illegal_pc = s->pc;
    s->illegal_op = op;
    mips_exception(s, MIPS_EXC_RI, false);
    return false;
}

void mips_run(MipsState *s, uint64_t cycles)
{
    uint64_t end = s->cycles + cycles;
    while (s->cycles < end) {
        if (!mips_step(s))
            break;
        if (s->idle_hint && (s->pc < s->loop_target || s->pc > s->loop_end))
            s->idle_hint = false;       /* out of the loop */
        if ((s->halted || s->idle_hint) && !interrupt_pending(s)) {
            /* Sleep the rest of the budget away in one go; the timer keeps
             * counting so Compare still fires. */
            uint64_t left = end - s->cycles;
            uint32_t to_compare = s->cp0_compare - s->cp0_count;
            if (to_compare && to_compare <= left) {
                s->cycles += to_compare;
                s->cp0_count += to_compare;
                s->cp0_cause |= 1u << 15;
            } else {
                s->cycles += left;
                s->cp0_count += (uint32_t)left;
            }
        }
    }
}

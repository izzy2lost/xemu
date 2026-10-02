/*
 * MIPS32 core of the AMD Alchemy Au1500, the processor of the Chihiro
 * Type-3 network board
 *
 * The core only (see chihiro-mips.c); the board around it is
 * chihiro-netboard.c, which supplies the bus callbacks below.
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
#ifndef HW_CHIHIRO_MIPS_H
#define HW_CHIHIRO_MIPS_H

#include <stdint.h>
#include <stdbool.h>

#define MIPS_TLB_ENTRIES 32

/* CP0 Status bits */
#define MIPS_ST_IE   (1u << 0)
#define MIPS_ST_EXL  (1u << 1)
#define MIPS_ST_ERL  (1u << 2)
#define MIPS_ST_IM   (0xFFu << 8)
#define MIPS_ST_BEV  (1u << 22)

/* CP0 Cause bits */
#define MIPS_CAUSE_EXC_MASK  (0x1Fu << 2)
#define MIPS_CAUSE_IP        (0xFFu << 8)
#define MIPS_CAUSE_IV        (1u << 23)
#define MIPS_CAUSE_CE        (3u << 28)
#define MIPS_CAUSE_BD        (1u << 31)

/* Exception codes (Cause.ExcCode) */
enum {
    MIPS_EXC_INT  = 0,
    MIPS_EXC_MOD  = 1,
    MIPS_EXC_TLBL = 2,
    MIPS_EXC_TLBS = 3,
    MIPS_EXC_ADEL = 4,
    MIPS_EXC_ADES = 5,
    MIPS_EXC_SYS  = 8,
    MIPS_EXC_BP   = 9,
    MIPS_EXC_RI   = 10,
    MIPS_EXC_CPU  = 11,
    MIPS_EXC_OV   = 12,
    MIPS_EXC_TR   = 13,
};

typedef struct {
    uint32_t pagemask;   /* bits 28:13 */
    uint32_t entryhi;    /* VPN2 (31:13) | ASID (7:0) */
    uint32_t lo0, lo1;   /* PFN (29:6) | C (5:3) | D (2) | V (1) | G (0) */
} MipsTlbEntry;

typedef struct MipsState MipsState;

struct MipsState {
    uint32_t r[32];      /* r0 reads as zero */
    uint32_t hi, lo;
    uint32_t pc;
    /* A branch or jump is taken after the instruction that follows it. */
    bool     in_delay;   /* the instruction at pc sits in a delay slot */
    uint32_t branch_to;  /* where the slot's successor goes */

    /* CP0 */
    uint32_t cp0_index, cp0_random, cp0_entrylo0, cp0_entrylo1, cp0_context;
    uint32_t cp0_pagemask, cp0_wired, cp0_badvaddr, cp0_count, cp0_entryhi;
    uint32_t cp0_compare, cp0_status, cp0_cause, cp0_epc, cp0_prid;
    uint32_t cp0_config, cp0_config1, cp0_lladdr, cp0_scratch, cp0_errorepc;
    uint32_t cp0_debug, cp0_desave, cp0_watchlo, cp0_watchhi;
    MipsTlbEntry tlb[MIPS_TLB_ENTRIES];

    bool     ll_bit;     /* LL/SC link */

    /* Clock cycles. HEURISTIC: one per instruction, the pipeline's nominal
     * rate; loads, multiplies and taken branches cost more on the silicon. */
    uint64_t cycles;
    bool     halted;     /* WAIT: sleeps until an interrupt is taken */
    /* HEURISTIC: a short backward loop with no store, spun many times, is a
     * poll (VxWorks idles on workQIsEmpty); time then passes without running
     * it, until an interrupt or the loop's exit ends the hint. */
    bool     idle_hint;
    uint32_t loop_target, loop_end;      /* its delay slot included */
    uint32_t loop_count;
    bool     loop_stored, loop_loaded;   /* a poll loads and never stores */
    bool     illegal;    /* an opcode did not decode: fail loud */
    uint32_t illegal_pc, illegal_op;

    /* Hardware interrupt lines, IP2..IP7 as a mask of Cause.IP bits held
     * at level by the board. */
    uint32_t irq_lines;

    void *opaque;
    /* 36-bit physical bus. size is 1, 2 or 4; the value is little-endian. */
    uint32_t (*read)(void *opaque, uint64_t paddr, int size);
    void (*write)(void *opaque, uint64_t paddr, int size, uint32_t val);
    /* Direct window on the SDRAM: physical 0 .. ram_size-1. Fetches and
     * data accesses inside it skip the callbacks. NULL disables it. */
    uint8_t *ram;
    uint32_t ram_size;
};

void mips_init(MipsState *s, void *opaque,
               uint32_t (*read)(void *, uint64_t, int),
               void (*write)(void *, uint64_t, int, uint32_t));
void mips_reset(MipsState *s);
/* Runs for `cycles` clock cycles; WAIT and a detected poll loop sleep through
 * them; returns early only on an opcode that does not decode. */
void mips_run(MipsState *s, uint64_t cycles);
/* Board interrupt lines: a mask of Cause.IP bits (bit 10 = IP2 ... bit 15 =
 * IP7). Held at level until lowered. */
void mips_set_irq(MipsState *s, uint32_t ip_mask, bool level);

#endif

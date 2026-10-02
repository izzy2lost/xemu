/*
 * NEC V850E / V850ES interpreter — the Chihiro Type-3 media board CPU
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
 * The ASIC on a Type-3 media board carries this core. The game uploads an
 * encrypted image to 0x84800000, writes 1 to 0x80000140, and from then on the
 * firmware answers the mailbox at 0x84000000/0x84000020 by itself. The images
 * use the V850E/ES instruction set and nothing from the V850E2.
 *
 * Memory, as the firmware sees it:
 *   0x00000000-0x00017FFF  the uploaded image (tp = 0x118 points at a config table)
 *   0x0FFF0000-0x0FFFFFFF  work RAM (ep base; gp = 0x0FFFF000)
 *   0xFFFF8000-0xFFFFEFFF  internal RAM — the RTOS scheduler lives at 0xFFFF8010+
 *   0xFFFFF000-0xFFFFFFFF  on-chip peripherals (19 registers in use)
 */
#ifndef CHIHIRO_V850_H
#define CHIHIRO_V850_H

#include <stdint.h>
#include <stdbool.h>

/* Program space is 16 MB: PC bits 0-23, bit 0 always 0 (NEC manual 3.1, 4.1).
 * EIPC and FEPC keep bits 0-23, bit 0 included. */
#define V850_PC_MASK  0x00FFFFFEu
#define V850_EPC_MASK 0x00FFFFFFu

/* PSW bits */
#define V850_PSW_Z   (1u << 0)
#define V850_PSW_S   (1u << 1)
#define V850_PSW_OV  (1u << 2)
#define V850_PSW_CY  (1u << 3)
#define V850_PSW_SAT (1u << 4)
#define V850_PSW_ID  (1u << 5)
#define V850_PSW_EP  (1u << 6)
#define V850_PSW_NP  (1u << 7)

typedef struct V850State V850State;

struct V850State {
    uint32_t r[32];     /* r0 reads as zero; r3=sp, r4=gp, r5=tp, r30=ep, r31=lp */
    uint32_t pc;

    /* The system registers the firmware uses. */
    uint32_t eipc, eipsw, fepc, fepsw, ecr, psw, ctpc, ctpsw, ctbp;

    /* Clock cycles, not instructions. Table 5-10 of the NEC V850 Family
     * manual (U10243EJ3V0UM00, pages 90-92) prices the base set:
     *   JMP [R], JR, JARL 3; Bcond taken 3, not taken 1; SET1/CLR1/NOT1 4,
     *   TST1 3; TRAP, RETI 4; DIVH 36; LDSR to EIPC/FEPC 3; the rest 1.
     * HEURISTIC: the V850E instructions are not in that table and are priced
     * after the base instruction each extends (each site says which); the
     * V850E1 manual (U14559) would settle them. */
    uint64_t cycles;
    bool halted;
    /* Level-held requests, taken at the first instruction boundary where
     * PSW.ID allows. Bit n is the maskable vector 0x80 + 0x10 * n. */
    uint32_t irq_pending;
    bool illegal;       /* set when an opcode does not decode: fail loud */
    uint32_t illegal_pc;

    void *opaque;
    uint32_t (*read)(void *opaque, uint32_t addr, int size);   /* size 1/2/4 */
    void (*write)(void *opaque, uint32_t addr, int size, uint32_t val);

    /* Optional flat read-only view of program space: fetches read it instead
     * of calling read(). NULL: every fetch takes the callback. */
    const uint8_t *code;
    uint32_t code_size;
};

void v850_init(V850State *s, void *opaque,
               uint32_t (*read)(void *, uint32_t, int),
               void (*write)(void *, uint32_t, int, uint32_t));
void v850_reset(V850State *s);
/* Executes one instruction. Returns its length in bytes, 0 if it did not
 * decode (and sets s->illegal) or the core is halted. */
int  v850_step(V850State *s);
/* Requests a maskable interrupt and holds it until the core can take it. */
void v850_raise(V850State *s, uint32_t vector);

#endif

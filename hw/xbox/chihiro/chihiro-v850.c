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
 * The decode follows binutils (opcodes/v850-opc.c), MAME's V850 disassembler
 * (BSD-3-Clause, by AJR) and the NEC V850ES manual (U15943EJ). An encoding it
 * does not know stops the core and says where.
 */
#include "qemu/osdep.h"
#include "chihiro-v850.h"

#define SIGN8(x)   ((int32_t)(int8_t)(x))
#define SIGN16(x)  ((int32_t)(int16_t)(x))
#define BITS(w, p, n) (((w) >> (p)) & ((1u << (n)) - 1))

static inline uint32_t rd(V850State *s, int n) { return n ? s->r[n] : 0; }
static inline void wr(V850State *s, int n, uint32_t v) { if (n) s->r[n] = v; }

static inline uint32_t mem_r(V850State *s, uint32_t a, int sz)
{
    return s->read(s->opaque, a, sz);
}
static inline void mem_w(V850State *s, uint32_t a, int sz, uint32_t v)
{
    s->write(s->opaque, a, sz, v);
}

/* The bus maps the image, read-only once uploaded, to exactly this array, so
 * a fetch reads it straight. */
static inline uint16_t fetch16(V850State *s, uint32_t a)
{
    if (a < s->code_size && a + 1 < s->code_size)
        return (uint16_t)(s->code[a] | ((uint16_t)s->code[a + 1] << 8));
    return (uint16_t)mem_r(s, a, 2);
}

/* --- flags ------------------------------------------------------------- */

static void set_zs(V850State *s, uint32_t v)
{
    s->psw &= ~(V850_PSW_Z | V850_PSW_S);
    if (v == 0) s->psw |= V850_PSW_Z;
    if (v & 0x80000000u) s->psw |= V850_PSW_S;
}

/* A logic operation or a shift: Z and S from the result, OV cleared. */
static void set_logic(V850State *s, uint32_t v)
{
    set_zs(s, v);
    s->psw &= ~V850_PSW_OV;
}

/* shr, sar or shl by 0-31, from an immediate or a register: CY is the last
 * bit shifted out, none for a shift by zero. */
enum { SHIFT_SHR, SHIFT_SAR, SHIFT_SHL };

static void do_shift(V850State *s, int kind, uint32_t n, int r2)
{
    uint32_t a = rd(s, r2), v, out;

    switch (kind) {
    case SHIFT_SHR:
        v = n ? a >> n : a;
        out = n ? (a >> (n - 1)) & 1 : 0;
        break;
    case SHIFT_SAR:
        v = n ? (uint32_t)((int32_t)a >> n) : a;
        out = n ? (a >> (n - 1)) & 1 : 0;
        break;
    default:
        v = n ? a << n : a;
        out = n ? (a >> (32 - n)) & 1 : 0;
        break;
    }
    s->psw = (s->psw & ~V850_PSW_CY) | (out ? V850_PSW_CY : 0);
    wr(s, r2, v);
    set_logic(s, v);
}

static void set_add(V850State *s, uint32_t a, uint32_t b, uint32_t r)
{
    set_zs(s, r);
    s->psw &= ~(V850_PSW_CY | V850_PSW_OV);
    if (r < a) s->psw |= V850_PSW_CY;
    if (~(a ^ b) & (a ^ r) & 0x80000000u) s->psw |= V850_PSW_OV;
}

static void set_sub(V850State *s, uint32_t a, uint32_t b, uint32_t r)
{
    /* r = a - b */
    set_zs(s, r);
    s->psw &= ~(V850_PSW_CY | V850_PSW_OV);
    if (a < b) s->psw |= V850_PSW_CY;
    if ((a ^ b) & (a ^ r) & 0x80000000u) s->psw |= V850_PSW_OV;
}

/* Saturated add and subtract (NEC U15943EJ, SATADD/SATSUB): CY and OV come from
 * the plain 32-bit operation, S and Z from the saturated result, SAT is sticky. */
static uint32_t sat_add(V850State *s, uint32_t a, uint32_t b)
{
    uint32_t r = a + b;
    set_add(s, a, b, r);
    if (s->psw & V850_PSW_OV) {
        r = (a & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
        s->psw |= V850_PSW_SAT;
        set_zs(s, r);
    }
    return r;
}

static uint32_t sat_sub(V850State *s, uint32_t a, uint32_t b)
{
    uint32_t r = a - b;
    set_sub(s, a, b, r);
    if (s->psw & V850_PSW_OV) {
        r = (a & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
        s->psw |= V850_PSW_SAT;
        set_zs(s, r);
    }
    return r;
}

/* reg3 || reg2 <- reg2 x operand; the high word is written last (MUL/MULU). */
static void do_multiply(V850State *s, uint32_t operand, bool is_signed,
                        int r2, int r3)
{
    uint64_t p = is_signed
        ? (uint64_t)((int64_t)(int32_t)rd(s, r2) * (int32_t)operand)
        : (uint64_t)rd(s, r2) * operand;
    wr(s, r2, (uint32_t)p);
    wr(s, r3, (uint32_t)(p >> 32));
}

/* DIV/DIVH/DIVU/DIVHU: a zero divisor sets OV and leaves the registers alone,
 * 0x80000000 / -1 gives 0x80000000 with OV, and the remainder is written last,
 * so reg2 == reg3 keeps the remainder. r3 < 0: the two-operand DIVH. */
static void do_divide(V850State *s, uint32_t dividend, uint32_t divisor,
                      bool is_signed, int r2, int r3)
{
    uint32_t q, rem;

    if (divisor == 0) {
        s->psw |= V850_PSW_OV;
        return;
    }
    if (is_signed && dividend == 0x80000000u && divisor == 0xFFFFFFFFu) {
        q = 0x80000000u;
        rem = 0;
        s->psw |= V850_PSW_OV;
    } else if (is_signed) {
        q = (uint32_t)((int32_t)dividend / (int32_t)divisor);
        rem = (uint32_t)((int32_t)dividend % (int32_t)divisor);
        s->psw &= ~V850_PSW_OV;
    } else {
        q = dividend / divisor;
        rem = dividend % divisor;
        s->psw &= ~V850_PSW_OV;
    }
    wr(s, r2, q);
    if (r3 >= 0) {
        wr(s, r3, rem);
    }
    set_zs(s, q);
}

static bool cond_true(V850State *s, int c)
{
    bool z  = !!(s->psw & V850_PSW_Z);
    bool sg = !!(s->psw & V850_PSW_S);
    bool ov = !!(s->psw & V850_PSW_OV);
    bool cy = !!(s->psw & V850_PSW_CY);
    bool sa = !!(s->psw & V850_PSW_SAT);

    switch (c) {
    case 0x0: return ov;                 /* bv  */
    case 0x1: return cy;                 /* bc  / bl  */
    case 0x2: return z;                  /* be  */
    case 0x3: return cy || z;            /* bnh */
    case 0x4: return sg;                 /* bn  */
    case 0x5: return true;               /* br  */
    case 0x6: return sg != ov;           /* blt */
    case 0x7: return (sg != ov) || z;    /* ble */
    case 0x8: return !ov;                /* bnv */
    case 0x9: return !cy;                /* bnc / bnl */
    case 0xA: return !z;                 /* bne */
    case 0xB: return !(cy || z);         /* bh  */
    case 0xC: return !sg;                /* bp  */
    case 0xD: return sa;                 /* bsa */
    case 0xE: return sg == ov;           /* bge */
    case 0xF: return !((sg != ov) || z); /* bgt */
    }
    return false;
}

/* --- system registers --------------------------------------------------- */

static uint32_t sysreg_r(V850State *s, int n)
{
    switch (n) {
    case 0:  return s->eipc;
    case 1:  return s->eipsw;
    case 2:  return s->fepc;
    case 3:  return s->fepsw;
    case 4:  return s->ecr;
    case 5:  return s->psw;
    case 16: return s->ctpc;
    case 17: return s->ctpsw;
    case 20: return s->ctbp;
    default: return 0;
    }
}

static void sysreg_w(V850State *s, int n, uint32_t v)
{
    /* NEC manual 3.1: bits 24-31 of EIPC and FEPC are fixed to 0, and bits
     * 8-31 of EIPSW and FEPSW likewise. */
    switch (n) {
    case 0:  s->eipc = v & V850_EPC_MASK;  break;
    case 1:  s->eipsw = v & 0xFFu; break;
    case 2:  s->fepc = v & V850_EPC_MASK;  break;
    case 3:  s->fepsw = v & 0xFFu; break;
    case 5:  s->psw = v;   break;
    case 16: s->ctpc = v;  break;
    case 17: s->ctpsw = v; break;
    case 20: s->ctbp = v;  break;
    default: break;
    }
}

/* --- public ------------------------------------------------------------- */

void v850_init(V850State *s, void *opaque,
               uint32_t (*read)(void *, uint32_t, int),
               void (*write)(void *, uint32_t, int, uint32_t))
{
    memset(s, 0, sizeof(*s));
    s->opaque = opaque;
    s->read = read;
    s->write = write;
    v850_reset(s);
}

void v850_reset(V850State *s)
{
    memset(s->r, 0, sizeof(s->r));
    s->pc = 0;
    s->psw = V850_PSW_ID;                      /* reset value 00000020H */
    s->eipc = s->eipsw = s->fepc = s->fepsw = 0;
    s->ecr = s->ctpc = s->ctpsw = s->ctbp = 0;
    s->halted = false;
    s->illegal = false;
    s->illegal_pc = 0;
    s->cycles = 0;
}

/* Bit n of irq_pending is the maskable vector 0x80 + 0x10 * n. */
#define IRQ_BIT(v)  (((v) >= 0x80 && (v) <= 0x80 + 0x10 * 31 && ((v) & 0xF) == 0) \
                     ? (1u << (((v) - 0x80) >> 4)) : 0u)

/* Takes an interrupt now, saving PC and PSW; the caller has checked PSW.ID. */
static void v850_interrupt(V850State *s, uint32_t vector)
{
    s->eipc = s->pc;
    s->eipsw = s->psw;
    s->ecr = (s->ecr & 0xFFFF0000u) | (vector & 0xFFFF);
    /* A maskable interrupt sets ID and clears EP; only TRAP and the exceptions
     * set EP (NEC manual, TRAP and RETI). */
    s->psw |= V850_PSW_ID;
    s->psw &= ~V850_PSW_EP;
    s->pc = vector;
    s->halted = false;
}

/* The board raises only maskable vectors (the tick and the mailbox). */
void v850_raise(V850State *s, uint32_t vector)
{
    s->irq_pending |= IRQ_BIT(vector);
}

/* Takes the lowest-numbered held request, if any, once interrupts are open. */
static void v850_service_irq(V850State *s)
{
    if (!s->irq_pending || (s->psw & V850_PSW_ID)) return;
    int n = __builtin_ctz(s->irq_pending);
    uint32_t vector = 0x80u + 0x10u * (uint32_t)n;
    s->irq_pending &= ~(1u << n);
    v850_interrupt(s, vector);
}

/* The V850E register list (binutils v850-opc.c, list12): bit 11 of the
 * 12-bit field is r20, bit 0 is r31. */
static uint16_t list12_of(uint16_t w1, uint16_t w2)
{
    return (uint16_t)((w2 & 0x0f00) | ((w2 & 0xf000) >> 8) | ((w2 & 0x00c0) >> 4)
                      | ((w1 & 0x0001) << 1) | ((w2 & 0x0020) >> 5));
}

/* prepare list12, imm5 [, ep] — pushes the list, opens a frame, may set ep. */
static int do_prepare(V850State *s, uint16_t w1, uint16_t w2, uint32_t pc)
{
    uint16_t list = list12_of(w1, w2);
    uint32_t imm5 = BITS(w1, 1, 5);
    int len = 4;

    /* HEURISTIC: V850E, one clock per register stored. */
    s->cycles += __builtin_popcount(list);

    /* Highest register first, so dispose restores in mirror order. */
    for (int b = 0; b < 12; b++) {
        if (list & (1u << b)) {
            int reg = 20 + (11 - b);
            s->r[3] -= 4;
            mem_w(s, s->r[3], 4, rd(s, reg));
        }
    }
    s->r[3] -= imm5 << 2;

    switch (w2 & 0x001a) {
    case 0x00: break;                                   /* ep unchanged */
    case 0x02: s->r[30] = s->r[3]; break;               /* sp to ep      */
    case 0x0A: s->r[30] = (uint32_t)SIGN16(fetch16(s, pc + 4)); len = 6; break;
    case 0x12: s->r[30] = (uint32_t)fetch16(s, pc + 4) << 16; len = 6; break;
    case 0x1A: s->r[30] = fetch16(s, pc + 4) | ((uint32_t)fetch16(s, pc + 6) << 16);
               len = 8; break;
    default: break;
    }
    s->pc = pc + len;
    return len;
}

/* dispose imm5, list12 [, reg] — closes the frame, pops, may return. */
static int do_dispose(V850State *s, uint16_t w1, uint16_t w2, uint32_t pc)
{
    uint16_t list = list12_of(w1, w2);
    uint32_t imm5 = BITS(w1, 1, 5);
    int jump = BITS(w2, 0, 5);

    /* HEURISTIC: V850E, one clock per register loaded, plus a jump's three. */
    s->cycles += __builtin_popcount(list) + (jump ? 2 : 0);

    s->r[3] += imm5 << 2;
    for (int b = 11; b >= 0; b--) {
        if (list & (1u << b)) {
            int reg = 20 + (11 - b);
            wr(s, reg, mem_r(s, s->r[3], 4));
            s->r[3] += 4;
        }
    }
    if (jump) { s->pc = rd(s, jump) & ~1u; return 4; }
    s->pc = pc + 4;
    return 4;
}

/* callt imm6 — a call through the table at ctbp. */
static int do_callt(V850State *s, uint16_t w, uint32_t pc)
{
    uint32_t imm6 = BITS(w, 0, 6);
    s->cycles += 3;            /* HEURISTIC: V850E, priced like TRAP */
    s->ctpc = pc + 2;
    s->ctpsw = s->psw;
    uint32_t off = mem_r(s, s->ctbp + (imm6 << 1), 2);
    s->pc = s->ctbp + off;
    return 2;
}

/* --- the interpreter ---------------------------------------------------- */

static int step_sub_000000(V850State *s, uint16_t w, uint32_t pc);
static int step_sub_0001(V850State *s, uint16_t w);
static int step_extended(V850State *s, uint16_t w, uint16_t w2, uint32_t pc);

static int v850_execute(V850State *s);

int v850_step(V850State *s)
{
    int len = v850_execute(s);
    s->pc &= V850_PC_MASK;
    return len;
}

static int v850_execute(V850State *s)
{
    if (s->illegal) return 0;

    /* A held request is taken at the boundary; that is also how a halt ends. */
    v850_service_irq(s);
    if (s->halted) return 0;

    /* NEC 3.1: PC bits 31-24 read as zero and bit 0 is fixed; the firmware
     * keeps a handler pointer as 0x040015AC. */
    s->pc &= V850_PC_MASK;
    uint32_t pc = s->pc;
    uint16_t w = fetch16(s, pc);
    int r1 = BITS(w, 0, 5);
    int r2 = BITS(w, 11, 5);
    int top = BITS(w, 7, 4);
    int len = 2;

    /* One clock (Table 5-10); dearer instructions add the rest at their site. */
    s->cycles++;

    if (top == 0x0) {
        if (w & (1u << 6)) {
            if (w & (1u << 5)) {
                /* opcode 000011: jmp [reg1], or V850E sld.bu / sld.hu */
                if (r2 == 0) { s->cycles += 2;      /* jmp: 3 */
                               s->pc = rd(s, r1) & ~1u; return 2; }
                uint32_t ep = rd(s, 30);
                if (w & (1u << 4)) {     /* sld.hu: bits 3-0 are disp5 >> 1 */
                    wr(s, r2, (uint16_t)mem_r(s, ep + (BITS(w, 0, 4) << 1), 2));
                } else {                 /* sld.bu: bits 3-0 are disp4 */
                    wr(s, r2, (uint8_t)mem_r(s, ep + BITS(w, 0, 4), 1));
                }
                s->pc = pc + 2;
                return 2;
            }
            /* opcode 000010: divh reg1, reg2, or V850E switch reg1 */
            if (r2 == 0) {                       /* switch */
                s->cycles += 2;        /* HEURISTIC: V850E, priced as a jump */
                uint32_t idx = rd(s, r1);
                uint32_t base = pc + 2;
                int16_t off = (int16_t)mem_r(s, base + idx * 2, 2);
                s->pc = base + (uint32_t)(off * 2);
                return 2;
            }
            if (r1 == 0) {          /* not divh: dbtrap (0xF840) or unused */
                s->illegal = true;
                s->illegal_pc = pc;
                return 0;
            }
            s->cycles += 35;                        /* divh: 36 */
            do_divide(s, rd(s, r2), (uint32_t)SIGN16(rd(s, r1)), true, r2, -1);
            s->pc = pc + 2;
            return 2;
        }
        if (w & (1u << 5)) {                     /* not reg1, reg2 */
            uint32_t v = ~rd(s, r1);
            wr(s, r2, v);
            set_logic(s, v);
            s->pc = pc + 2;
            return 2;
        }
        return step_sub_000000(s, w, pc);
    }
    if (top == 0x1) {
        return step_sub_0001(s, w);
    }

    /* register-register group: or xor and tst subr sub add cmp */
    if (top == 0x2 || top == 0x3) {
        uint32_t a = rd(s, r2), b = rd(s, r1), v;
        switch (BITS(w, 5, 3)) {
        case 0: v = a | b;  wr(s, r2, v); set_logic(s, v); break;
        case 1: v = a ^ b;  wr(s, r2, v); set_logic(s, v); break;
        case 2: v = a & b;  wr(s, r2, v); set_logic(s, v); break;
        case 3: v = a & b;  set_logic(s, v); break;   /* tst */
        case 4: v = b - a;  set_sub(s, b, a, v); wr(s, r2, v); break;      /* subr */
        case 5: v = a - b;  set_sub(s, a, b, v); wr(s, r2, v); break;      /* sub  */
        case 6: v = a + b;  set_add(s, a, b, v); wr(s, r2, v); break;      /* add  */
        case 7: v = a - b;  set_sub(s, a, b, v); break;                    /* cmp  */
        }
        s->pc = pc + 2;
        return 2;
    }

    /* imm5 group: mov satadd add cmp shr sar shl mulh */
    if (top == 0x4 || top == 0x5) {
        if ((w & 0xf8c0) == 0) return do_callt(s, w, pc);
        /* sign-extend 5 bits */
        int32_t imm = (int32_t)(int8_t)(BITS(w, 0, 5) << 3) >> 3;
        uint32_t u = BITS(w, 0, 5);
        uint32_t a = rd(s, r2), v;
        switch (BITS(w, 5, 3)) {
        case 0: wr(s, r2, (uint32_t)imm); break;                          /* mov  */
        case 1: v = sat_add(s, a, (uint32_t)imm); wr(s, r2, v); break;   /* satadd */
        case 2: v = a + (uint32_t)imm; set_add(s, a, (uint32_t)imm, v);
                wr(s, r2, v); break;
        case 3: v = a - (uint32_t)imm; set_sub(s, a, (uint32_t)imm, v); break;   /* cmp */
        case 4: do_shift(s, SHIFT_SHR, u, r2); break;
        case 5: do_shift(s, SHIFT_SAR, u, r2); break;
        case 6: do_shift(s, SHIFT_SHL, u, r2); break;
        case 7: { int32_t p = (int32_t)(int16_t)a * imm; wr(s, r2, (uint32_t)p); break; }
        }
        s->pc = pc + 2;
        return 2;
    }

    switch (top) {
    case 0x6:   /* sld.b disp7[ep], reg2 */
        wr(s, r2, (uint32_t)SIGN8(mem_r(s, rd(s, 30) + BITS(w, 0, 7), 1)));
        break;
    case 0x7:   /* sst.b reg2, disp7[ep] */
        mem_w(s, rd(s, 30) + BITS(w, 0, 7), 1, rd(s, r2));
        break;
    case 0x8:   /* sld.h disp8[ep], reg2 */
        wr(s, r2, (uint32_t)SIGN16(mem_r(s, rd(s, 30) + (BITS(w, 0, 7) << 1), 2)));
        break;
    case 0x9:   /* sst.h */
        mem_w(s, rd(s, 30) + (BITS(w, 0, 7) << 1), 2, rd(s, r2));
        break;
    case 0xA: { /* sld.w / sst.w, disp = bits1-6 << 2 */
        uint32_t d = BITS(w, 1, 6) << 2;
        if (w & 1) mem_w(s, rd(s, 30) + d, 4, rd(s, r2));
        else       wr(s, r2, mem_r(s, rd(s, 30) + d, 4));
        break;
    }
    case 0xB: { /* Bcond disp9 */
        int32_t d = SIGN8(((w & 0xF800) >> 8) | ((w & 0x0070) >> 4)) * 2;
        if (cond_true(s, BITS(w, 0, 4))) {
            s->cycles += 2;                          /* taken: 3, else 1 */
            s->pc = pc + (uint32_t)d; return 2;
        }
        break;
    }
    case 0xC: case 0xD: {   /* 32-bit immediate group */
        uint16_t imm = fetch16(s, pc + 2);
        len = 4;
        int op = BITS(w, 5, 6);
        uint32_t a = rd(s, r1), v;
        switch (op) {
        case 0x30: v = a + (uint32_t)SIGN16(imm);
                   set_add(s, a, (uint32_t)SIGN16(imm), v); wr(s, r2, v); break;
        case 0x31:  /* movea, or V850E mov imm32 when reg2 == 0 */
            if (r2 == 0) {
                uint32_t lo = imm, hi = fetch16(s, pc + 4);
                wr(s, r1, lo | ((uint32_t)hi << 16));
                len = 6;
            } else {
                wr(s, r2, a + (uint32_t)SIGN16(imm));
            }
            break;
        /* dispose is reg2 == 0 in two slots: bit 5 of the first halfword is
         * the top bit of its imm5, so imm5 >= 16 lands in 0x33 (satsubi)
         * rather than 0x32 (movhi). Both would write r0 otherwise. */
        case 0x32:
            if (r2 == 0) return do_dispose(s, w, imm, pc);
            wr(s, r2, a + ((uint32_t)imm << 16));   /* movhi */
            break;
        case 0x33:
            if (r2 == 0) return do_dispose(s, w, imm, pc);
            v = sat_sub(s, a, (uint32_t)SIGN16(imm)); wr(s, r2, v); break;   /* satsubi */
        case 0x34: v = a | imm;  wr(s, r2, v); set_logic(s, v); break;
        case 0x35: v = a ^ imm;  wr(s, r2, v); set_logic(s, v); break;
        case 0x36: v = a & imm;  wr(s, r2, v); set_logic(s, v); break;
        case 0x37: { int32_t p = (int32_t)(int16_t)a * (int32_t)(int16_t)imm;
                     wr(s, r2, (uint32_t)p); break; }
        default:
            s->illegal = true; s->illegal_pc = pc; return 0;
        }
        break;
    }
    case 0xE: {  /* ld/st with disp16 */
        uint16_t d16 = fetch16(s, pc + 2);
        len = 4;
        bool store = !!(w & (1u << 6));
        bool wide  = !!(w & (1u << 5));
        uint32_t base = rd(s, r1);
        if (wide) {
            uint32_t addr = base + (uint32_t)SIGN16(d16 & 0xFFFE);
            /* The address is aligned, not faulted: words drop the low two
             * bits, halfwords one (GDB sim/v850/simops.c). */
            if (d16 & 1) {   /* word */
                addr &= ~3u;
                if (store) mem_w(s, addr, 4, rd(s, r2));
                else       wr(s, r2, mem_r(s, addr, 4));
            } else {         /* halfword */
                addr &= ~1u;
                if (store) mem_w(s, addr, 2, rd(s, r2));
                else       wr(s, r2, (uint32_t)SIGN16(mem_r(s, addr, 2)));
            }
        } else {
            uint32_t addr = base + (uint32_t)SIGN16(d16);
            if (store) mem_w(s, addr, 1, rd(s, r2));
            else       wr(s, r2, (uint32_t)SIGN8(mem_r(s, addr, 1)));
        }
        break;
    }
    case 0xF:
        if (!(w & (1u << 6))) {
            /* Bit 0 of the second halfword separates the three forms here. */
            uint16_t w2 = fetch16(s, pc + 2);
            if (!(w2 & 1)) {                        /* jr / jarl, disp22 */
                s->cycles += 2;                     /* jr, jarl: 3 */
                int32_t d = (int32_t)((BITS(w, 0, 6) << 16) | w2);
                if (d & 0x00200000) d |= 0xFFC00000;
                if (r2) wr(s, r2, pc + 4);
                s->pc = pc + (uint32_t)(d & ~1);
                return 4;
            }
            if (r2 != 0) {                          /* ld.bu disp16 */
                uint32_t d = (uint32_t)SIGN16((w2 & 0xFFFE) | ((w & 0x0020) >> 5));
                wr(s, r2, (uint8_t)mem_r(s, rd(s, r1) + d, 1));
                s->pc = pc + 4;
                return 4;
            }
            return do_prepare(s, w, w2, pc);
        }
        if (w & (1u << 5)) {
            uint16_t w2 = fetch16(s, pc + 2);
            /* ld.hu and the extended group share this opcode: bit 0 of the
             * second halfword set is ld.hu (E3 37 1B 00 ld.hu 0x1A[sp], r6),
             * clear is an extended instruction (E0 07 60 01 di). */
            if (w2 & 1) {
                uint32_t addr = rd(s, r1) + (uint32_t)SIGN16(w2 & 0xFFFE);
                wr(s, r2, (uint16_t)mem_r(s, addr & ~1u, 2));
                s->pc = pc + 4;
                return 4;
            }
                                              /* extended group */
            return step_extended(s, w, w2, pc);
        }
        {                                  /* bit operations: set1/not1/clr1/tst1 */
            uint16_t d16 = fetch16(s, pc + 2);
            len = 4;
            int bit = BITS(w, 11, 3);
            int op = BITS(w, 14, 2);
            uint32_t addr = rd(s, r1) + (uint32_t)SIGN16(d16);
            uint8_t v = (uint8_t)mem_r(s, addr, 1);
            s->psw = (v & (1u << bit)) ? (s->psw & ~V850_PSW_Z) : (s->psw | V850_PSW_Z);
            switch (op) {
            case 0: v |= (1u << bit);  mem_w(s, addr, 1, v); break;
            case 1: v ^= (1u << bit);  mem_w(s, addr, 1, v); break;
            case 2: v &= ~(1u << bit); mem_w(s, addr, 1, v); break;
            case 3: break;   /* tst1 */
            }
            s->cycles += (op == 3) ? 2 : 3;   /* tst1: 3, the others: 4 */
        }
        break;
    default:
        s->illegal = true;
        s->illegal_pc = pc;
        return 0;
    }

    s->pc = pc + len;
    return len;
}

/* opcode 000000: mov reg1, reg2. */
static int step_sub_000000(V850State *s, uint16_t w, uint32_t pc)
{
    int r1 = BITS(w, 0, 5);
    int r2 = BITS(w, 11, 5);
    if (r2 != 0) {
        wr(s, r2, rd(s, r1));
    }
    /* reg2 == 0 is "mov reg1, r0", a no-op on V850E/ES (the encoding was only
     * reclaimed as RIE on V850E2, and no such use appears in these images). */
    s->pc = pc + 2;
    return 2;
}

/* opcodes 000100-000111: satsubr, satsub, satadd, mulh — with the V850E
 * zxb/zxh/sxb/sxh forms when reg2 is zero. */
static int step_sub_0001(V850State *s, uint16_t w)
{
    uint32_t pc = s->pc;
    int r1 = BITS(w, 0, 5);
    int r2 = BITS(w, 11, 5);
    int op = BITS(w, 5, 2);
    uint32_t a = rd(s, r2), b = rd(s, r1), v;

    if (r2 == 0) {
        switch (op) {
        case 0: wr(s, r1, rd(s, r1) & 0xFF); break;                    /* zxb */
        case 1: wr(s, r1, (uint32_t)SIGN8(rd(s, r1))); break;          /* sxb */
        case 2: wr(s, r1, rd(s, r1) & 0xFFFF); break;                  /* zxh */
        case 3: wr(s, r1, (uint32_t)SIGN16(rd(s, r1))); break;         /* sxh */
        }
        s->pc = pc + 2;
        return 2;
    }

    switch (op) {
    case 0: v = sat_sub(s, b, a); wr(s, r2, v); break;                 /* satsubr */
    case 1: v = sat_sub(s, a, b); wr(s, r2, v); break;                 /* satsub */
    case 2: v = sat_add(s, a, b); wr(s, r2, v); break;                 /* satadd */
    case 3: { int32_t p = (int32_t)(int16_t)a * (int32_t)(int16_t)b;
              wr(s, r2, (uint32_t)p); break; }
    }
    s->pc = pc + 2;
    return 2;
}

/* BSW/BSH/HSW flags: CY and Z as each instruction defines them, S from bit 31,
 * OV cleared. */
static void set_swap_flags(V850State *s, uint32_t v, bool cy, bool z)
{
    s->psw &= ~(V850_PSW_CY | V850_PSW_OV | V850_PSW_S | V850_PSW_Z);
    if (cy) s->psw |= V850_PSW_CY;
    if (z) s->psw |= V850_PSW_Z;
    if (v & 0x80000000u) s->psw |= V850_PSW_S;
}

/* The 32-bit extended group, told apart by bits 10-5 of the second halfword
 * (binutils opcodes/v850-opc.c; NEC U15943EJ). reg3 is bits 15-11 of that
 * halfword. Anything else stops the core. */
static int step_extended(V850State *s, uint16_t w, uint16_t w2, uint32_t pc)
{
    int r1 = BITS(w, 0, 5);
    int r2 = BITS(w, 11, 5);
    int r3 = BITS(w2, 11, 5);
    int sub = BITS(w2, 5, 6);
    bool is_unsigned = w2 & 2;     /* mulu, divu, divhu: bit 1 of the sub-op */
    uint32_t v;

    switch (sub) {
    case 0x00:  /* setf cccc, reg2 */
        wr(s, r2, cond_true(s, BITS(w, 0, 4)) ? 1 : 0);
        break;
    case 0x01:  /* ldsr: system register in reg2, GPR in reg1, the reverse of
                 * stsr (0x07E8 = ldsr r8, EIPC); 3 clocks into EIPC/FEPC */
        if (r2 == 0 || r2 == 2) s->cycles += 2;
        sysreg_w(s, r2, rd(s, r1));
        break;
    case 0x02:  /* stsr regID, reg2 */
        wr(s, r2, sysreg_r(s, r1));
        break;
    case 0x04:  /* shr reg1, reg2 */
        do_shift(s, SHIFT_SHR, rd(s, r1) & 31, r2);
        break;
    case 0x05:  /* sar reg1, reg2 */
        do_shift(s, SHIFT_SAR, rd(s, r1) & 31, r2);
        break;
    case 0x06:  /* shl reg1, reg2 */
        do_shift(s, SHIFT_SHL, rd(s, r1) & 31, r2);
        break;
    case 0x07:  /* set1 / not1 / clr1 / tst1 reg2, [reg1]: bit number reg2 & 7 */
        { uint32_t addr = rd(s, r1);
          uint8_t bit = 1u << (rd(s, r2) & 7);
          uint8_t b = (uint8_t)mem_r(s, addr, 1);
          int op = BITS(w2, 1, 2);
          s->psw = (b & bit) ? (s->psw & ~V850_PSW_Z) : (s->psw | V850_PSW_Z);
          switch (op) {
          case 0: mem_w(s, addr, 1, b | bit); break;
          case 1: mem_w(s, addr, 1, b ^ bit); break;
          case 2: mem_w(s, addr, 1, b & ~bit); break;
          case 3: break;
          }
          s->cycles += (op == 3) ? 2 : 3; }   /* priced like the disp16 forms */
        break;
    /* trap w2=0x0100 (0x08), halt 0x0120 (0x09), reti 0x0140 (0x0A), di/ei
     * 0x0160 (0x0B), ei with reg2 = 16. */
    case 0x08:  /* trap: exception code 0x40 + vector, handler 0x40 or 0x50 */
        s->cycles += 3;                                  /* trap: 4 */
        s->eipc = pc + 4; s->eipsw = s->psw;
        s->ecr = (s->ecr & 0xFFFF0000u) | (0x40u + (uint32_t)r1);
        s->psw |= V850_PSW_ID | V850_PSW_EP;
        s->pc = 0x40 + ((r1 & 0x10) ? 0x10 : 0);
        return 4;
    case 0x09:  /* halt */
        s->halted = true;
        break;
    case 0x0A:  /* reti (w2 0x0140), ctret (0x0144); dbret (0x0146) is not modelled */
        switch (BITS(w2, 1, 2)) {
        case 0:
            s->cycles += 3;                              /* reti: 4 */
            /* NEC manual, RETI: FEPC is taken only when EP is clear AND NP
             * is set, that is, on return from a non-maskable interrupt. */
            if (!(s->psw & V850_PSW_EP) && (s->psw & V850_PSW_NP)) {
                s->pc = s->fepc; s->psw = s->fepsw;
            } else {
                s->pc = s->eipc; s->psw = s->eipsw;
            }
            return 4;
        case 2:
            s->cycles += 3;        /* V850E, not in the table: priced like reti */
            s->pc = s->ctpc;
            s->psw = s->ctpsw;
            return 4;
        default:
            s->illegal = true; s->illegal_pc = pc; return 0;
        }
    case 0x0B:  /* di (reg2 == 0) / ei (reg2 == 16) */
        if (r2 & 0x10) s->psw &= ~V850_PSW_ID;
        else           s->psw |= V850_PSW_ID;
        break;
    case 0x10:  /* sasf cccc, reg2 */
        wr(s, r2, (rd(s, r2) << 1) | (cond_true(s, BITS(w, 0, 4)) ? 1 : 0));
        break;
    case 0x11:  /* mul / mulu reg1, reg2, reg3 */
        do_multiply(s, rd(s, r1), !is_unsigned, r2, r3);
        break;
    case 0x12:  /* mul / mulu imm9, reg2, reg3: imm9 = w2 bits 5-2 : w bits 4-0 */
    case 0x13:
        { uint32_t imm9 = BITS(w, 0, 5) | (BITS(w2, 2, 4) << 5);
          if (is_unsigned) {
              do_multiply(s, imm9, false, r2, r3);
          } else {
              do_multiply(s, (uint32_t)((int32_t)(imm9 << 23) >> 23), true, r2, r3);
          } }
        break;
    case 0x14:  /* divh / divhu reg1, reg2, reg3: the divisor is reg1's low half */
        s->cycles += 35;                      /* HEURISTIC: like DIVH (36) */
        if (is_unsigned) {
            do_divide(s, rd(s, r2), rd(s, r1) & 0xFFFF, false, r2, r3);
        } else {
            do_divide(s, rd(s, r2), (uint32_t)SIGN16(rd(s, r1)), true, r2, r3);
        }
        break;
    case 0x16:  /* div / divu reg1, reg2, reg3 */
        s->cycles += 35;       /* HEURISTIC: V850E, priced like DIVH (36) */
        do_divide(s, rd(s, r2), rd(s, r1), !is_unsigned, r2, r3);
        break;
    case 0x18:  /* cmov cccc, imm5, reg2, reg3 */
    case 0x19:  /* cmov cccc, reg1, reg2, reg3 */
        { uint32_t taken = (sub == 0x19)
              ? rd(s, r1)
              : (uint32_t)((int32_t)((uint32_t)r1 << 27) >> 27);
          wr(s, r3, cond_true(s, BITS(w2, 1, 4)) ? taken : rd(s, r2)); }
        break;
    case 0x1A:  /* bsw / bsh / hsw reg2, reg3 */
        { uint32_t a = rd(s, r2);
          switch (BITS(w2, 1, 2)) {
          case 0:   /* bsw: bytes reversed; CY if any byte is 0 */
              v = __builtin_bswap32(a);
              set_swap_flags(s, v, !(v & 0xFF) || !(v & 0xFF00) || !(v & 0xFF0000)
                                   || !(v & 0xFF000000u), v == 0);
              break;
          case 1:   /* bsh: bytes swapped in each half; CY, Z on the low half */
              v = ((a & 0x00FF00FFu) << 8) | ((a >> 8) & 0x00FF00FFu);
              set_swap_flags(s, v, !(v & 0xFF) || !(v & 0xFF00), !(v & 0xFFFF));
              break;
          case 2:   /* hsw: halves swapped; CY if either half is 0 */
              v = (a << 16) | (a >> 16);
              set_swap_flags(s, v, !(v & 0xFFFF) || !(v >> 16), v == 0);
              break;
          default:
              s->illegal = true; s->illegal_pc = pc; return 0;
          }
          wr(s, r3, v); }
        break;
    default:
        s->illegal = true;
        s->illegal_pc = pc;
        return 0;
    }

    s->pc = pc + 4;
    return 4;
}

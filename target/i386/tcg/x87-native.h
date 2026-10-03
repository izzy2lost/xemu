/*
 * x87 arithmetic on the host FPU, where it is bit-exact
 *
 * Xbox games run the x87 with precision control set to double (53-bit
 * significand) and round-to-nearest. In that mode an x87 add, subtract,
 * multiply, divide or square root rounds the exact result once, to 53 bits,
 * keeping the 15-bit extended exponent. A host IEEE double operation rounds
 * the same exact result once, to the same 53 bits. The two agree bit for bit
 * whenever
 *
 *   - both operands are exactly representable as doubles (normal, at most 53
 *     significant bits; a register loaded or computed at 64-bit precision may
 *     carry more), and
 *   - the result lies well inside the double normal range, so neither the
 *     double's narrower exponent nor its gradual underflow comes into play.
 *
 * Under those conditions the only exception the operation can raise is
 * inexact: invalid needs a NaN or infinity (excluded), divide-by-zero gives
 * an infinity (excluded), overflow and underflow need an out-of-range result
 * (excluded). Inexact is recovered exactly from the rounding error: TwoSum
 * for add/subtract, an FMA residual for multiply, divide and square root.
 * Those residuals are exact because the guard keeps every intermediate far
 * from both ends of the exponent range.
 *
 * Anything else returns false and the caller falls back to softfloat. The
 * registers keep their 80-bit format throughout; only the arithmetic moves.
 * (This is unlike USE_HARD_FPU, which stored registers as doubles and lost
 * extended-precision equality, see fpu_helper_hard.c.)
 *
 * Self-contained, so tests can check it against softfloat directly.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef TARGET_I386_TCG_X87_NATIVE_H
#define TARGET_I386_TCG_X87_NATIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* No contraction: the error terms below rely on separately rounded steps. */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

enum {
    X87N_ADD,
    X87N_SUB,
    X87N_MUL,
    X87N_DIV,
};

/*
 * Operands and results must have an unbiased exponent within +-X87N_EXP_LIMIT.
 * The double normal range is -1022..1023; the margin keeps the residuals
 * (down to about 2^(e - 106)) normal and products/quotients finite or caught.
 */
#define X87N_EXP_LIMIT 900

#define X87N_FRAC_MASK 0x000FFFFFFFFFFFFFull

/* An extended value as a double, if it is exactly one (and in range). */
static inline bool x87n_to_double(uint64_t low, uint16_t high, double *out)
{
    uint32_t exp = high & 0x7fff;
    uint64_t sign = (uint64_t)(high >> 15) << 63;
    uint64_t bits;

    if (exp == 0) {
        if (low) {
            return false;               /* denormal or pseudo-denormal */
        }
        bits = sign;                    /* +-0 */
    } else {
        int e = (int)exp - 16383;
        if (!(low >> 63) ||             /* unnormal, or infinity/NaN encoding */
            (low & 0x7ff) ||            /* more than 53 significant bits */
            e < -X87N_EXP_LIMIT || e > X87N_EXP_LIMIT) {
            return false;
        }
        bits = sign | ((uint64_t)(e + 1023) << 52) | ((low >> 11) & X87N_FRAC_MASK);
    }
    memcpy(out, &bits, sizeof(bits));
    return true;
}

/* A double result as an extended value, if it is zero or in range. */
static inline bool x87n_from_double(double r, uint64_t *low, uint16_t *high)
{
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));
    uint16_t sign = (uint16_t)((bits >> 63) << 15);
    int biased = (int)((bits >> 52) & 0x7ff);

    if (biased == 0) {
        if (bits & X87N_FRAC_MASK) {
            return false;               /* subnormal */
        }
        *low = 0;
        *high = sign;                   /* +-0 */
        return true;
    }
    int e = biased - 1023;
    if (e < -X87N_EXP_LIMIT || e > X87N_EXP_LIMIT) {
        return false;                   /* also rejects infinity and NaN */
    }
    *low = (1ull << 63) | ((bits & X87N_FRAC_MASK) << 11);
    *high = sign | (uint16_t)(e + 16383);
    return true;
}

/*
 * a op b at double precision, round-to-nearest. Returns false (and writes
 * nothing) when the result might differ from the x87's; *inexact is the PE
 * flag otherwise.
 */
static inline bool x87n_binop(int op, uint64_t a_low, uint16_t a_high,
                              uint64_t b_low, uint16_t b_high,
                              uint64_t *r_low, uint16_t *r_high, bool *inexact)
{
    double a, b, r, err;

    if (!x87n_to_double(a_low, a_high, &a) ||
        !x87n_to_double(b_low, b_high, &b)) {
        return false;
    }

    switch (op) {
    case X87N_ADD:
    case X87N_SUB: {
        if (op == X87N_SUB) {
            b = -b;
        }
        r = a + b;
        /* TwoSum: err is exactly (a + b) - r */
        double bb = r - a;
        err = (a - (r - bb)) + (b - bb);
        break;
    }
    case X87N_MUL:
        r = a * b;
        err = fma(a, b, -r);
        break;
    case X87N_DIV:
        if (b == 0.0) {
            return false;               /* divide-by-zero or invalid */
        }
        r = a / b;
        err = fma(-r, b, a);            /* a - r * b, exact */
        break;
    default:
        return false;
    }

    /*
     * A zero is exact from add/subtract (exact cancellation; the operands are
     * far above the underflow threshold) or from a zero operand. From two
     * non-zero factors, or a non-zero dividend, it is a double underflow
     * where the x87's wider exponent still holds a tiny value.
     */
    if (r == 0.0 && (op == X87N_MUL ? (a != 0.0 && b != 0.0) :
                     op == X87N_DIV ? a != 0.0 : false)) {
        return false;
    }

    if (!x87n_from_double(r, r_low, r_high)) {
        return false;
    }
    *inexact = err != 0.0;
    return true;
}

/* sqrt(a) at double precision, round-to-nearest; as x87n_binop. */
static inline bool x87n_sqrt(uint64_t a_low, uint16_t a_high,
                             uint64_t *r_low, uint16_t *r_high, bool *inexact)
{
    double a, r;

    if (!x87n_to_double(a_low, a_high, &a) || a < 0.0) {
        return false;                   /* negative: invalid */
    }
    r = sqrt(a);
    if (!x87n_from_double(r, r_low, r_high)) {
        return false;
    }
    *inexact = fma(-r, r, a) != 0.0;
    return true;
}

/*
 * Compares and integer conversions need no host FPU at all: for finite
 * operands they are exact integer work on the 80-bit encoding, valid at any
 * precision (a register may hold all 64 significand bits). Only zeros and
 * normal numbers are taken; denormals (which raise DE), unnormals,
 * infinities and NaNs go to softfloat.
 */

/* A zero or a normal finite number. */
static inline bool x87n_ordinary(uint64_t low, uint16_t high, uint32_t *exp)
{
    *exp = high & 0x7fff;
    if (*exp == 0) {
        return low == 0;                /* +-0, not a denormal */
    }
    return *exp != 0x7fff && (low >> 63);
}

/*
 * a compared with b: -1 less, 0 equal, 1 greater (softfloat's FloatRelation).
 * No exception is possible for these operands, signalling or quiet.
 */
static inline bool x87n_compare(uint64_t a_low, uint16_t a_high,
                                uint64_t b_low, uint16_t b_high, int *rel)
{
    uint32_t ea, eb;

    if (!x87n_ordinary(a_low, a_high, &ea) ||
        !x87n_ordinary(b_low, b_high, &eb)) {
        return false;
    }

    bool za = ea == 0, zb = eb == 0;
    if (za && zb) {
        *rel = 0;                       /* +0 == -0 */
        return true;
    }
    bool na = !za && (a_high >> 15), nb = !zb && (b_high >> 15);
    if (na != nb) {
        *rel = na ? -1 : 1;
        return true;
    }
    /* Same sign (a zero counts as positive here): order the magnitudes. */
    int mag = (ea != eb) ? (ea < eb ? -1 : 1) :
              (a_low != b_low) ? (a_low < b_low ? -1 : 1) : 0;
    *rel = na ? -mag : mag;
    return true;
}

enum {                                  /* x87 RC, FPUC bits 11:10 */
    X87N_RC_NEAREST,
    X87N_RC_DOWN,
    X87N_RC_UP,
    X87N_RC_CHOP,
};

/*
 * a rounded to an integer by rc, if |a| < 2^62 (so any int64 result fits
 * after rounding); *inexact if a had a fractional part.
 */
static inline bool x87n_to_int64(uint64_t low, uint16_t high, int rc,
                                 int64_t *out, bool *inexact)
{
    uint32_t exp;
    uint64_t mag, frac;

    if (!x87n_ordinary(low, high, &exp)) {
        return false;
    }
    if (exp == 0) {
        *out = 0;
        *inexact = false;
        return true;
    }

    int e = (int)exp - 16383;
    if (e > 61) {
        return false;                   /* may not fit; leave the overflow */
    }
    int shift = 63 - e;                 /* >= 2 */
    if (shift < 64) {
        mag = low >> shift;
        frac = low << (64 - shift);     /* fraction, left-aligned */
    } else if (shift == 64) {
        mag = 0;
        frac = low;                     /* [0.5, 1) */
    } else {
        mag = 0;
        frac = 1;                       /* (0, 0.5): non-zero, below a half */
    }

    bool neg = high >> 15;
    bool inc;
    switch (rc) {
    case X87N_RC_NEAREST:
        inc = frac > (1ull << 63) || (frac == (1ull << 63) && (mag & 1));
        break;
    case X87N_RC_DOWN:
        inc = neg && frac;
        break;
    case X87N_RC_UP:
        inc = !neg && frac;
        break;
    default:
        inc = false;
        break;
    }
    mag += inc;

    *out = neg ? -(int64_t)mag : (int64_t)mag;
    *inexact = frac != 0;
    return true;
}

#endif

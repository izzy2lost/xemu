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

#endif

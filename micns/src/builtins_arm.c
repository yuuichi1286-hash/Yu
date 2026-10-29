/*
 * 32-bit integer division helpers for 32-bit ARM. ARMv7-A does not
 * guarantee a hardware divider, so the compiler emits calls to these
 * run-time ABI functions; the NDK would link them from compiler-rt.
 * Shift-and-subtract versions are plenty for MicNS (setup code only).
 */
#if defined(__arm__)

typedef unsigned int su_int;
typedef int si_int;
typedef unsigned long long du_int;

static su_int udiv32(su_int n, su_int d, su_int *rem) {
    su_int q = 0, r = 0;
    int i;
    if (d == 0) {
        *rem = 0;
        return 0;
    }
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    *rem = r;
    return q;
}

static si_int sdiv32(si_int n, si_int d, si_int *rem) {
    su_int un = n < 0 ? 0u - (su_int)n : (su_int)n;
    su_int ud = d < 0 ? 0u - (su_int)d : (su_int)d;
    su_int ur, uq = udiv32(un, ud, &ur);
    *rem = n < 0 ? -(si_int)ur : (si_int)ur;
    return (n < 0) != (d < 0) ? -(si_int)uq : (si_int)uq;
}

__attribute__((visibility("hidden"))) su_int __aeabi_uidiv(su_int n, su_int d) {
    su_int r;
    return udiv32(n, d, &r);
}

__attribute__((visibility("hidden"))) si_int __aeabi_idiv(si_int n, si_int d) {
    si_int r;
    return sdiv32(n, d, &r);
}

/* Quotient in r0, remainder in r1: exactly how AAPCS returns a 64-bit value. */
__attribute__((visibility("hidden"))) du_int __aeabi_uidivmod(su_int n, su_int d) {
    su_int r, q = udiv32(n, d, &r);
    return ((du_int)r << 32) | q;
}

__attribute__((visibility("hidden"))) du_int __aeabi_idivmod(si_int n, si_int d) {
    si_int r, q = sdiv32(n, d, &r);
    return ((du_int)(su_int)r << 32) | (su_int)q;
}

/*
 * The ARM run-time ABI memory helpers clang emits for struct copies and
 * zeroing. Bionic exports them too, but defining them here keeps the
 * library independent of which ones a given Android release exposes.
 * This file is built with -fno-builtin so the calls below stay plain
 * memcpy/memmove/memset calls instead of turning back into __aeabi_*.
 */
typedef __SIZE_TYPE__ size_t;
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#define AEABI_HIDDEN __attribute__((visibility("hidden")))

AEABI_HIDDEN void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
AEABI_HIDDEN void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
AEABI_HIDDEN void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
AEABI_HIDDEN void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
AEABI_HIDDEN void __aeabi_memmove4(void *d, const void *s, size_t n) { memmove(d, s, n); }
AEABI_HIDDEN void __aeabi_memmove8(void *d, const void *s, size_t n) { memmove(d, s, n); }
/* Note the run-time ABI argument order: (dest, n, c). */
AEABI_HIDDEN void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
AEABI_HIDDEN void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }
AEABI_HIDDEN void __aeabi_memset8(void *d, size_t n, int c) { memset(d, c, n); }
AEABI_HIDDEN void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
AEABI_HIDDEN void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
AEABI_HIDDEN void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }

#endif

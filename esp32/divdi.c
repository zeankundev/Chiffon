/* 64-bit unsigned division for the call0 ABI.  The toolchain's libgcc is built for the windowed ABI,
 * so the kernel supplies these two helpers itself (only shifts and subtraction, so no recursion). */
typedef unsigned long long u64;

static u64 udivmod64(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return udivmod64(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; udivmod64(n, d, &r); return r; }

#ifndef SNESRECOMP_SIMD_H
#define SNESRECOMP_SIMD_H

/*
 * SIMD is an optional implementation detail, never part of the runner ABI.
 * Build systems that intentionally opt in define SNESRECOMP_ENABLE_SIMD=1.
 * Keeping the default at zero means a plain C11 compile always selects the
 * portable implementation rather than silently depending on the build host.
 */
#ifndef SNESRECOMP_ENABLE_SIMD
#define SNESRECOMP_ENABLE_SIMD 0
#endif

#if SNESRECOMP_ENABLE_SIMD && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__))
#define SR_SIMD_NEON 1
#else
#define SR_SIMD_NEON 0
#endif

#if SNESRECOMP_ENABLE_SIMD && \
    (defined(__aarch64__) || defined(_M_ARM64)) && SR_SIMD_NEON
#define SR_SIMD_NEON64 1
#else
#define SR_SIMD_NEON64 0
#endif

#if SNESRECOMP_ENABLE_SIMD && \
    (defined(__SSE2__) || defined(_M_X64) || \
     (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define SR_SIMD_SSE2 1
#else
#define SR_SIMD_SSE2 0
#endif

#define SR_SIMD_AVAILABLE (SR_SIMD_NEON || SR_SIMD_SSE2)

#endif

// SPDX-License-Identifier: LGPL-2.0-or-later
//
// simd_scan — find the first NON-printable-ASCII byte in a buffer, fast.
//
// The parser's ground-state fast path is a run of ordinary printable ASCII
// (0x20..0x7E): plain text, and the ASCII inside escape-heavy output. It's the
// single hottest loop under any real terminal load. The scalar version walks
// one byte at a time; this walks 16 at a time with SIMD, returning the offset
// of the first byte that is < 0x20 or > 0x7E (a control char, DEL, or a UTF-8
// lead byte >= 0x80) — i.e. where the fast path must stop and hand back to the
// full state machine.
//
// Three implementations, chosen at compile time, all returning the identical
// answer as the scalar reference:
//   • ARM NEON   (Apple Silicon, aarch64)
//   • x86 SSE2   (baseline on every x86-64)
//   • scalar     (portable fallback)
//
// The predicate "printable" is `b - 0x20 <= 0x5E` (unsigned): a single subtract
// + unsigned compare that folds both bounds (0x20..0x7E) into one test — the
// same trick the SIMD paths use lane-wise.

#ifndef TOE_VT_SIMD_SCAN_HPP
#define TOE_VT_SIMD_SCAN_HPP

#include <cstddef>
#include <cstdint>

#if defined(TOE_FORCE_SCALAR)
// Benchmark/debug: force the portable scalar path regardless of arch.
#elif defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define TOE_SIMD_NEON 1
#elif defined(__AVX2__)
// AVX2 doubles the window to 32 bytes per compare and, unlike SSE2, has a
// direct unsigned min/max so the range test needs no sign-bias trick. Gated on
// __AVX2__ so the binary still runs on pre-2015 CPUs when built at the plain
// x86-64 baseline (see TOE_ARCH in the top-level CMakeLists).
#include <immintrin.h>
#define TOE_SIMD_AVX2 1
#elif defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define TOE_SIMD_SSE2 1
#endif

namespace toe::vt {

// A byte is "printable ASCII" iff 0x20 <= b <= 0x7E. Fold to one unsigned test.
[[nodiscard]] inline bool is_printable_ascii(std::uint8_t b) noexcept {
    return static_cast<std::uint8_t>(b - 0x20u) <= (0x7Eu - 0x20u);
}

// Return the number of leading printable-ASCII bytes in [p, p+n): the offset of
// the first byte that is NOT in 0x20..0x7E, or n if the whole span is printable.
[[nodiscard]] inline std::size_t printable_ascii_run(const std::uint8_t *p, std::size_t n) noexcept {
    std::size_t i = 0;

#if defined(TOE_SIMD_NEON)
    // NEON: 16 bytes/iter. printable = (v >= 0x20) & (v <= 0x7E). We build a
    // per-lane mask of BAD lanes, then compress the 16 lanes to a 64-bit value
    // (4 bits/lane) with vshrn so the first bad lane is (ctz>>2) — fully
    // branchless, no scalar re-sweep.
    const uint8x16_t lo = vdupq_n_u8(0x20);
    const uint8x16_t hi = vdupq_n_u8(0x7E);
    for (; i + 16 <= n; i += 16) {
        const uint8x16_t v = vld1q_u8(p + i);
        const uint8x16_t ge = vcgeq_u8(v, lo); // 0xFF where v >= 0x20
        const uint8x16_t le = vcleq_u8(v, hi); // 0xFF where v <= 0x7E
        const uint8x16_t bad = vmvnq_u8(vandq_u8(ge, le)); // 0xFF where NOT printable
        // Compress: narrow each 16-bit lane pair to 4 bits (vshrn #4) -> u64.
        // A bad byte leaves a non-zero nibble; the first bad lane is ctz(mask)/4.
        const uint64_t mask =
            vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(bad), 4)), 0);
        if (mask != 0) {
            return i + (static_cast<std::size_t>(__builtin_ctzll(mask)) >> 2);
        }
    }
#elif defined(TOE_SIMD_AVX2)
    // AVX2: 32 bytes/iter, using the same sign-bias trick as the SSE2 path.
    //
    // NOTE the tempting shortcut that does NOT work: `subs_epu8(b, 0x20)` then
    // a range compare. Saturating subtract maps every byte below 0x20 to the
    // SAME value (0) as 0x20 itself, so the low bound is destroyed and control
    // bytes like ESC are misread as printable. AVX2 has no unsigned byte
    // compare, so bias into signed space and use two signed compares.
    const __m256i bias = _mm256_set1_epi8(static_cast<char>(0x80));
    const __m256i lo = _mm256_set1_epi8(static_cast<char>(0x20 ^ 0x80));
    const __m256i hi = _mm256_set1_epi8(static_cast<char>(0x7E ^ 0x80));
    for (; i + 32 <= n; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + i));
        v = _mm256_xor_si256(v, bias); // unsigned compare == signed compare
        // bad = (v < lo) | (v > hi). AVX2 has only cmpgt, so `v < lo` is
        // spelled `lo > v`.
        const __m256i lt = _mm256_cmpgt_epi8(lo, v);
        const __m256i gt = _mm256_cmpgt_epi8(v, hi);
        const __m256i bad = _mm256_or_si256(lt, gt);
        const std::uint32_t mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(bad));
        if (mask != 0) {
            return i + static_cast<std::size_t>(__builtin_ctz(mask));
        }
    }
    // Fall through to the scalar tail for the final <32 bytes.
#elif defined(TOE_SIMD_SSE2)
    // SSE2 has no unsigned compare, so shift the range into signed space: a byte
    // is printable iff (int8)(b ^ 0x80) is within [0x20^0x80 .. 0x7E^0x80], i.e.
    // we test (b - 0x20) <= 0x5E via saturating tricks. Easiest correct SSE2:
    // compute bad = (b < 0x20) | (b > 0x7E) using signed compares on biased bytes.
    const __m128i bias = _mm_set1_epi8(static_cast<char>(0x80));
    const __m128i lo = _mm_set1_epi8(static_cast<char>(0x20 ^ 0x80)); // biased 0x20
    const __m128i hi = _mm_set1_epi8(static_cast<char>(0x7E ^ 0x80)); // biased 0x7E
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + i));
        v = _mm_xor_si128(v, bias); // now an unsigned compare == signed compare
        // bad = (v < lo) | (v > hi)
        const __m128i lt = _mm_cmplt_epi8(v, lo);
        const __m128i gt = _mm_cmpgt_epi8(v, hi);
        const __m128i bad = _mm_or_si128(lt, gt);
        const int mask = _mm_movemask_epi8(bad); // bit k set == byte k is bad
        if (mask != 0) {
            return i + static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
        }
    }
#endif

    // Scalar tail (and the whole loop when no SIMD is available).
    for (; i < n; ++i) {
        if (!is_printable_ascii(p[i])) return i;
    }
    return n;
}

} // namespace toe::vt

#endif // TOE_VT_SIMD_SCAN_HPP

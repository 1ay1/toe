// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe::target — the compiler and the CPU's vector unit, read ONCE for toe.
//
// toe stands alone (no jaal, no mayag), so it has its own one reader; every
// other toe file tests TOE_ISA_* or calls what is defined here, never
// __SSE2__ / __aarch64__ / _M_X64 / __builtin_*. The ISA definitions match
// jaal/meta/target.hpp's JAAL_ISA_* exactly, and hand (which sees both)
// static_asserts that they agree (src/core/term.cpp), so the stack still has
// one answer. Each is always defined, 0 or 1.
//
// (The old inline test was `__x86_64__ || __SSE2__`: MSVC defines neither,
// only _M_X64, so on Windows the parser's hot loop silently ran scalar.)

#ifndef TOE_CORE_TARGET_HPP
#define TOE_CORE_TARGET_HPP

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#define TOE_ISA_NEON 1
#else
#define TOE_ISA_NEON 0
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define TOE_ISA_SSE2 1
#else
#define TOE_ISA_SSE2 0
#endif
#if defined(__AVX2__)
#define TOE_ISA_AVX2 1
#else
#define TOE_ISA_AVX2 0
#endif

#if !defined(__GNUC__) && !defined(__clang__) && TOE_ISA_SSE2
#include <xmmintrin.h>
#endif

namespace toe {
/// A hint that the cache line at `p` is about to be written. The one place
/// toe spells it: GCC and clang have a builtin, MSVC an SSE intrinsic, and
/// anywhere else it is nothing (a hint may always be dropped).
inline void prefetch_for_write([[maybe_unused]] const void *p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 1, 0);
#elif TOE_ISA_SSE2
    _mm_prefetch(static_cast<const char *>(p), _MM_HINT_NTA);
#endif
}
} // namespace toe

#endif // TOE_CORE_TARGET_HPP

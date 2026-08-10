// Differential test for the SIMD printable-ASCII scanner. The AVX2/SSE2/NEON
// paths must agree with the scalar reference on EVERY input, including the
// awkward cases: runs shorter than a vector, bad bytes at each lane position,
// 0x1F/0x7F boundaries, and high-bit (UTF-8 lead) bytes.
//
// A scanner bug here corrupts output in a way that looks like a parser bug, so
// this is checked exhaustively rather than by sampling.
#include "toe/vt/simd_scan.hpp"

#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

// Independent reference: deliberately the naive loop, not the header's helper.
static std::size_t reference(const std::uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] < 0x20 || p[i] > 0x7E) return i;
    return n;
}

int main() {
    int fails = 0;
    auto check = [&](const std::vector<std::uint8_t> &v, const char *what) {
        const std::size_t got = toe::vt::printable_ascii_run(v.data(), v.size());
        const std::size_t want = reference(v.data(), v.size());
        if (got != want) {
            std::printf("FAIL %-22s n=%zu got=%zu want=%zu\n", what, v.size(), got, want);
            ++fails;
        }
    };

    // 1. All-printable runs of every length up to 4 vectors + tail.
    for (std::size_t n = 0; n <= 140; ++n) {
        check(std::vector<std::uint8_t>(n, 'x'), "all-printable");
    }

    // 2. A single bad byte at EVERY position, for every length. This is what
    //    catches an off-by-one in the ctz/lane-index math.
    for (std::size_t n = 1; n <= 140; ++n) {
        for (std::size_t bad = 0; bad < n; ++bad) {
            std::vector<std::uint8_t> v(n, 'a');
            v[bad] = 0x1B; // ESC
            check(v, "single-esc");
        }
    }

    // 3. Boundary bytes: 0x1F/0x20 (just below/at low bound) and 0x7E/0x7F.
    for (std::uint8_t b : {0x00, 0x1F, 0x20, 0x7E, 0x7F, 0x80, 0xFF}) {
        for (std::size_t n = 1; n <= 70; ++n) {
            for (std::size_t pos = 0; pos < n; pos += 7) {
                std::vector<std::uint8_t> v(n, 'q');
                v[pos] = b;
                check(v, "boundary-byte");
            }
        }
    }

    // 4. Random fuzz over the full byte range, biased toward printable so runs
    //    are long enough to exercise the vector loop.
    std::mt19937 rng{12345};
    for (int iter = 0; iter < 20000; ++iter) {
        const std::size_t n = rng() % 200;
        std::vector<std::uint8_t> v(n);
        for (auto &c : v) {
            c = (rng() % 10 < 8) ? static_cast<std::uint8_t>(0x20 + rng() % 0x5F)
                                 : static_cast<std::uint8_t>(rng() % 256);
        }
        check(v, "fuzz");
    }

    if (fails) {
        std::printf("%d SIMD SCAN MISMATCH(ES)\n", fails);
        return 1;
    }
    std::puts("SIMD scanner matches scalar reference on all cases");
    return 0;
}

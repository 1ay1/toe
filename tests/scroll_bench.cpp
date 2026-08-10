// Micro-benchmark isolating the scroll path. `scroll` was ~3x slower per byte
// than plain ASCII in the throughput bench; this splits that workload into its
// components (through the public feed API) so the cost can be attributed to a
// specific operation rather than guessed at.
#include "toe/term/update.hpp"
#include "toe/terminal.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using Clock = std::chrono::steady_clock;

// Feed `payload` repeatedly; report ns per BYTE so workloads compare directly.
static void bench(const char *name, const std::string &payload, int reps, int cols = 200) {
    toe::Config cfg;
    toe::term::Model m{cfg, toe::Extent{cols, 50}};
    (void)toe::term::feed_output(m, payload); // warm

    const auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i) (void)toe::term::feed_output(m, payload);
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    const double bytes = static_cast<double>(payload.size()) * reps;
    std::printf("%-26s %7.2f ns/byte  %7.1f MiB/s\n", name, secs * 1e9 / bytes,
                bytes / (1024.0 * 1024.0) / secs);
}

int main() {
    const int N = 20000;

    // Pure printable text, no line breaks: the put() path alone (wraps at col
    // 200, so it still scrolls, but 1 scroll per 200 chars).
    std::string text;
    for (int i = 0; i < N; ++i) text += "abcdefghijklmnopqrstuvwxyz0123456789";

    // Newline-dense: one LF per 24 chars => a scroll every 24 bytes once the
    // cursor reaches the bottom margin. This is the `scroll` workload's shape.
    std::string lines;
    for (int i = 0; i < N; ++i) lines += "line of scrolling output\n";

    // Bare newlines: isolates line_feed + scroll with NO glyph work at all.
    std::string lf(static_cast<std::size_t>(N), '\n');

    // Same byte count as `lines` but no scrolling (CR instead of LF), to
    // separate "writing cells" from "scrolling".
    std::string cr;
    for (int i = 0; i < N; ++i) cr += "line of scrolling output\r";

    bench("text (wrap-scroll)", text, 20);
    bench("lines (LF every 24)", lines, 20);
    bench("bare LF only", lf, 20);
    bench("same text, CR only", cr, 20);

    // Blanking the new bottom row is O(cols), so scroll cost should scale with
    // WIDTH. If it does, the remaining cost is inherent memory traffic, not a
    // missing algorithmic trick.
    std::printf("\n-- bare LF vs grid width --\n");
    bench("LF @ 80 cols", lf, 20, 80);
    bench("LF @ 200 cols", lf, 20, 200);
    bench("LF @ 400 cols", lf, 20, 400);
    return 0;
}

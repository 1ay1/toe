// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/gfx/font_discovery.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace toe::gfx {

namespace {

namespace fs = std::filesystem;

// --- a tiny bounds-checked big-endian reader over a font blob ---------------
struct BE {
    const std::uint8_t *d = nullptr;
    std::size_t n = 0;
    [[nodiscard]] bool ok(std::size_t o, std::size_t len) const noexcept { return o + len <= n; }
    [[nodiscard]] std::uint16_t u16(std::size_t o) const noexcept {
        return ok(o, 2) ? static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]) : 0;
    }
    [[nodiscard]] std::uint32_t u32(std::size_t o) const noexcept {
        return ok(o, 4) ? (static_cast<std::uint32_t>(d[o]) << 24 |
                           static_cast<std::uint32_t>(d[o + 1]) << 16 |
                           static_cast<std::uint32_t>(d[o + 2]) << 8 | d[o + 3])
                        : 0;
    }
};

// Read a file's raw bytes (empty on failure).
[[nodiscard]] std::vector<std::uint8_t> slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char *>(b.data()), sz);
    return b;
}

// Does the cmap in this sfnt map `cp` to a non-zero glyph? Handles format 4 and
// 12 (the two Unicode formats in practice), bounds-checked throughout. `sfnt`
// is the offset of the sfnt header within the blob (0 for a plain font, the
// sub-font offset for a TTC) — table-directory offsets inside are FILE-absolute
// either way, so `r` always spans the whole blob.
[[nodiscard]] bool cmap_has(const BE &r, char32_t cp, std::size_t sfnt = 0) {
    // sfnt header at `sfnt` -> table directory -> find 'cmap'.
    const std::uint16_t num = r.u16(sfnt + 4);
    std::size_t cmap = 0;
    for (std::uint16_t i = 0; i < num; ++i) {
        const std::size_t rec = sfnt + 12 + 16u * i;
        if (!r.ok(rec, 16)) break;
        if (r.u32(rec) == 0x636d6170u) { cmap = r.u32(rec + 8); break; } // 'cmap' (file-absolute)
    }
    if (!cmap) return false;

    // Pick the best Unicode subtable (prefer format 12 for full-plane coverage).
    const std::uint16_t nsub = r.u16(cmap + 2);
    std::size_t sub4 = 0, sub12 = 0;
    for (std::uint16_t i = 0; i < nsub; ++i) {
        const std::size_t e = cmap + 4 + 8u * i;
        const std::uint16_t plat = r.u16(e), enc = r.u16(e + 2);
        const std::size_t sub = cmap + r.u32(e + 4);
        const bool unicode = (plat == 0) || (plat == 3 && (enc == 1 || enc == 10));
        if (!unicode) continue;
        const std::uint16_t fmt = r.u16(sub);
        if (fmt == 12) sub12 = sub;
        else if (fmt == 4 && !sub4) sub4 = sub;
    }

    if (sub12) {
        // Format 12: sorted groups {startChar, endChar, startGlyph}.
        const std::uint32_t ngroups = r.u32(sub12 + 12);
        std::size_t lo = 0, hi = ngroups;
        while (lo < hi) { // binary search the groups
            const std::size_t mid = (lo + hi) / 2;
            const std::size_t g = sub12 + 16 + 12u * mid;
            const std::uint32_t s = r.u32(g), e = r.u32(g + 4);
            if (cp < s) hi = mid;
            else if (cp > e) lo = mid + 1;
            else return r.u32(g + 8) != 0 || true; // in range -> covered
        }
        return false;
    }
    if (sub4 && cp <= 0xFFFF) {
        // Format 4: segments via endCode/startCode/idDelta/idRangeOffset.
        const std::uint16_t segX2 = r.u16(sub4 + 6);
        const std::uint16_t segs = segX2 / 2;
        const std::size_t endBase = sub4 + 14;
        const std::size_t startBase = endBase + segX2 + 2;
        const std::size_t deltaBase = startBase + segX2;
        const std::size_t roBase = deltaBase + segX2;
        for (std::uint16_t s = 0; s < segs; ++s) {
            const std::uint16_t end = r.u16(endBase + 2u * s);
            if (cp > end) continue;
            const std::uint16_t start = r.u16(startBase + 2u * s);
            if (cp < start) return false; // segments sorted; miss
            const std::uint16_t ro = r.u16(roBase + 2u * s);
            if (ro == 0) {
                const std::uint16_t delta = r.u16(deltaBase + 2u * s);
                return static_cast<std::uint16_t>(cp + delta) != 0;
            }
            const std::size_t gi = roBase + 2u * s + ro + 2u * (cp - start);
            const std::uint16_t g = r.u16(gi);
            return g != 0;
        }
    }
    return false;
}

// Is this filename a bold/italic/oblique variant? Those are poor fallback bases.
[[nodiscard]] bool is_styled(const std::string &lower) {
    static const char *bad[] = {"bold", "italic", "oblique", "light", "thin"};
    for (const char *b : bad)
        if (lower.find(b) != std::string::npos) return true;
    return false;
}

// Does this codepoint default to EMOJI presentation? A pragmatic subset of
// Unicode's Emoji_Presentation property: the ranges that are emoji-by-default
// and that a monochrome symbol font is also likely to cover. Text-presentation
// characters (←↑→, ✓, †) are deliberately excluded so they keep using the
// text font, which is what a terminal wants.
[[nodiscard]] bool is_emoji_presentation(char32_t cp) noexcept {
    return (cp >= 0x1F300 && cp <= 0x1FAFF) || // pictographs, faces, symbols, extended-A
           (cp >= 0x1F000 && cp <= 0x1F0FF) || // mahjong / dominoes / cards
           (cp >= 0x2600 && cp <= 0x27BF) ||   // misc symbols + dingbats (❤, ⚡, ✨)
           (cp >= 0x2B00 && cp <= 0x2BFF) ||   // misc symbols and arrows (⭐)
           cp == 0x203C || cp == 0x2049 ||     // ‼ ⁉
           cp == 0xFE0F;                       // VS16 (emoji presentation selector)
}

// Cheap check for colour tables, without parsing the whole font: look for
// CBDT/CBLC (bitmap) or COLR/CPAL (layered vector) in the table directory.
[[nodiscard]] bool file_is_color(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    // sfnt header + table directory is enough; read a bounded prefix.
    std::vector<char> head(64 * 1024);
    f.read(head.data(), static_cast<std::streamsize>(head.size()));
    const std::size_t n = static_cast<std::size_t>(f.gcount());
    const std::string_view sv{head.data(), n};
    return sv.find("CBDT") != std::string_view::npos ||
           sv.find("COLR") != std::string_view::npos;
}

} // namespace

FontDiscovery::FontDiscovery() = default;

std::string FontDiscovery::cache_path() const {
    const char *xdg = std::getenv("XDG_CACHE_HOME");
    const char *home = std::getenv("HOME");
    std::string dir;
    if (xdg && *xdg) dir = std::string{xdg} + "/toe";
    else if (home && *home) dir = std::string{home} + "/.cache/toe";
#if defined(_WIN32)
    // Windows has no HOME in a bare cmd session; LOCALAPPDATA is the correct
    // per-user cache root. Without this the block->font map can't persist and
    // every launch re-scans the font tree on the first CJK/emoji glyph.
    else if (const char *lad = std::getenv("LOCALAPPDATA"); lad && *lad)
        dir = std::string{lad} + "/toe/cache";
    else if (const char *up = std::getenv("USERPROFILE"); up && *up)
        dir = std::string{up} + "/.cache/toe";
#endif
    else return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir + "/fontmap";
}

void FontDiscovery::load_cache() {
    if (cache_loaded_) return;
    cache_loaded_ = true;
    const std::string p = cache_path();
    if (p.empty()) return;
    std::ifstream f(p);
    if (!f) return;
    // Lines: "<block-hex> <path>".
    std::string line;
    while (std::getline(f, line)) {
        const auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::uint32_t block =
            static_cast<std::uint32_t>(std::strtoul(line.substr(0, sp).c_str(), nullptr, 16));
        std::string path = line.substr(sp + 1);
        if (!path.empty() && fs::exists(path)) block_font_[block] = std::move(path);
    }
}

void FontDiscovery::flush() const {
    if (!dirty_) return;
    const std::string p = cache_path();
    if (p.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    if (!f) return;
    for (const auto &[block, path] : block_font_) f << std::hex << block << ' ' << path << '\n';
    dirty_ = false;
}

void FontDiscovery::ensure_candidates() {
    if (candidates_ready_) return;
    candidates_ready_ = true;

    const char *home = std::getenv("HOME");
    std::vector<fs::path> roots;
#if defined(_WIN32)
    // Windows keeps fonts in the system store plus a per-user store (fonts
    // installed without admin rights, which is the default in modern Windows).
    if (const char *win = std::getenv("SystemRoot"); win && *win)
        roots.emplace_back(std::string{win} + "\\Fonts");
    else
        roots.emplace_back("C:\\Windows\\Fonts");
    if (const char *lad = std::getenv("LOCALAPPDATA"); lad && *lad)
        roots.emplace_back(std::string{lad} + "\\Microsoft\\Windows\\Fonts");
#else
    roots.emplace_back("/usr/share/fonts");
    roots.emplace_back("/usr/local/share/fonts");
#endif
    if (home) {
        roots.emplace_back(std::string{home} + "/.local/share/fonts");
        roots.emplace_back(std::string{home} + "/.fonts");
    }

    // Broad-coverage families, best first — a scan usually hits on one of these
    // so we rarely walk the whole tree. Noto alone covers nearly all of Unicode.
    //
    // The Windows names matter: a stock Windows install has NO Noto and NO
    // DejaVu, so a POSIX-only list degrades to "scan every file in
    // C:\Windows\Fonts alphabetically", which is both slow and likely to pick a
    // decorative face. These are the system fonts that actually carry the
    // coverage: Segoe UI Emoji/Symbol (emoji + symbols), the CJK faces, Nirmala
    // UI (Indic), and Arial/Segoe UI for general scripts.
    static const char *prefer[] = {
        // Cross-platform broad-coverage families (present if the user installed them).
        "notosansmono", "notosans", "notoserif", "notonaskh", "notosanscjk", "notoserifcjk",
        "notocoloremoji", "notoemoji", "notosanssymbols", "notosanssymbols2", "notosansmath",
        "dejavusans", "dejavusansmono",
#if defined(_WIN32)
        // Windows stock coverage, in the order that resolves most glyphs first.
        "seguiemj",  // Segoe UI Emoji  — colour emoji
        "seguisym",  // Segoe UI Symbol — arrows, box drawing, dingbats
        "segoeui",   // Segoe UI        — broad Latin/Greek/Cyrillic
        "cascadiamono", "cascadiacode", // shipped monospace, good symbol coverage
        "consola",   // Consolas
        "msgothic", "meiryo", "yugothic",   // Japanese
        "malgun",                            // Korean
        "msyh", "simsun", "simhei",          // Chinese (Yahei / Song / Hei)
        "nirmala",                           // Indic scripts
        "gadugi",                            // additional scripts
        "arial", "tahoma", "lucon",
#endif
        "freefont", "freeserif", "freesans", "unifont",
        "symbola", "twemoji", "opensansemoji",
    };
    auto rank = [](const std::string &lower) -> int {
        for (std::size_t i = 0; i < std::size(prefer); ++i)
            if (lower.find(prefer[i]) != std::string::npos) return static_cast<int>(i);
        return static_cast<int>(std::size(prefer)) + 1; // unknown fonts scanned last
    };

    struct Cand {
        int rank;
        std::string path;
    };
    std::vector<Cand> found;
    for (const auto &root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc" && ext != ".TTF" &&
                ext != ".OTF" && ext != ".TTC")
                continue;
            std::string lower = it->path().filename().string();
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            int rk = rank(lower);
            if (is_styled(lower)) rk += 1000; // deprioritise styled variants
            found.push_back({rk, it->path().string()});
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Cand &a, const Cand &b) { return a.rank < b.rank; });
    candidates_.reserve(found.size());
    for (auto &c : found) candidates_.push_back(std::move(c.path));
}

bool FontDiscovery::file_covers(const std::string &path, char32_t cp) {
    const std::vector<std::uint8_t> bytes = slurp(path);
    if (bytes.size() < 12) return false;
    // A .ttc (collection) starts with 'ttcf'; the first offset points at a real
    // sfnt. Read that; one face's cmap is enough for a coverage probe.
    BE r{bytes.data(), bytes.size()};
    if (r.u32(0) == 0x74746366u) { // 'ttcf' collection: probe the first sub-font.
        const std::uint32_t off0 = r.u32(12);
        if (off0 && off0 < bytes.size()) return cmap_has(r, cp, off0);
        return false;
    }
    return cmap_has(r, cp, 0);
}

std::optional<std::string> FontDiscovery::resolve(char32_t cp,
                                                  const std::vector<std::string> &exclude) {
    load_cache();
    const std::uint32_t block = block_of(cp);

    // Emoji-presentation codepoints get a COLOUR font wherever one exists.
    //
    // Several emoji (U+2764 heart, U+26A1 voltage, the arrows and dingbats)
    // live in blocks that a monochrome symbol font also covers — on Windows,
    // Segoe UI Symbol. Plain "first font that has the glyph" then wins the
    // whole block for the mono font, and those characters render as flat
    // outlines beside neighbours that are full colour. Checking colour first
    // for these ranges is what makes emoji look right out of the box.
    if (is_emoji_presentation(cp)) {
        ensure_candidates();
        for (const std::string &path : candidates_) {
            if (std::find(exclude.begin(), exclude.end(), path) != exclude.end()) continue;
            if (!file_is_color(path)) continue;
            if (file_covers(path, cp)) return path; // NOT block-cached: per-cp
        }
        // No colour font covers it; fall through to the normal search.
    }

    // 1. Cached block hit — instant, and the common case after warm-up.
    if (auto it = block_font_.find(block); it != block_font_.end()) {
        // Re-verify the cached font actually covers THIS codepoint (a block can
        // straddle a font's coverage edge); accept if so.
        if (std::find(exclude.begin(), exclude.end(), it->second) == exclude.end() &&
            file_covers(it->second, cp))
            return it->second;
    }

    // 2. Scan candidates in priority order for the first that covers cp.
    ensure_candidates();
    for (const std::string &path : candidates_) {
        if (std::find(exclude.begin(), exclude.end(), path) != exclude.end()) continue;
        if (file_covers(path, cp)) {
            block_font_[block] = path; // remember the whole block
            dirty_ = true;
            flush();
            return path;
        }
    }
    return std::nullopt; // nothing on the system covers it -> caller draws .notdef
}

} // namespace toe::gfx

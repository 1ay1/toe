// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The kitty graphics protocol: inline images transmitted over APC
// (ESC _ G <control> ; <base64 payload> ESC \). This owns the decoded image
// pixels and the placements that pin them onto the cell grid.
//
// Scope: the common `icat` / `timg` path — direct RGB/RGBA (f=24/32) and PNG
// (f=100) transmission, single- and multi-chunk (m=1), display at the cursor
// (a=T), and deletion (a=d). Placements are anchored in ABSOLUTE row
// coordinates so they scroll with the grid and drop out of the viewport
// naturally. Advanced features (animation, relative/Unicode placeholders,
// z-stacking beyond a simple order) are out of scope for now.

#ifndef GVTE_TERM_GRAPHICS_HPP
#define GVTE_TERM_GRAPHICS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gvte::term {

// One decoded image: tightly-packed RGBA8, top-left origin.
struct Image {
    std::uint32_t id{0};
    int width{0}, height{0};             // pixels
    std::vector<std::uint8_t> rgba;      // width*height*4
    // Set true once uploaded to a GPU texture (renderer owns the tex handle in
    // a side table keyed by id); the model just tracks pixels.
};

// One on-screen placement of an image.
struct Placement {
    std::uint32_t image_id{0};
    std::uint32_t placement_id{0};
    std::int64_t abs_row{0};             // absolute top row (scrolls with grid)
    std::int32_t col{0};                 // left column
    int rows{0}, cols{0};                // cells spanned (0 = derive from pixels)
    int z{0};                            // z-index (draw order)
    // Source crop within the image (x,y,w,h in px; w/h 0 = whole image).
    int src_x{0}, src_y{0}, src_w{0}, src_h{0};
};

// The graphics state for one screen (primary or alt). Fed APC control strings.
class Graphics {
public:
    // Handle one kitty graphics APC payload (the bytes between ESC _ and ST,
    // i.e. starting at 'G'). `cursor_abs_row` / `cursor_col` anchor a display.
    // If `out_response` is non-null it receives the protocol response the client
    // expects (e.g. ESC _ G i=<id>;OK ESC \), unless suppressed by q=1/q=2.
    // Returns true if anything visible changed (the caller bumps damage).
    bool handle_apc(std::string_view data, std::int64_t cursor_abs_row, std::int32_t cursor_col,
                    int cell_w, int cell_h, std::string *out_response = nullptr);

    // Decode a sixel image (the DCS payload after the 'q', i.e. the sixel data)
    // into an RGBA image and place it at the cursor. Returns true on success.
    // `params` are the DCS parameters before 'q' (P1;P2;P3) — mostly ignored;
    // we honour the P2=1 "transparent background" hint.
    bool handle_sixel(std::string_view params, std::string_view data, std::int64_t cursor_abs_row,
                      std::int32_t cursor_col, int cell_w, int cell_h);

    [[nodiscard]] const std::vector<Placement> &placements() const noexcept { return placements_; }
    [[nodiscard]] const Image *image(std::uint32_t id) const noexcept {
        auto it = images_.find(id);
        return it == images_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool empty() const noexcept { return placements_.empty(); }

    // A monotonically increasing token bumped whenever images/placements change,
    // so the renderer can lazily (re)upload textures.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // Drop everything (RIS / resize when we can't remap sensibly).
    void clear();

private:
    // A transmission in progress across m=1 chunks, keyed by image id.
    struct Pending {
        std::string b64;        // accumulated base64 payload
        int format{32};         // 24 RGB, 32 RGBA, 100 PNG
        int width{0}, height{0};
        std::uint32_t id{0};
        std::uint32_t placement_id{0};
        bool display{false};    // a=T (display after transmit)
        int cols{0}, rows{0};
        int z{0};
    };

    void commit(Pending &p, std::int64_t cursor_abs_row, std::int32_t cursor_col, int cell_w,
                int cell_h, bool &changed);
    void do_delete(std::string_view data, bool &changed);

    std::unordered_map<std::uint32_t, Image> images_{};
    std::vector<Placement> placements_{};
    std::unordered_map<std::uint32_t, Pending> pending_{}; // in-flight chunked xfers
    std::uint32_t next_auto_id_{0x80000000u};              // for images with no id
    std::uint64_t revision_{0};
};

} // namespace gvte::term

#endif // GVTE_TERM_GRAPHICS_HPP

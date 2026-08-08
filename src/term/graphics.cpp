// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/term/graphics.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

#include <png.h>

namespace toe::term {

namespace {

// Standard base64 decode. Skips whitespace; stops at padding. Returns bytes.
std::vector<std::uint8_t> b64_decode(std::string_view in) {
    static const auto table = [] {
        std::array<std::int8_t, 256> t{};
        t.fill(-1);
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<std::uint8_t>(a[i])] = static_cast<std::int8_t>(i);
        return t;
    }();
    std::vector<std::uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t buf = 0;
    int bits = 0;
    for (char ch : in) {
        const std::int8_t v = table[static_cast<std::uint8_t>(ch)];
        if (v < 0) continue; // skip '=' and whitespace
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

int to_int(std::string_view s, int def = 0) {
    int v = def;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}
long long to_ll(std::string_view s, long long def = 0) {
    long long v = def;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

// Decode a PNG (from memory) to tightly-packed RGBA8 via libpng. Returns false
// on any error. Kept self-contained so the rest of the protocol is decoder-free.
bool decode_png(const std::uint8_t *data, std::size_t len, int &w, int &h,
                std::vector<std::uint8_t> &rgba) {
    if (len < 8 || png_sig_cmp(data, 0, 8) != 0) return false;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) { // libpng error handler
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    struct Src {
        const std::uint8_t *p;
        std::size_t left;
    } src{data, len};
    png_set_read_fn(png, &src, [](png_structp pp, png_bytep out, png_size_t n) {
        auto *s = static_cast<Src *>(png_get_io_ptr(pp));
        const std::size_t take = std::min<std::size_t>(n, s->left);
        std::memcpy(out, s->p, take);
        s->p += take;
        s->left -= take;
    });
    png_read_info(png, info);

    w = static_cast<int>(png_get_image_width(png, info));
    h = static_cast<int>(png_get_image_height(png, info));
    const png_byte color = png_get_color_type(png, info);
    const png_byte depth = png_get_bit_depth(png, info);

    // Normalise everything to 8-bit RGBA.
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (color == PNG_COLOR_TYPE_RGB || color == PNG_COLOR_TYPE_GRAY ||
        color == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > (64LL << 20)) return false; // sanity cap
    rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    std::vector<png_bytep> rows(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        rows[static_cast<std::size_t>(y)] =
            rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    return true;
}

} // namespace

void Graphics::clear() {
    images_.clear();
    placements_.clear();
    pending_.clear();
    ++revision_;
}

bool Graphics::handle_sixel(std::string_view params, std::string_view data,
                            std::int64_t cursor_abs_row, std::int32_t cursor_col, int cell_w,
                            int cell_h) {
    // P2 (the second DCS parameter) == 1 means "pixels left un-set are
    // transparent"; otherwise they're the background colour. We render un-set
    // pixels as transparent regardless (cleaner over a terminal cell), but
    // track it for correctness of the default fill.
    (void)params;

    // The default sixel palette (VT340-ish): 16 colours. Apps usually redefine
    // them, but provide a sane default so a palette-less image still shows.
    struct Col { std::uint8_t r, g, b, a; };
    std::vector<Col> pal(256, Col{0, 0, 0, 255});
    static const std::uint8_t kDefault[16][3] = {
        {0, 0, 0},    {20, 20, 80},  {80, 13, 13}, {20, 80, 20}, {80, 20, 80}, {20, 80, 80},
        {80, 80, 20}, {53, 53, 53},  {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33},
        {33, 33, 60}, {33, 60, 60},  {60, 60, 33}, {80, 80, 80},
    };
    for (int i = 0; i < 16; ++i) {
        pal[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(kDefault[i][0] * 255 / 100),
                                            static_cast<std::uint8_t>(kDefault[i][1] * 255 / 100),
                                            static_cast<std::uint8_t>(kDefault[i][2] * 255 / 100),
                                            255};
    }

    // Decode into a growable RGBA canvas. Sixel writes 6-pixel vertical bands.
    std::vector<std::uint8_t> canvas; // RGBA, row-major, resized as we grow
    int cw = 0, chgt = 0;             // current canvas dims
    int px = 0;                       // pen x
    int band = 0;                     // current band's top y (multiple of 6)
    int cur = 0;                      // current colour index
    int max_x = 0, max_y = 0;         // used extent

    auto ensure = [&](int nx, int ny) {
        if (nx < cw && ny < chgt) return;
        const int ncw = std::max(cw, nx + 1);
        const int nch = std::max(chgt, ((ny / 6) + 1) * 6);
        if (static_cast<long long>(ncw) * nch > (16LL << 20)) return; // 16Mpx cap
        std::vector<std::uint8_t> nn(static_cast<std::size_t>(ncw) * static_cast<std::size_t>(nch) * 4, 0);
        for (int y = 0; y < chgt; ++y)
            std::memcpy(nn.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(ncw) * 4,
                        canvas.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) * 4,
                        static_cast<std::size_t>(cw) * 4);
        canvas.swap(nn);
        cw = ncw;
        chgt = nch;
    };
    auto plot = [&](int x, int y, int colour) {
        if (colour < 0 || colour >= 256) return;
        ensure(x, y);
        if (x >= cw || y >= chgt) return;
        const Col &c = pal[static_cast<std::size_t>(colour)];
        std::uint8_t *d =
            canvas.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x)) * 4;
        d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = c.a;
        if (x > max_x) max_x = x;
        if (y > max_y) max_y = y;
    };

    for (std::size_t i = 0; i < data.size(); ++i) {
        const char ch = data[i];
        if (ch == '#') { // colour: #Pc or #Pc;Pu;Px;Py;Pz
            ++i;
            int idx = 0;
            while (i < data.size() && data[i] >= '0' && data[i] <= '9') idx = idx * 10 + (data[i++] - '0');
            if (i < data.size() && data[i] == ';') { // definition
                ++i;
                int comps[4] = {0, 0, 0, 0};
                int n = 0;
                while (n < 4) {
                    int v = 0;
                    while (i < data.size() && data[i] >= '0' && data[i] <= '9') v = v * 10 + (data[i++] - '0');
                    comps[n++] = v;
                    if (i < data.size() && data[i] == ';') { ++i; continue; }
                    break;
                }
                if (idx >= 0 && idx < 256) {
                    // comps: system(1=HLS,2=RGB), then three 0..100 values.
                    auto sc = [](int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 100) * 255 / 100); };
                    pal[static_cast<std::size_t>(idx)] = {sc(comps[1]), sc(comps[2]), sc(comps[3]), 255};
                }
            }
            cur = idx;
            --i; // the for-loop will ++i
        } else if (ch == '$') { // carriage return: back to x=0, same band
            px = 0;
        } else if (ch == '-') { // next band (down 6 px)
            px = 0;
            band += 6;
        } else if (ch == '!') { // run-length: !Pn <sixel>
            ++i;
            int rep = 0;
            while (i < data.size() && data[i] >= '0' && data[i] <= '9') rep = rep * 10 + (data[i++] - '0');
            if (i < data.size()) {
                const char sx = data[i];
                if (sx >= '?' && sx <= '~') {
                    const int bits = sx - '?';
                    for (int r = 0; r < rep; ++r) {
                        for (int b = 0; b < 6; ++b)
                            if (bits & (1 << b)) plot(px, band + b, cur);
                        ++px;
                    }
                }
            }
        } else if (ch >= '?' && ch <= '~') { // one sixel column
            const int bits = ch - '?';
            for (int b = 0; b < 6; ++b)
                if (bits & (1 << b)) plot(px, band + b, cur);
            ++px;
        }
        // Other bytes (whitespace, "Pn;Pn;Pn;Pn q" raster attrs handled by DCS
        // params, etc.) are ignored.
    }

    const int w = max_x + 1, h = max_y + 1;
    if (w <= 0 || h <= 0 || canvas.empty()) return false;

    Image img;
    img.id = next_auto_id_++;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    for (int y = 0; y < h; ++y)
        std::memcpy(img.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4,
                    canvas.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) * 4,
                    static_cast<std::size_t>(w) * 4);

    const std::uint32_t iid = img.id;
    images_[iid] = std::move(img);

    Placement pl;
    pl.image_id = iid;
    pl.abs_row = cursor_abs_row;
    pl.col = cursor_col;
    pl.cols = cell_w > 0 ? (w + cell_w - 1) / cell_w : 1;
    pl.rows = cell_h > 0 ? (h + cell_h - 1) / cell_h : 1;
    placements_.push_back(pl);
    ++revision_;
    return true;
}

bool Graphics::handle_apc(std::string_view data, std::int64_t cursor_abs_row,
                          std::int32_t cursor_col, int cell_w, int cell_h,
                          std::string *out_response) {
    // kitty graphics packets start with 'G'. Anything else isn't ours.
    if (data.empty() || data.front() != 'G') return false;
    data.remove_prefix(1);

    // Split control ; payload.
    std::string_view control = data;
    std::string_view payload;
    if (const auto semi = data.find(';'); semi != std::string_view::npos) {
        control = data.substr(0, semi);
        payload = data.substr(semi + 1);
    }

    // Parse comma-separated key=value control pairs.
    char action = 'T';   // default transmit+display in practice varies; kitty uses per-cmd
    int format = 32;     // 32=RGBA, 24=RGB, 100=PNG
    int width = 0, height = 0;
    int more = 0;        // m=1 => more chunks follow
    int quiet = 0;       // q: 0 verbose, 1 suppress OK, 2 suppress all
    std::uint32_t id = 0, pid = 0;
    int cols = 0, rows = 0, z = 0;
    int sx = 0, sy = 0, sw = 0, sh = 0; // source crop (x=,y=,w=,h=)
    bool have_action = false;
    {
        std::size_t i = 0;
        while (i < control.size()) {
            std::size_t comma = control.find(',', i);
            if (comma == std::string_view::npos) comma = control.size();
            std::string_view kv = control.substr(i, comma - i);
            i = comma + 1;
            const auto eq = kv.find('=');
            if (eq == std::string_view::npos) continue;
            const std::string_view k = kv.substr(0, eq);
            const std::string_view v = kv.substr(eq + 1);
            if (k == "a") { action = v.empty() ? 'T' : v.front(); have_action = true; }
            else if (k == "f") format = to_int(v, 32);
            else if (k == "s") width = to_int(v);
            else if (k == "v") height = to_int(v);
            else if (k == "m") more = to_int(v);
            else if (k == "q") quiet = to_int(v);
            else if (k == "i") id = static_cast<std::uint32_t>(to_ll(v));
            else if (k == "p") pid = static_cast<std::uint32_t>(to_ll(v));
            else if (k == "c") cols = to_int(v);
            else if (k == "r") rows = to_int(v);
            else if (k == "z") z = to_int(v);
            else if (k == "x") sx = to_int(v);
            else if (k == "y") sy = to_int(v);
            else if (k == "w") sw = to_int(v);
            else if (k == "h") sh = to_int(v);
        }
    }
    (void)have_action;
    const std::uint32_t client_id = id; // the id the client used (for the reply)

    // A response is expected on the FINAL chunk of a command, unless q>=1 (OK
    // suppressed) / q>=2 (all suppressed), and only when the client tagged the
    // command with an id (i>0) so it can correlate the reply.
    auto respond_ok = [&]() {
        if (out_response && quiet < 1 && client_id != 0) {
            *out_response = "\x1b_Gi=" + std::to_string(client_id) + ";OK\x1b\\";
        }
    };

    bool changed = false;

    if (action == 'd') { // delete placements/images
        do_delete(control, changed);
        if (changed) ++revision_;
        return changed;
    }

    if (action == 'p') { // display a previously-transmitted image by id
        auto it = images_.find(id);
        if (it == images_.end()) return false;
        const Image &stored = it->second;
        Placement pl;
        pl.image_id = id;
        pl.placement_id = pid;
        pl.abs_row = cursor_abs_row;
        pl.col = cursor_col;
        pl.z = z;
        pl.cols = cols > 0 ? cols : (cell_w > 0 ? (stored.width + cell_w - 1) / cell_w : 1);
        pl.rows = rows > 0 ? rows : (cell_h > 0 ? (stored.height + cell_h - 1) / cell_h : 1);
        pl.src_x = sx; pl.src_y = sy; pl.src_w = sw; pl.src_h = sh;
        std::erase_if(placements_, [&](const Placement &e) {
            return e.image_id == pl.image_id && e.placement_id == pl.placement_id;
        });
        placements_.push_back(pl);
        ++revision_;
        respond_ok();
        return true;
    }

    // Transmit a new animation FRAME for an existing image (a=f). The frame
    // pixels come in the same formats; gap (r=) is its display duration in ms.
    if (action == 'f') {
        if (id == 0) return false;
        // Reuse the pending buffer for chunked frame data, keyed like transmit.
        Pending &fp = pending_[id];
        if (fp.b64.empty()) {
            fp.format = format;
            fp.width = width > 0 ? width : (images_.count(id) ? images_[id].width : 0);
            fp.height = height > 0 ? height : (images_.count(id) ? images_[id].height : 0);
            fp.id = id;
            fp.z = z; // repurposed: frame gap in ms (r= is also accepted below)
        }
        fp.b64.append(payload);
        if (fp.b64.size() > (32u << 20)) { pending_.erase(id); return false; }
        if (more == 0) {
            // 'r' (rows) is repurposed as the gap in ms for a frame; default 40.
            const int gap = rows > 0 ? rows : 40;
            handle_frame(id, gap, fp.format, fp.width, fp.height, fp.b64, changed);
            pending_.erase(id);
            respond_ok();
        }
        if (changed) ++revision_;
        return changed;
    }

    // Transmit (a=t) or transmit+display (a=T). Both accumulate payload, keyed
    // by id; a=q (query) is ignored. Auto-assign an id when none is given so
    // chunked transfers can coalesce.
    if (action != 't' && action != 'T') {
        return false;
    }
    if (id == 0) id = next_auto_id_++; // ephemeral

    Pending &p = pending_[id];
    if (p.b64.empty()) { // first chunk: capture params
        p.format = format;
        p.width = width;
        p.height = height;
        p.id = id;
        p.placement_id = pid;
        p.display = (action == 'T');
        p.cols = cols;
        p.rows = rows;
        p.z = z;
    }
    p.b64.append(payload);
    if (p.b64.size() > (32u << 20)) { // 32MB guard
        pending_.erase(id);
        return false;
    }

    if (more == 0) { // final chunk: decode + store
        commit(p, cursor_abs_row, cursor_col, cell_w, cell_h, changed);
        pending_.erase(id);
        respond_ok();
    }
    if (changed) ++revision_;
    return changed;
}

// Decode a transmission payload (base64 + format) into tightly-packed RGBA8.
// Returns false on malformed data. Shared by transmit and animation frames.
static bool decode_payload(int format, int width, int height, const std::string &b64,
                           int &out_w, int &out_h, std::vector<std::uint8_t> &rgba) {
    std::vector<std::uint8_t> raw = b64_decode(b64);
    if (format == 100) { // PNG
        return decode_png(raw.data(), raw.size(), out_w, out_h, rgba);
    }
    if (width <= 0 || height <= 0) return false;
    out_w = width;
    out_h = height;
    if (format == 24) { // RGB -> RGBA
        const std::size_t need = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 3;
        if (raw.size() < need) return false;
        rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        for (std::size_t i = 0, o = 0; i + 2 < need; i += 3, o += 4) {
            rgba[o] = raw[i]; rgba[o + 1] = raw[i + 1]; rgba[o + 2] = raw[i + 2]; rgba[o + 3] = 0xFF;
        }
        return true;
    }
    // 32 = RGBA
    const std::size_t need = static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(height) * 4;
    if (raw.size() < need) return false;
    rgba.assign(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(need));
    return true;
}

void Graphics::commit(Pending &p, std::int64_t cursor_abs_row, std::int32_t cursor_col,
                      int cell_w, int cell_h, bool &changed) {
    Image img;
    img.id = p.id;
    if (!decode_payload(p.format, p.width, p.height, p.b64, img.width, img.height, img.rgba))
        return;
    if (img.width <= 0 || img.height <= 0) return;
    // Seed the animation frame list with the base frame.
    img.frames.push_back(Image::Frame{img.rgba, 40});

    images_[img.id] = std::move(img);
    changed = true;

    if (p.display) {
        const Image &stored = images_[p.id];
        Placement pl;
        pl.image_id = p.id;
        pl.placement_id = p.placement_id;
        pl.abs_row = cursor_abs_row;
        pl.col = cursor_col;
        pl.z = p.z;
        // Derive cell span from the image size when not given.
        pl.cols = p.cols > 0 ? p.cols
                             : (cell_w > 0 ? (stored.width + cell_w - 1) / cell_w : 1);
        pl.rows = p.rows > 0 ? p.rows
                             : (cell_h > 0 ? (stored.height + cell_h - 1) / cell_h : 1);
        // Replace an existing placement with the same (image,placement) id.
        std::erase_if(placements_, [&](const Placement &e) {
            return e.image_id == pl.image_id && e.placement_id == pl.placement_id;
        });
        placements_.push_back(pl);
    }
}

void Graphics::do_delete(std::string_view control, bool &changed) {
    // Parse the delete target: d=<what>[,i=<id>]. Common cases:
    //   a (or A): all placements  |  i=<id>: by image id  |  default: visible.
    char what = 'a';
    std::uint32_t id = 0;
    std::size_t i = 0;
    while (i < control.size()) {
        std::size_t comma = control.find(',', i);
        if (comma == std::string_view::npos) comma = control.size();
        std::string_view kv = control.substr(i, comma - i);
        i = comma + 1;
        const auto eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = kv.substr(0, eq);
        const std::string_view v = kv.substr(eq + 1);
        if (k == "d" && !v.empty()) what = v.front();
        else if (k == "i") id = static_cast<std::uint32_t>(to_ll(v));
    }
    const std::size_t before = placements_.size();
    if (what == 'i' || id != 0) {
        std::erase_if(placements_, [&](const Placement &e) { return e.image_id == id; });
        // 'I' (uppercase) also frees the image data.
        if (what == 'I') images_.erase(id);
    } else { // 'a'/'A' or unrecognised: clear all placements.
        placements_.clear();
        if (what == 'A') images_.clear();
    }
    changed = placements_.size() != before;
}

void Graphics::handle_frame(std::uint32_t id, int gap_ms, int format, int width, int height,
                            const std::string &b64, bool &changed) {
    auto it = images_.find(id);
    if (it == images_.end()) return; // frame for an unknown image
    Image &img = it->second;
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    if (!decode_payload(format, width > 0 ? width : img.width, height > 0 ? height : img.height,
                        b64, w, h, rgba))
        return;
    // Frames must match the base image dimensions.
    if (w != img.width || h != img.height) return;
    if (img.frames.empty()) img.frames.push_back(Image::Frame{img.rgba, 40});
    img.frames.push_back(Image::Frame{std::move(rgba), gap_ms > 0 ? gap_ms : 40});
    animating_ = true;
    changed = true;
}

bool Graphics::advance_animations(std::uint64_t now_ms) {
    if (!animating_) return false;
    bool any = false;
    bool still_animating = false;
    for (auto &[id, img] : images_) {
        if (img.frames.size() < 2) continue;
        still_animating = true;
        if (img.next_advance_ms == 0) {
            // Start the clock the first time we see this animation.
            img.next_advance_ms =
                now_ms + static_cast<std::uint64_t>(img.frames[static_cast<std::size_t>(img.current)].gap_ms);
            continue;
        }
        if (now_ms >= img.next_advance_ms) {
            img.current = (img.current + 1) % static_cast<int>(img.frames.size());
            img.rgba = img.frames[static_cast<std::size_t>(img.current)].rgba;
            img.next_advance_ms =
                now_ms + static_cast<std::uint64_t>(img.frames[static_cast<std::size_t>(img.current)].gap_ms);
            any = true;
        }
    }
    animating_ = still_animating;
    if (any) ++revision_;
    return any;
}

std::uint64_t Graphics::next_animation_deadline() const noexcept {
    std::uint64_t soonest = 0;
    for (const auto &[id, img] : images_) {
        if (img.frames.size() < 2 || img.next_advance_ms == 0) continue;
        if (soonest == 0 || img.next_advance_ms < soonest) soonest = img.next_advance_ms;
    }
    return soonest;
}

} // namespace toe::term

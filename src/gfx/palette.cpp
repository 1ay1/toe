// SPDX-License-Identifier: LGPL-2.0-or-later

#include "toe/gfx/palette.hpp"

namespace toe::gfx {

Palette::Palette() {
    // 0-15: standard + bright ANSI colors (a common Tango-ish set).
    static constexpr std::array<Rgb, 16> base = {{
        {0x00, 0x00, 0x00}, {0xcc, 0x00, 0x00}, {0x4e, 0x9a, 0x06}, {0xc4, 0xa0, 0x00},
        {0x34, 0x65, 0xa4}, {0x75, 0x50, 0x7b}, {0x06, 0x98, 0x9a}, {0xd3, 0xd7, 0xcf},
        {0x55, 0x57, 0x53}, {0xef, 0x29, 0x29}, {0x8a, 0xe2, 0x34}, {0xfc, 0xe9, 0x4f},
        {0x72, 0x9f, 0xcf}, {0xad, 0x7f, 0xa8}, {0x34, 0xe2, 0xe2}, {0xee, 0xee, 0xec},
    }};
    for (int i = 0; i < 16; ++i) table_[static_cast<std::size_t>(i)] = base[static_cast<std::size_t>(i)];

    // 16-231: 6x6x6 color cube.
    auto level = [](int v) -> std::uint8_t {
        return static_cast<std::uint8_t>(v == 0 ? 0 : 55 + v * 40);
    };
    int idx = 16;
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 6; ++g)
            for (int b = 0; b < 6; ++b)
                table_[static_cast<std::size_t>(idx++)] = Rgb{level(r), level(g), level(b)};

    // 232-255: 24-step grayscale ramp.
    for (int i = 0; i < 24; ++i) {
        const auto v = static_cast<std::uint8_t>(8 + i * 10);
        table_[static_cast<std::size_t>(232 + i)] = Rgb{v, v, v};
    }
}

} // namespace toe::gfx

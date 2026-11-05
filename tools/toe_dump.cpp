// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Conformance dump tool. Reads a VT byte stream on stdin (or as a \xNN-escaped
// argument), feeds it to the model, and prints the resulting grid plus cursor
// position in a stable text format. Used by tools/conformance.py to diff toe
// against an independent reference implementation.
//
//   toe_dump <cols> <rows>          < stream.bin
//   toe_dump <cols> <rows> --cursor < stream.bin   (also print cursor row,col)

#include "toe/term/update.hpp"

#include <cstdio>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    int cols = argc > 1 ? std::atoi(argv[1]) : 80;
    int rows = argc > 2 ? std::atoi(argv[2]) : 24;
    bool want_cursor = false;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "--cursor") want_cursor = true;

    std::string bytes{std::istreambuf_iterator<char>(std::cin),
                      std::istreambuf_iterator<char>()};

    toe::term::Config cfg;
    toe::term::Model m{cfg, toe::Extent{cols, rows}};
    (void)toe::term::feed_output(m, bytes);

    // One line per visible row: the codepoints, with the trailing spacer of a
    // wide glyph dropped so the text lines up with a reference that stores one
    // cell per character. Combining marks follow their base glyph.
    auto push_utf8 = [](std::string &out, char32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (int r = 0; r < rows; ++r) {
        std::string line;
        for (const toe::term::Cell &c : m.screen.row(toe::Row{r})) {
            if (c.width == 0 && c.cp == U' ') continue; // wide-glyph spacer
            push_utf8(line, c.cp == 0 ? U' ' : c.cp);
            for (char32_t mk : m.screen.marks_of(c)) push_utf8(line, mk);
        }
        std::printf("%s\n", line.c_str());
    }
    if (want_cursor)
        std::printf("CURSOR %d %d\n", m.screen.cursor().row.get(), m.screen.cursor().col.get());
    return 0;
}

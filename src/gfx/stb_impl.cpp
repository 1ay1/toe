// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The single translation unit that instantiates stb_truetype. Kept apart from
// font.cpp so the ~5k-line implementation compiles once and doesn't slow every
// rebuild of the font logic.

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

// Minimal hand-drawn 5x7 bitmap font.
//
// Only the glyphs actually used by the display UI are defined (digits,
// a small set of letters, and a few punctuation marks). Each glyph is
// 7 rows of 5 bits, MSB = leftmost pixel.
#pragma once

#include <array>
#include <cstdint>

namespace aerolith {

constexpr int kFontWidth = 5;
constexpr int kFontHeight = 7;

using Glyph = std::array<uint8_t, kFontHeight>;

// Returns the bit-row array for ch, falling back to space for unknown chars.
const Glyph &glyph(char ch);

} // namespace aerolith

#include "aerolith/font5x7.hpp"

#include <cctype>
#include <unordered_map>

namespace aerolith {
namespace {

// Converts a "#...#"-style 5-char row into a 5-bit pattern (MSB = leftmost).
constexpr uint8_t Row(const char (&s)[6]) {
  uint8_t bits = 0;
  for (int i = 0; i < kFontWidth; ++i) {
    bits = static_cast<uint8_t>((bits << 1) | (s[i] == '#' ? 1 : 0));
  }
  return bits;
}

const std::unordered_map<char, Glyph> &Glyphs() {
  static const std::unordered_map<char, Glyph> glyphs = {
      {'0', {Row(".###."), Row("#...#"), Row("#..##"), Row("#.#.#"),
             Row("##..#"), Row("#...#"), Row(".###.")}},
      {'1', {Row("..#.."), Row(".##.."), Row("..#.."), Row("..#.."),
             Row("..#.."), Row("..#.."), Row(".###.")}},
      {'2', {Row(".###."), Row("#...#"), Row("....#"), Row("...#."),
             Row("..#.."), Row(".#..."), Row("#####")}},
      {'3', {Row(".###."), Row("#...#"), Row("....#"), Row("..##."),
             Row("....#"), Row("#...#"), Row(".###.")}},
      {'4', {Row("...#."), Row("..##."), Row(".#.#."), Row("#..#."),
             Row("#####"), Row("...#."), Row("...#.")}},
      {'5', {Row("#####"), Row("#...."), Row("#...."), Row("####."),
             Row("....#"), Row("#...#"), Row(".###.")}},
      {'6', {Row("..##."), Row(".#..."), Row("#...."), Row("####."),
             Row("#...#"), Row("#...#"), Row(".###.")}},
      {'7', {Row("#####"), Row("....#"), Row("...#."), Row("..#.."),
             Row(".#..."), Row(".#..."), Row(".#...")}},
      {'8', {Row(".###."), Row("#...#"), Row("#...#"), Row(".###."),
             Row("#...#"), Row("#...#"), Row(".###.")}},
      {'9', {Row(".###."), Row("#...#"), Row("#...#"), Row(".####"),
             Row("....#"), Row("...#."), Row(".##..")}},
      {'A', {Row("..#.."), Row(".#.#."), Row("#...#"), Row("#...#"),
             Row("#####"), Row("#...#"), Row("#...#")}},
      {'C', {Row(".###."), Row("#...#"), Row("#...."), Row("#...."),
             Row("#...."), Row("#...#"), Row(".###.")}},
      {'D', {Row("####."), Row("#...#"), Row("#...#"), Row("#...#"),
             Row("#...#"), Row("#...#"), Row("####.")}},
      {'E', {Row("#####"), Row("#...."), Row("#...."), Row("####."),
             Row("#...."), Row("#...."), Row("#####")}},
      {'H', {Row("#...#"), Row("#...#"), Row("#...#"), Row("#####"),
             Row("#...#"), Row("#...#"), Row("#...#")}},
      {'I', {Row(".###."), Row("..#.."), Row("..#.."), Row("..#.."),
             Row("..#.."), Row("..#.."), Row(".###.")}},
      {'L', {Row("#...."), Row("#...."), Row("#...."), Row("#...."),
             Row("#...."), Row("#...."), Row("#####")}},
      {'M', {Row("#...#"), Row("##.##"), Row("#.#.#"), Row("#...#"),
             Row("#...#"), Row("#...#"), Row("#...#")}},
      {'N', {Row("#...#"), Row("##..#"), Row("#.#.#"), Row("#.#.#"),
             Row("#..##"), Row("#...#"), Row("#...#")}},
      {'O', {Row(".###."), Row("#...#"), Row("#...#"), Row("#...#"),
             Row("#...#"), Row("#...#"), Row(".###.")}},
      {'P', {Row("####."), Row("#...#"), Row("#...#"), Row("####."),
             Row("#...."), Row("#...."), Row("#....")}},
      {'R', {Row("####."), Row("#...#"), Row("#...#"), Row("####."),
             Row("#.#.."), Row("#..#."), Row("#...#")}},
      {'S', {Row(".###."), Row("#...."), Row("#...."), Row(".###."),
             Row("....#"), Row("....#"), Row(".###.")}},
      {'T', {Row("#####"), Row("..#.."), Row("..#.."), Row("..#.."),
             Row("..#.."), Row("..#.."), Row("..#..")}},
      {'U', {Row("#...#"), Row("#...#"), Row("#...#"), Row("#...#"),
             Row("#...#"), Row("#...#"), Row(".###.")}},
      {' ', {Row("....."), Row("....."), Row("....."), Row("....."),
             Row("....."), Row("....."), Row(".....")}},
      {'.', {Row("....."), Row("....."), Row("....."), Row("....."),
             Row("....."), Row("..##."), Row("..##.")}},
      {'-', {Row("....."), Row("....."), Row("....."), Row("#####"),
             Row("....."), Row("....."), Row(".....")}},
      {':', {Row("....."), Row("..##."), Row("..##."), Row("....."),
             Row("..##."), Row("..##."), Row(".....")}},
      {'%', {Row("#...#"), Row("#..#."), Row("...#."), Row("..#.."),
             Row(".#..."), Row(".#..#"), Row("#...#")}},
  };
  return glyphs;
}

} // namespace

const Glyph &glyph(char ch) {
  const auto &glyphs = Glyphs();
  auto it = glyphs.find(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  if (it == glyphs.end()) {
    it = glyphs.find(' ');
  }
  return it->second;
}

} // namespace aerolith

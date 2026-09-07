// Direct /dev/fb0 framebuffer access (no X11/DRM client libraries needed).
//
// Queries FBIOGET_VSCREENINFO to learn the real geometry and RGB bitfield
// layout instead of assuming a fixed pixel format, since panel drivers can
// expose either RGB565 (16bpp) or XRGB8888 (32bpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aerolith {

struct Color {
  uint8_t r, g, b;
};

class FrameBuffer {
public:
  explicit FrameBuffer(const std::string &path = "/dev/fb0");
  ~FrameBuffer();

  FrameBuffer(const FrameBuffer &) = delete;
  FrameBuffer &operator=(const FrameBuffer &) = delete;

  void Clear(Color color = {0, 0, 0});
  void FillRect(int x, int y, int w, int h, Color color);

  // Draws text at (x, y) using the built-in 5x7 font, returns width in pixels.
  int DrawText(int x, int y, const std::string &text, Color color, int scale = 1,
               std::optional<Color> bg = std::nullopt);
  int TextWidth(const std::string &text, int scale = 1) const;

  // Draws text horizontally centered on center_x (defaults to the screen center).
  int DrawTextCentered(int y, const std::string &text, Color color, int scale = 1,
                        std::optional<Color> bg = std::nullopt,
                        std::optional<int> center_x = std::nullopt);

  void Flush();

  int xres() const { return xres_; }
  int yres() const { return yres_; }

private:
  int fd_;
  int xres_ = 0, yres_ = 0, xres_virtual_ = 0, yres_virtual_ = 0;
  int bits_per_pixel_ = 0;
  int bytes_per_pixel_ = 0;
  int stride_ = 0;
  uint32_t r_off_ = 0, r_len_ = 0, g_off_ = 0, g_len_ = 0, b_off_ = 0, b_len_ = 0;
  std::vector<uint8_t> canvas_;

  void Pack(Color color, uint8_t *out) const;
};

} // namespace aerolith

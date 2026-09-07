#include "aerolith/framebuffer.hpp"

#include "aerolith/font5x7.hpp"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace aerolith {

FrameBuffer::FrameBuffer(const std::string &path) {
  fd_ = open(path.c_str(), O_RDWR);
  if (fd_ < 0) {
    throw std::runtime_error("failed to open " + path + ": " + std::strerror(errno));
  }

  fb_var_screeninfo vinfo{};
  if (ioctl(fd_, FBIOGET_VSCREENINFO, &vinfo) < 0) {
    close(fd_);
    throw std::runtime_error(std::string("FBIOGET_VSCREENINFO failed: ") + std::strerror(errno));
  }

  xres_ = static_cast<int>(vinfo.xres);
  yres_ = static_cast<int>(vinfo.yres);
  xres_virtual_ = static_cast<int>(vinfo.xres_virtual);
  yres_virtual_ = static_cast<int>(vinfo.yres_virtual);
  bits_per_pixel_ = static_cast<int>(vinfo.bits_per_pixel);
  r_off_ = vinfo.red.offset;
  r_len_ = vinfo.red.length;
  g_off_ = vinfo.green.offset;
  g_len_ = vinfo.green.length;
  b_off_ = vinfo.blue.offset;
  b_len_ = vinfo.blue.length;

  if (bits_per_pixel_ != 16 && bits_per_pixel_ != 32) {
    close(fd_);
    throw std::runtime_error("unsupported bits_per_pixel=" + std::to_string(bits_per_pixel_));
  }

  bytes_per_pixel_ = bits_per_pixel_ / 8;
  stride_ = xres_virtual_ * bytes_per_pixel_;
  canvas_.assign(static_cast<size_t>(stride_) * yres_, 0);
}

FrameBuffer::~FrameBuffer() { close(fd_); }

void FrameBuffer::Pack(Color color, uint8_t *out) const {
  const uint32_t value =
      ((static_cast<uint32_t>(color.r) >> (8 - r_len_)) << r_off_) |
      ((static_cast<uint32_t>(color.g) >> (8 - g_len_)) << g_off_) |
      ((static_cast<uint32_t>(color.b) >> (8 - b_len_)) << b_off_);
  for (int i = 0; i < bytes_per_pixel_; ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
}

void FrameBuffer::Clear(Color color) {
  std::vector<uint8_t> pixel(bytes_per_pixel_);
  Pack(color, pixel.data());
  std::vector<uint8_t> row(static_cast<size_t>(xres_) * bytes_per_pixel_);
  for (int x = 0; x < xres_; ++x) {
    std::memcpy(&row[static_cast<size_t>(x) * bytes_per_pixel_], pixel.data(), bytes_per_pixel_);
  }
  for (int y = 0; y < yres_; ++y) {
    std::memcpy(&canvas_[static_cast<size_t>(y) * stride_], row.data(), row.size());
  }
}

void FrameBuffer::FillRect(int x, int y, int w, int h, Color color) {
  const int x0 = std::max(x, 0);
  const int y0 = std::max(y, 0);
  const int x1 = std::min(x + w, xres_);
  const int y1 = std::min(y + h, yres_);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  std::vector<uint8_t> pixel(bytes_per_pixel_);
  Pack(color, pixel.data());
  std::vector<uint8_t> row(static_cast<size_t>(x1 - x0) * bytes_per_pixel_);
  for (int x_i = x0; x_i < x1; ++x_i) {
    std::memcpy(&row[static_cast<size_t>(x_i - x0) * bytes_per_pixel_], pixel.data(),
                bytes_per_pixel_);
  }
  const int start = x0 * bytes_per_pixel_;
  for (int py = y0; py < y1; ++py) {
    std::memcpy(&canvas_[static_cast<size_t>(py) * stride_ + start], row.data(), row.size());
  }
}

int FrameBuffer::DrawText(int x, int y, const std::string &text, Color color, int scale,
                           std::optional<Color> bg) {
  int cursor_x = x;
  for (char ch : text) {
    const Glyph &rows = glyph(ch);
    if (bg.has_value()) {
      FillRect(cursor_x, y, (kFontWidth + 1) * scale, kFontHeight * scale, *bg);
    }
    for (int row_idx = 0; row_idx < kFontHeight; ++row_idx) {
      const uint8_t bits = rows[row_idx];
      for (int col_idx = 0; col_idx < kFontWidth; ++col_idx) {
        if (bits & (1 << (kFontWidth - 1 - col_idx))) {
          FillRect(cursor_x + col_idx * scale, y + row_idx * scale, scale, scale, color);
        }
      }
    }
    cursor_x += (kFontWidth + 1) * scale;
  }
  return cursor_x - x;
}

int FrameBuffer::TextWidth(const std::string &text, int scale) const {
  return static_cast<int>(text.size()) * (kFontWidth + 1) * scale;
}

int FrameBuffer::DrawTextCentered(int y, const std::string &text, Color color, int scale,
                                   std::optional<Color> bg, std::optional<int> center_x) {
  const int cx = center_x.value_or(xres_ / 2);
  const int x = cx - TextWidth(text, scale) / 2;
  return DrawText(x, y, text, color, scale, bg);
}

void FrameBuffer::Flush() {
  lseek(fd_, 0, SEEK_SET);
  write(fd_, canvas_.data(), canvas_.size());
}

} // namespace aerolith

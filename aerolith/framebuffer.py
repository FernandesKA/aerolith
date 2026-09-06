"""Direct /dev/fb0 framebuffer access (no X11/DRM client libraries needed).

Queries FBIOGET_VSCREENINFO to learn the real geometry and RGB bitfield
layout instead of assuming a fixed pixel format, since panel drivers can
expose either RGB565 (16bpp) or XRGB8888 (32bpp).
"""

import fcntl
import struct

from .font5x7 import FONT_HEIGHT, FONT_WIDTH, glyph

FBIOGET_VSCREENINFO = 0x4600
_VSCREENINFO_BUF_SIZE = 160  # comfortably larger than struct fb_var_screeninfo


class FrameBuffer:
    def __init__(self, path="/dev/fb0"):
        self._fd = open(path, "r+b", buffering=0)

        buf = bytearray(_VSCREENINFO_BUF_SIZE)
        fcntl.ioctl(self._fd, FBIOGET_VSCREENINFO, buf)
        (self.xres, self.yres, self.xres_virtual, self.yres_virtual,
         _xoffset, _yoffset, self.bits_per_pixel, _grayscale) = struct.unpack_from("<8I", buf, 0)
        self._r_off, self._r_len, _ = struct.unpack_from("<3I", buf, 32)
        self._g_off, self._g_len, _ = struct.unpack_from("<3I", buf, 44)
        self._b_off, self._b_len, _ = struct.unpack_from("<3I", buf, 56)

        if self.bits_per_pixel not in (16, 32):
            raise RuntimeError(f"unsupported bits_per_pixel={self.bits_per_pixel}")

        self.bytes_per_pixel = self.bits_per_pixel // 8
        self.stride = self.xres_virtual * self.bytes_per_pixel
        self.canvas = bytearray(self.stride * self.yres)

    def close(self):
        self._fd.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _pack(self, color):
        r, g, b = color
        value = (
            ((r >> (8 - self._r_len)) << self._r_off)
            | ((g >> (8 - self._g_len)) << self._g_off)
            | ((b >> (8 - self._b_len)) << self._b_off)
        )
        return value.to_bytes(self.bytes_per_pixel, "little")

    def clear(self, color=(0, 0, 0)):
        row = self._pack(color) * self.xres
        for y in range(self.yres):
            off = y * self.stride
            self.canvas[off:off + len(row)] = row

    def fill_rect(self, x, y, w, h, color):
        x0, y0 = max(x, 0), max(y, 0)
        x1, y1 = min(x + w, self.xres), min(y + h, self.yres)
        if x1 <= x0 or y1 <= y0:
            return
        row = self._pack(color) * (x1 - x0)
        start = x0 * self.bytes_per_pixel
        end = x1 * self.bytes_per_pixel
        for py in range(y0, y1):
            off = py * self.stride
            self.canvas[off + start:off + end] = row

    def draw_text(self, x, y, text, color, scale=1, bg=None):
        """Draw text at (x, y) using the built-in 5x7 font, returns width in pixels."""
        cursor_x = x
        for ch in text:
            rows = glyph(ch)
            if bg is not None:
                self.fill_rect(cursor_x, y, (FONT_WIDTH + 1) * scale, FONT_HEIGHT * scale, bg)
            for row_idx, bits in enumerate(rows):
                for col_idx in range(FONT_WIDTH):
                    if bits & (1 << (FONT_WIDTH - 1 - col_idx)):
                        self.fill_rect(
                            cursor_x + col_idx * scale,
                            y + row_idx * scale,
                            scale,
                            scale,
                            color,
                        )
            cursor_x += (FONT_WIDTH + 1) * scale
        return cursor_x - x

    def text_width(self, text, scale=1):
        return len(text) * (FONT_WIDTH + 1) * scale

    def draw_text_centered(self, y, text, color, scale=1, bg=None, center_x=None):
        """Draw text horizontally centered on center_x (defaults to the screen center)."""
        if center_x is None:
            center_x = self.xres // 2
        x = center_x - self.text_width(text, scale) // 2
        return self.draw_text(x, y, text, color, scale=scale, bg=bg)

    def flush(self):
        self._fd.seek(0)
        self._fd.write(self.canvas)

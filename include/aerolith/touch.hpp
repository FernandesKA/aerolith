// Watches a Linux evdev touchscreen node (the board's CST816x single-touch
// panel, normally /dev/input/event0) and detects a short tap -- pressed and
// released quickly, without dragging -- near the center of the touch
// surface, used to cycle the display's rotation.
#pragma once

#include <cstdint>
#include <string>

namespace aerolith {

class TouchInput {
public:
  explicit TouchInput(const std::string &path);
  ~TouchInput();

  TouchInput(const TouchInput &) = delete;
  TouchInput &operator=(const TouchInput &) = delete;

  // False if the device couldn't be opened (e.g. no touch panel on this
  // board); callers should treat that as "rotation feature unavailable"
  // rather than a fatal error.
  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }

  // Drains all currently pending events. Returns true the moment a short
  // tap near the center of the touch surface completes (finger lifted).
  bool PollShortCenterTap();

private:
  int fd_ = -1;
  bool have_abs_range_ = false;
  int32_t x_min_ = 0, x_max_ = 0, y_min_ = 0, y_max_ = 0;

  bool touching_ = false;
  bool in_center_ = false;
  int32_t cur_x_ = 0, cur_y_ = 0;
  int64_t down_time_us_ = 0;

  bool InCenterZone(int32_t x, int32_t y) const;
};

} // namespace aerolith

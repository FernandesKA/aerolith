// Watches a Linux evdev touchscreen node (the board's CST816x single-touch
// panel, normally /dev/input/event0) and recognizes two gestures near the
// center of the touch surface: a short tap (pressed and released quickly,
// without dragging) and a long press (held in place past a threshold).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace aerolith {

enum class TouchEvent { kNone, kShortTap, kLongPress };

class TouchInput {
public:
  explicit TouchInput(const std::string &path);
  ~TouchInput();

  TouchInput(const TouchInput &) = delete;
  TouchInput &operator=(const TouchInput &) = delete;

  // False if the device couldn't be opened (e.g. no touch panel on this
  // board); callers should treat that as "gestures unavailable" rather
  // than a fatal error.
  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }

  // Call roughly every 100ms regardless of whether the fd has pending
  // data -- internally non-blocking, and a long press must still be
  // recognized even if the panel stops reporting while held perfectly
  // still. Returns the gesture recognized on this call, if any.
  TouchEvent Poll();

private:
  int fd_ = -1;
  bool have_abs_range_ = false;
  int32_t x_min_ = 0, x_max_ = 0, y_min_ = 0, y_max_ = 0;

  bool touching_ = false;
  bool in_center_ = false;
  bool long_press_fired_ = false;
  int32_t cur_x_ = 0, cur_y_ = 0;
  std::chrono::steady_clock::time_point down_time_;

  bool InCenterZone(int32_t x, int32_t y) const;
};

} // namespace aerolith

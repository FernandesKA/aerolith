// Watches a Linux evdev touchscreen node (the board's CST816x single-touch
// panel, normally /dev/input/event0) and recognizes gestures: near the
// center of the touch surface, a short tap (pressed and released quickly,
// without dragging), three of those in quick succession recognized as a
// separate "triple tap" gesture instead, and a long press (held in place
// past a threshold); in the bottom-left corner of the panel (physical
// space, not rotation-aware -- like the brightness slider), a longer hold
// recognized as a separate "corner long press" gesture; and, from any
// other starting point, a quick horizontal drag recognized as a left/right
// swipe.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace aerolith {

enum class TouchEvent {
  kNone,
  kShortTap,
  kTripleTap,
  kLongPress,
  kCornerLongPress,
  kSwipeLeft,
  kSwipeRight,
};

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

  // Raw touch state as of the most recent Poll() call, for gestures (like
  // an edge-of-screen slider) that need continuous position rather than a
  // discrete recognized event. Position is in the panel's own physical
  // coordinate space, not adjusted for the display's software rotation.
  bool Touching() const { return touching_; }
  // Normalizes the current position to [0, 1] over the panel's reported
  // ABS_X/ABS_Y range. Returns false (leaving *fx/*fy untouched) if not
  // currently touching or the device never reported a usable range.
  bool NormalizedPosition(double *fx, double *fy) const;

private:
  int fd_ = -1;
  bool have_abs_range_ = false;
  int32_t x_min_ = 0, x_max_ = 0, y_min_ = 0, y_max_ = 0;

  bool touching_ = false;
  bool in_center_ = false;
  bool long_press_fired_ = false;
  bool in_corner_ = false;
  bool corner_long_press_fired_ = false;
  // Swipes are measured from where the touch started, and disallowed
  // entirely if it started somewhere another gesture already owns (the
  // brightness slider's right-edge strip, or the corner reset zone) so a
  // drag through those can't also register as a screen swipe.
  int32_t down_x_ = 0, down_y_ = 0;
  bool swipe_disallowed_ = false;
  int32_t cur_x_ = 0, cur_y_ = 0;
  std::chrono::steady_clock::time_point down_time_;

  // Triple-tap counting: a completed tap doesn't fire kShortTap right away
  // -- it waits briefly to see whether more taps follow within the gap
  // window. Two taps in that window keeps waiting; a third fires
  // kTripleTap immediately; letting the window lapse with only one or two
  // taps pending resolves to kShortTap (two are just treated as one, there
  // being no separate meaning for a double tap).
  int pending_taps_ = 0;
  std::chrono::steady_clock::time_point last_tap_time_;

  bool InCenterZone(int32_t x, int32_t y) const;
  // Bottom-left corner of the panel's physical coordinate space, used for
  // the (destructive, so deliberately out-of-the-way) stats reset gesture.
  bool InCornerZone(int32_t x, int32_t y) const;
  // Rightmost slice of the panel, matching main.cpp's brightness slider
  // zone -- kept in sync by convention, not by shared code, since
  // TouchInput otherwise has no notion of what a caller does with drags.
  bool InRightEdgeZone(int32_t x, int32_t y) const;
};

} // namespace aerolith

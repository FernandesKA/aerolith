#include "aerolith/touch.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace aerolith {

namespace {
constexpr auto kMaxTapDuration = std::chrono::milliseconds(400); // press+release, not a long-press
constexpr auto kLongPressThreshold = std::chrono::milliseconds(900);
constexpr auto kCornerLongPressThreshold = std::chrono::milliseconds(1500);
constexpr double kCenterZoneFraction = 0.6; // require the gesture within the middle 60%
// Corner zone is confined to the outer 15% along each axis, comfortably
// inside the center zone's 20%-per-side margin so the two never overlap
// and a single hold can't ever satisfy both.
constexpr double kCornerZoneFraction = 0.15;
// Matches main.cpp's kBrightnessEdgeZone -- duplicated rather than shared
// since TouchInput otherwise has no notion of what a caller does with
// drags, but a swipe starting here should defer to the brightness slider.
constexpr double kRightEdgeZoneFraction = 0.85;

// A swipe must be a quick, mostly-horizontal drag: fast enough not to be
// a deliberate long press, far enough to be a real swipe rather than a
// twitchy tap, and straight enough not to be mistaken for some other
// gesture.
constexpr auto kSwipeMaxDuration = std::chrono::milliseconds(600);
constexpr double kSwipeMinDistanceFraction = 0.25;
constexpr double kSwipeMaxCrossAxisFraction = 0.3;

// Longest gap allowed between two taps for them to count as the same
// multi-tap sequence. Long enough for an unhurried triple tap, short
// enough that two unrelated taps minutes apart don't get combined.
constexpr auto kMultiTapGap = std::chrono::milliseconds(350);
} // namespace

TouchInput::TouchInput(const std::string &path) {
  fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd_ < 0) {
    return;
  }

  input_absinfo abs_x{};
  input_absinfo abs_y{};
  if (ioctl(fd_, EVIOCGABS(ABS_X), &abs_x) == 0 &&
      ioctl(fd_, EVIOCGABS(ABS_Y), &abs_y) == 0) {
    x_min_ = abs_x.minimum;
    x_max_ = abs_x.maximum;
    y_min_ = abs_y.minimum;
    y_max_ = abs_y.maximum;
    have_abs_range_ = x_max_ > x_min_ && y_max_ > y_min_;
  }
}

TouchInput::~TouchInput() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool TouchInput::InCenterZone(int32_t x, int32_t y) const {
  if (!have_abs_range_) {
    return true;
  }
  const int32_t x_margin =
      static_cast<int32_t>((x_max_ - x_min_) * (1.0 - kCenterZoneFraction) / 2.0);
  const int32_t y_margin =
      static_cast<int32_t>((y_max_ - y_min_) * (1.0 - kCenterZoneFraction) / 2.0);
  return x >= x_min_ + x_margin && x <= x_max_ - x_margin && y >= y_min_ + y_margin &&
         y <= y_max_ - y_margin;
}

bool TouchInput::InCornerZone(int32_t x, int32_t y) const {
  if (!have_abs_range_) {
    // Without a known range we can't tell corner from center, and
    // InCenterZone already claims everything in that case -- leave this
    // gesture unavailable rather than let the two ever both match.
    return false;
  }
  const int32_t x_span = static_cast<int32_t>((x_max_ - x_min_) * kCornerZoneFraction);
  const int32_t y_span = static_cast<int32_t>((y_max_ - y_min_) * kCornerZoneFraction);
  return x <= x_min_ + x_span && y >= y_max_ - y_span;
}

bool TouchInput::InRightEdgeZone(int32_t x, int32_t /*y*/) const {
  if (!have_abs_range_) {
    return false;
  }
  const int32_t x_threshold = x_min_ + static_cast<int32_t>((x_max_ - x_min_) * kRightEdgeZoneFraction);
  return x >= x_threshold;
}

bool TouchInput::NormalizedPosition(double *fx, double *fy) const {
  if (!touching_ || !have_abs_range_) {
    return false;
  }
  *fx = static_cast<double>(cur_x_ - x_min_) / (x_max_ - x_min_);
  *fy = static_cast<double>(cur_y_ - y_min_) / (y_max_ - y_min_);
  return true;
}

TouchEvent TouchInput::Poll() {
  if (fd_ < 0) {
    return TouchEvent::kNone;
  }

  TouchEvent result = TouchEvent::kNone;
  input_event ev{};
  while (read(fd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
    if (ev.type == EV_ABS) {
      if (ev.code == ABS_X) {
        cur_x_ = ev.value;
      } else if (ev.code == ABS_Y) {
        cur_y_ = ev.value;
      }
      if (touching_) {
        in_center_ = in_center_ && InCenterZone(cur_x_, cur_y_);
        in_corner_ = in_corner_ && InCornerZone(cur_x_, cur_y_);
      }
    } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
      if (ev.value == 1) {
        touching_ = true;
        long_press_fired_ = false;
        corner_long_press_fired_ = false;
        down_time_ = std::chrono::steady_clock::now();
        down_x_ = cur_x_;
        down_y_ = cur_y_;
        in_center_ = InCenterZone(cur_x_, cur_y_);
        in_corner_ = InCornerZone(cur_x_, cur_y_);
        swipe_disallowed_ = in_corner_ || InRightEdgeZone(cur_x_, cur_y_);
      } else if (ev.value == 0 && touching_) {
        touching_ = false;
        const auto duration = std::chrono::steady_clock::now() - down_time_;
        bool swiped = false;
        if (have_abs_range_ && !swipe_disallowed_ && !long_press_fired_ &&
            duration <= kSwipeMaxDuration) {
          const double dx_frac = static_cast<double>(cur_x_ - down_x_) / (x_max_ - x_min_);
          const double dy_frac = static_cast<double>(cur_y_ - down_y_) / (y_max_ - y_min_);
          const double abs_dx = dx_frac < 0.0 ? -dx_frac : dx_frac;
          const double abs_dy = dy_frac < 0.0 ? -dy_frac : dy_frac;
          if (abs_dx >= kSwipeMinDistanceFraction && abs_dy <= kSwipeMaxCrossAxisFraction) {
            result = dx_frac > 0.0 ? TouchEvent::kSwipeRight : TouchEvent::kSwipeLeft;
            swiped = true;
            pending_taps_ = 0; // a swipe isn't part of any tap sequence
          }
        }
        if (!swiped && !long_press_fired_ && in_center_ && duration <= kMaxTapDuration) {
          // Don't fire kShortTap yet -- fold this into the pending tap
          // count and let it resolve below, either immediately (a third
          // tap) or once the gap window lapses with no further tap.
          const auto tap_time = std::chrono::steady_clock::now();
          if (pending_taps_ > 0 && tap_time - last_tap_time_ <= kMultiTapGap) {
            pending_taps_ += 1;
          } else {
            pending_taps_ = 1;
          }
          last_tap_time_ = tap_time;
          if (pending_taps_ >= 3) {
            result = TouchEvent::kTripleTap;
            pending_taps_ = 0;
          }
        } else if (!swiped) {
          // Whatever this touch was, it wasn't a tap -- don't let it later
          // combine with a tap that preceded or follows it.
          pending_taps_ = 0;
        }
      }
    }
  }

  // Checked independently of new events: a perfectly still hold may stop
  // generating fresh reports, but the press should still be recognized.
  const auto now = std::chrono::steady_clock::now();
  if (touching_ && in_center_ && !long_press_fired_ && now - down_time_ >= kLongPressThreshold) {
    long_press_fired_ = true;
    pending_taps_ = 0;
    result = TouchEvent::kLongPress;
  }
  if (touching_ && in_corner_ && !corner_long_press_fired_ &&
      now - down_time_ >= kCornerLongPressThreshold) {
    corner_long_press_fired_ = true;
    pending_taps_ = 0;
    result = TouchEvent::kCornerLongPress;
  }
  // A pending single (or double, which has no separate meaning) tap that
  // nothing else has joined within the gap window resolves now.
  if (result == TouchEvent::kNone && pending_taps_ == 1 && now - last_tap_time_ > kMultiTapGap) {
    pending_taps_ = 0;
    result = TouchEvent::kShortTap;
  } else if (pending_taps_ > 0 && now - last_tap_time_ > kMultiTapGap) {
    pending_taps_ = 0;
  }

  return result;
}

} // namespace aerolith

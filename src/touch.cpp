#include "aerolith/touch.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace aerolith {

namespace {
constexpr auto kMaxTapDuration = std::chrono::milliseconds(400); // press+release, not a long-press
constexpr auto kLongPressThreshold = std::chrono::milliseconds(900);
constexpr double kCenterZoneFraction = 0.6; // require the gesture within the middle 60%
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
      }
    } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
      if (ev.value == 1) {
        touching_ = true;
        long_press_fired_ = false;
        down_time_ = std::chrono::steady_clock::now();
        in_center_ = InCenterZone(cur_x_, cur_y_);
      } else if (ev.value == 0 && touching_) {
        touching_ = false;
        const auto duration = std::chrono::steady_clock::now() - down_time_;
        if (!long_press_fired_ && in_center_ && duration <= kMaxTapDuration) {
          result = TouchEvent::kShortTap;
        }
      }
    }
  }

  // Checked independently of new events: a perfectly still hold may stop
  // generating fresh reports, but the press should still be recognized.
  if (touching_ && in_center_ && !long_press_fired_ &&
      std::chrono::steady_clock::now() - down_time_ >= kLongPressThreshold) {
    long_press_fired_ = true;
    result = TouchEvent::kLongPress;
  }

  return result;
}

} // namespace aerolith

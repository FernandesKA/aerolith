// Aerolith: reads a Sensirion SCD41 and shows CO2/temp/humidity on the
// Sipeed M1S's onboard MIPI-DBI display.
//
// Run directly (`aerolith`) or install buildroot/S99aerolith as an init
// script for auto-start at boot.
#include "aerolith/font5x7.hpp"
#include "aerolith/framebuffer.hpp"
#include "aerolith/scd41.hpp"
#include "aerolith/touch.hpp"

#include <poll.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

using aerolith::Color;
using aerolith::FrameBuffer;
using aerolith::NextRotation;
using aerolith::Reading;
using aerolith::SCD41;
using aerolith::TouchEvent;
using aerolith::TouchInput;

constexpr Color kWhite{255, 255, 255};
constexpr Color kBlack{0, 0, 0};

enum class PomodoroState { kIdle, kRunning, kFinished };
constexpr auto kPomodoroDuration = std::chrono::minutes(25);

// CO2 thresholds (ppm) -> background color, roughly following common
// indoor air quality guidance (EN 13779 / REHVA).
struct CO2Level {
  double threshold;
  Color color;
};

constexpr CO2Level kCO2Levels[] = {
    {800.0, {20, 140, 20}},                       // good
    {1200.0, {170, 150, 0}},                      // acceptable
    {2000.0, {180, 90, 0}},                       // poor
    {std::numeric_limits<double>::infinity(), {150, 20, 20}}, // bad
};

Color StatusColor(double co2_ppm) {
  for (const auto &level : kCO2Levels) {
    if (co2_ppm < level.threshold) {
      return level.color;
    }
  }
  return kCO2Levels[std::size(kCO2Levels) - 1].color;
}

void Render(FrameBuffer &fb, const Reading *reading, const char *error) {
  fb.Clear(kBlack);
  fb.DrawTextCentered(6, "AEROLITH", kWhite, 2);

  if (error != nullptr) {
    fb.DrawTextCentered(40, "SENSOR", Color{200, 40, 40}, 3);
    fb.DrawTextCentered(70, "ERROR", Color{200, 40, 40}, 3);
    fb.Flush();
    return;
  }

  const Color band = StatusColor(reading->co2_ppm);
  fb.FillRect(0, 30, fb.xres(), 90, band);

  const std::string co2_text = std::to_string(static_cast<long>(reading->co2_ppm + 0.5));
  const int scale = 6;
  fb.DrawTextCentered(45, co2_text, kWhite, scale);
  fb.DrawTextCentered(45 + 7 * scale + 4, "PPM CO2", kWhite, 2);

  char temp_buf[32];
  std::snprintf(temp_buf, sizeof(temp_buf), "TEMP %.1fC", reading->temperature_c);
  fb.DrawTextCentered(135, temp_buf, kWhite, 2);

  char hum_buf[32];
  std::snprintf(hum_buf, sizeof(hum_buf), "HUM %.0f%%", reading->humidity_rh);
  fb.DrawTextCentered(160, hum_buf, kWhite, 2);

  fb.Flush();
}

void RenderPomodoro(FrameBuffer &fb, PomodoroState state, std::chrono::seconds remaining) {
  fb.Clear(kBlack);
  fb.DrawTextCentered(6, "POMODORO", kWhite, 2);

  if (state == PomodoroState::kFinished) {
    fb.FillRect(0, 30, fb.xres(), 90, Color{150, 20, 20});
    fb.DrawTextCentered(45, "DONE", kWhite, 6);
    fb.Flush();
    return;
  }

  fb.FillRect(0, 30, fb.xres(), 90, Color{190, 70, 30});
  const int total_seconds = static_cast<int>(std::max<long>(remaining.count(), 0) % 3600);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", total_seconds / 60, total_seconds % 60);
  fb.DrawTextCentered(45, buf, kWhite, 6);
  fb.Flush();
}

struct Args {
  std::string fb = "/dev/fb0";
  std::string iio_path;
  std::string touch_path = "/dev/input/event0";
  double interval = 5.0;
  bool once = false;
};

void PrintUsage(const char *prog) {
  std::printf(
      "Usage: %s [--fb DEV] [--iio-path PATH] [--touch-path PATH] "
      "[--interval SECONDS] [--once]\n"
      "\n"
      "  --fb DEV            framebuffer device (default: /dev/fb0)\n"
      "  --iio-path PATH     explicit /sys/bus/iio/devices/iio:deviceN path\n"
      "                      (auto-detected by driver name otherwise)\n"
      "  --touch-path PATH   touchscreen evdev node (default: /dev/input/event0);\n"
      "                      a short tap near the center rotates the display,\n"
      "                      a long press starts/cancels a 25-minute Pomodoro timer\n"
      "  --interval SECONDS  seconds between reads (default: 5.0)\n"
      "  --once              read and render a single frame, then exit\n",
      prog);
}

bool ParseArgs(int argc, char **argv, Args *args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next_value = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "aerolith: missing value for %s\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--fb") {
      args->fb = next_value("--fb");
    } else if (arg == "--iio-path") {
      args->iio_path = next_value("--iio-path");
    } else if (arg == "--touch-path") {
      args->touch_path = next_value("--touch-path");
    } else if (arg == "--interval") {
      args->interval = std::atof(next_value("--interval"));
    } else if (arg == "--once") {
      args->once = true;
    } else if (arg == "-h" || arg == "--help") {
      PrintUsage(argv[0]);
      return false;
    } else {
      std::fprintf(stderr, "aerolith: unrecognized argument: %s\n", arg.c_str());
      PrintUsage(argv[0]);
      std::exit(2);
    }
  }
  return true;
}

volatile std::sig_atomic_t g_running = 1;

void HandleStop(int) { g_running = 0; }

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    return 0;
  }

  std::signal(SIGTERM, HandleStop);
  std::signal(SIGINT, HandleStop);

  SCD41 sensor(args.iio_path);

  FrameBuffer fb(args.fb);
  TouchInput touch(args.touch_path);

  std::optional<Reading> last_reading;
  std::string last_error;
  PomodoroState pomo_state = PomodoroState::kIdle;
  std::chrono::steady_clock::time_point pomo_deadline;
  long last_rendered_pomo_seconds = -1;

  auto RenderCurrent = [&] {
    if (pomo_state == PomodoroState::kIdle) {
      Render(fb, last_reading ? &*last_reading : nullptr,
             last_reading ? nullptr : last_error.c_str());
      return;
    }
    const auto remaining = pomo_state == PomodoroState::kRunning
                                ? std::chrono::duration_cast<std::chrono::seconds>(
                                      pomo_deadline - std::chrono::steady_clock::now())
                                : std::chrono::seconds(0);
    RenderPomodoro(fb, pomo_state, remaining);
  };

  auto ReadSensor = [&] {
    try {
      last_reading = sensor.Read();
      last_error.clear();
    } catch (const std::exception &exc) {
      std::fprintf(stderr, "aerolith: sensor read failed: %s\n", exc.what());
      last_reading.reset();
      last_error = exc.what();
    }
  };

  ReadSensor();
  RenderCurrent();

  if (args.once) {
    fb.Clear(kBlack);
    fb.Flush();
    return 0;
  }

  // The SCD41 IIO driver re-triggers a fresh multi-second measurement on
  // every raw-attribute read, so a single Reading() can block for 15-30s.
  // Doing that on the main thread would freeze touch polling and rendering
  // for the whole span, so it runs on its own thread; the two threads only
  // communicate through reading_mutex-guarded state below.
  std::mutex reading_mutex;
  std::optional<Reading> shared_reading = last_reading;
  std::string shared_error = last_error;
  std::atomic<bool> reading_dirty{false};

  std::thread sensor_thread([&] {
    while (g_running) {
      for (int i = 0; i < static_cast<int>(args.interval * 10) && g_running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!g_running) {
        break;
      }
      std::optional<Reading> reading;
      std::string error;
      try {
        reading = sensor.Read();
      } catch (const std::exception &exc) {
        std::fprintf(stderr, "aerolith: sensor read failed: %s\n", exc.what());
        error = exc.what();
      }
      {
        std::lock_guard<std::mutex> lock(reading_mutex);
        shared_reading = reading;
        shared_error = error;
      }
      reading_dirty = true;
    }
  });

  while (g_running) {
    if (touch.valid()) {
      pollfd pfd{touch.fd(), POLLIN, 0};
      poll(&pfd, 1, 100);
      switch (touch.Poll()) {
        case TouchEvent::kShortTap:
          fb.SetRotation(NextRotation(fb.rotation()));
          RenderCurrent();
          break;
        case TouchEvent::kLongPress:
          pomo_state = pomo_state == PomodoroState::kIdle ? PomodoroState::kRunning
                                                            : PomodoroState::kIdle;
          if (pomo_state == PomodoroState::kRunning) {
            pomo_deadline = std::chrono::steady_clock::now() + kPomodoroDuration;
          }
          last_rendered_pomo_seconds = -1;
          RenderCurrent();
          break;
        case TouchEvent::kNone:
          break;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (reading_dirty.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(reading_mutex);
        last_reading = shared_reading;
        last_error = shared_error;
      }
      if (pomo_state == PomodoroState::kIdle) {
        RenderCurrent();
      }
    }

    if (pomo_state == PomodoroState::kRunning) {
      const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
          pomo_deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        pomo_state = PomodoroState::kFinished;
        last_rendered_pomo_seconds = -1;
        RenderCurrent();
      } else if (remaining.count() != last_rendered_pomo_seconds) {
        last_rendered_pomo_seconds = remaining.count();
        RenderCurrent();
      }
    }
  }

  sensor_thread.join();

  fb.Clear(kBlack);
  fb.Flush();

  return 0;
}

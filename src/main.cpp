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
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
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

enum class PomodoroState { kIdle, kRunning, kPaused, kFinished };
enum class PomodoroPhase { kWork, kBreak };
// Which screen is currently on the display, independent of Pomodoro state:
// a session keeps running/counting down in the background regardless of
// which screen is visible, and a swipe just changes what's visible.
enum class Screen { kSensor, kPomodoro };
constexpr auto kPomodoroDuration = std::chrono::minutes(25);
constexpr auto kBreakDuration = std::chrono::minutes(5);

// Daily Pomodoro stats, kept in memory and mirrored to kStatsPath so a
// restart (or a power cycle overnight) doesn't lose today's count. Rolls
// over automatically the first time it's touched on a new calendar day.
constexpr const char *kStatsPath = "/var/lib/aerolith/pomodoro_stats";

std::string TodayDateString() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  ::localtime_r(&t, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

struct DailyStats {
  std::string date = TodayDateString();
  int sessions = 0;
  int minutes = 0;
};

DailyStats LoadStats(const std::string &path) {
  DailyStats stats;
  std::ifstream f(path);
  std::string date;
  int sessions = 0, minutes = 0;
  if (f && (f >> date >> sessions >> minutes) && date == stats.date) {
    stats.sessions = sessions;
    stats.minutes = minutes;
  }
  return stats;
}

void SaveStats(const std::string &path, const DailyStats &stats) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream f(path, std::ios::trunc);
  if (f) {
    f << stats.date << ' ' << stats.sessions << ' ' << stats.minutes << '\n';
  }
}

// Rolls the in-memory stats over to today if the date has changed (e.g.
// the app has been running since before midnight). No-op, no write, if
// already current.
void RollStatsIfNewDay(DailyStats *stats, const std::string &path) {
  const std::string today = TodayDateString();
  if (stats->date == today) {
    return;
  }
  stats->date = today;
  stats->sessions = 0;
  stats->minutes = 0;
  SaveStats(path, *stats);
}

// A transient sensor read failure (I2C hiccup, single-shot measurement
// glitch) shouldn't flip the whole screen to an error state -- keep showing
// the last good reading until it's been this long since it was current.
constexpr auto kStaleGracePeriod = std::chrono::minutes(3);

// Rightmost slice of the screen (in current-rotation logical space) that
// starts a brightness drag; kept well clear of TouchInput's own center
// zone so the two gestures never both claim the same touch.
constexpr double kBrightnessEdgeZone = 0.85;

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

struct CO2Sample {
  std::chrono::steady_clock::time_point time;
  double co2_ppm;
};

constexpr auto kHistoryWindow = std::chrono::hours(1);
constexpr int kGraphBuckets = 60; // one per minute over the window

// Bucketed bar graph of the last hour of CO2 readings, drawn under the rest
// of the main screen. Buckets with no sample in them (sensor read failed,
// or the app just started) are left blank rather than interpolated.
void DrawCO2Graph(FrameBuffer &fb, const std::deque<CO2Sample> &history) {
  constexpr int kLabelY = 178;
  constexpr int kGraphY0 = 188;
  constexpr int kGraphY1 = 236;
  constexpr int kMarginX = 4;

  fb.DrawTextCentered(kLabelY, "CO2 1H", Color{140, 140, 140}, 1);

  const int x0 = kMarginX;
  const int width = fb.xres() - 2 * kMarginX;
  const int graph_h = kGraphY1 - kGraphY0;
  fb.FillRect(x0, kGraphY0, width, graph_h, Color{25, 25, 25});
  if (width <= 0) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  std::array<double, kGraphBuckets> sum{};
  std::array<int, kGraphBuckets> count{};
  for (const auto &s : history) {
    const double age_s = std::chrono::duration<double>(now - s.time).count();
    if (age_s < 0.0 || age_s > std::chrono::duration<double>(kHistoryWindow).count()) {
      continue;
    }
    const int bucket_from_right = static_cast<int>(age_s / 60.0);
    const int idx = kGraphBuckets - 1 - bucket_from_right;
    if (idx < 0 || idx >= kGraphBuckets) {
      continue;
    }
    sum[idx] += s.co2_ppm;
    count[idx] += 1;
  }

  double min_v = std::numeric_limits<double>::infinity();
  double max_v = -std::numeric_limits<double>::infinity();
  std::array<double, kGraphBuckets> value{};
  std::array<bool, kGraphBuckets> has{};
  bool any = false;
  for (int i = 0; i < kGraphBuckets; ++i) {
    if (count[i] == 0) {
      has[i] = false;
      continue;
    }
    value[i] = sum[i] / count[i];
    has[i] = true;
    any = true;
    min_v = std::min(min_v, value[i]);
    max_v = std::max(max_v, value[i]);
  }
  if (!any) {
    return;
  }
  if (max_v - min_v < 100.0) {
    const double mid = (max_v + min_v) / 2.0;
    min_v = mid - 50.0;
    max_v = mid + 50.0;
  }

  const double step = static_cast<double>(width) / kGraphBuckets;
  for (int i = 0; i < kGraphBuckets; ++i) {
    if (!has[i]) {
      continue;
    }
    const int x = x0 + static_cast<int>(i * step);
    const int x_next = x0 + static_cast<int>((i + 1) * step);
    const int bar_w = std::max(1, x_next - x - 1);
    const double frac = (value[i] - min_v) / (max_v - min_v);
    const int bar_h = std::clamp(static_cast<int>(frac * graph_h + 0.5), 1, graph_h);
    fb.FillRect(x, kGraphY1 - bar_h, bar_w, bar_h, StatusColor(value[i]));
  }
}

void Render(FrameBuffer &fb, const Reading *reading, const char *error,
            const std::deque<CO2Sample> &history) {
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

  DrawCO2Graph(fb, history);

  fb.Flush();
}

void DrawDailyStats(FrameBuffer &fb, int y, const DailyStats &stats) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "TODAY %d SESSIONS  %d MIN", stats.sessions, stats.minutes);
  fb.DrawTextCentered(y, buf, Color{170, 170, 170}, 1);
}

void RenderPomodoro(FrameBuffer &fb, PomodoroState state, PomodoroPhase phase,
                     std::chrono::seconds remaining, const DailyStats &stats) {
  fb.Clear(kBlack);
  fb.DrawTextCentered(6, phase == PomodoroPhase::kBreak ? "BREAK" : "POMODORO", kWhite, 2);

  if (state == PomodoroState::kIdle) {
    fb.FillRect(0, 30, fb.xres(), 90, Color{190, 70, 30});
    char buf[16];
    const auto mins = std::chrono::duration_cast<std::chrono::minutes>(kPomodoroDuration).count();
    std::snprintf(buf, sizeof(buf), "%02d:00", static_cast<int>(mins));
    fb.DrawTextCentered(45, buf, kWhite, 6);
    fb.DrawTextCentered(135, "HOLD CENTER TO START", Color{170, 170, 170}, 1);
    DrawDailyStats(fb, 155, stats);
    fb.DrawTextCentered(175, "HOLD CORNER TO RESET", Color{100, 100, 100}, 1);
    fb.Flush();
    return;
  }

  if (state == PomodoroState::kFinished) {
    fb.FillRect(0, 30, fb.xres(), 90, Color{150, 20, 20});
    fb.DrawTextCentered(45, "DONE", kWhite, 6);
    fb.DrawTextCentered(
        135, phase == PomodoroPhase::kWork ? "HOLD FOR BREAK" : "HOLD FOR NEXT", Color{170, 170, 170}, 1);
    DrawDailyStats(fb, 155, stats);
    fb.Flush();
    return;
  }

  // Running or Paused -- same layout, just a dimmer band and different
  // hints while paused so the frozen time doesn't read as a live countdown.
  const bool paused = state == PomodoroState::kPaused;
  const Color band = paused ? Color{90, 90, 90}
                             : phase == PomodoroPhase::kBreak ? Color{30, 110, 170}
                                                               : Color{190, 70, 30};
  fb.FillRect(0, 30, fb.xres(), 90, band);
  const int total_seconds = static_cast<int>(std::max<long>(remaining.count(), 0) % 3600);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", total_seconds / 60, total_seconds % 60);
  fb.DrawTextCentered(45, buf, kWhite, 6);
  fb.DrawTextCentered(135, paused ? "PAUSED - TAP TO RESUME" : "TAP TO PAUSE", Color{170, 170, 170},
                       1);
  DrawDailyStats(fb, 155, stats);
  fb.DrawTextCentered(175, "HOLD TO STOP", Color{100, 100, 100}, 1);
  fb.Flush();
}

struct Args {
  std::string fb = "/dev/fb0";
  std::string iio_path;
  std::string touch_path = "/dev/input/event0";
  std::string stats_path = kStatsPath;
  double interval = 5.0;
  bool once = false;
};

void PrintUsage(const char *prog) {
  std::printf(
      "Usage: %s [--fb DEV] [--iio-path PATH] [--touch-path PATH] "
      "[--stats-path PATH] [--interval SECONDS] [--once]\n"
      "\n"
      "  --fb DEV            framebuffer device (default: /dev/fb0)\n"
      "  --iio-path PATH     explicit /sys/bus/iio/devices/iio:deviceN path\n"
      "                      (auto-detected by driver name otherwise); if no\n"
      "                      SCD41 is found at startup, the display falls back\n"
      "                      to the Pomodoro screen instead of a CO2 readout\n"
      "  --stats-path PATH   where daily Pomodoro stats are persisted\n"
      "                      (default: %s)\n"
      "  --touch-path PATH   touchscreen evdev node (default: /dev/input/event0);\n"
      "                      three quick taps near the center rotate the display\n"
      "                      (a single tap doesn't -- too easy to trigger by\n"
      "                      accident -- except on the Pomodoro screen during a\n"
      "                      session, where a single tap pauses/resumes it), a\n"
      "                      left/right swipe switches between the CO2 and\n"
      "                      Pomodoro screens, a long press near the center\n"
      "                      starts a Pomodoro work session, stops the current\n"
      "                      one (with a reset), or advances to the next phase\n"
      "                      once one finishes, and a longer press held in the\n"
      "                      bottom-left corner resets today's Pomodoro stats\n"
      "                      (only while idle on the Pomodoro screen)\n"
      "  --interval SECONDS  seconds between reads (default: 5.0)\n"
      "  --once              read and render a single frame, then exit\n",
      prog, kStatsPath);
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
    } else if (arg == "--stats-path") {
      args->stats_path = next_value("--stats-path");
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

  // If no SCD41 is wired up (or the kernel hasn't detected one), there's
  // nothing meaningful to show on the CO2 screen -- fall back to the
  // Pomodoro screen as the default view instead of a permanent error
  // display.
  std::optional<SCD41> sensor;
  bool sensor_present = true;
  try {
    sensor.emplace(args.iio_path);
  } catch (const std::exception &exc) {
    std::fprintf(stderr, "aerolith: no CO2 sensor detected, defaulting to Pomodoro: %s\n",
                 exc.what());
    sensor_present = false;
  }

  FrameBuffer fb(args.fb);
  TouchInput touch(args.touch_path);

  // last_reading is the last *successful* reading and is never cleared on a
  // failed read -- only overwritten by the next success. Staleness is
  // judged from last_reading_time instead, so a transient read failure
  // keeps showing the old value rather than flipping to an error screen.
  std::optional<Reading> last_reading;
  std::chrono::steady_clock::time_point last_reading_time;
  std::string last_error;
  std::deque<CO2Sample> co2_history;
  PomodoroState pomo_state = PomodoroState::kIdle;
  PomodoroPhase pomo_phase = PomodoroPhase::kWork;
  std::chrono::steady_clock::time_point pomo_deadline;
  // Frozen remaining time as of the moment a running session was paused;
  // resuming re-derives a fresh pomo_deadline from this instead of the
  // (by-then meaningless) old deadline.
  std::chrono::seconds pomo_paused_remaining{0};
  long last_rendered_pomo_seconds = -1;
  bool brightness_dragging = false;
  DailyStats stats = LoadStats(args.stats_path);
  Screen current_screen = sensor_present ? Screen::kSensor : Screen::kPomodoro;

  auto RenderCurrent = [&] {
    RollStatsIfNewDay(&stats, args.stats_path);
    if (current_screen == Screen::kSensor && sensor_present) {
      const bool stale = !last_reading.has_value() ||
                          std::chrono::steady_clock::now() - last_reading_time > kStaleGracePeriod;
      Render(fb, stale ? nullptr : &*last_reading, stale ? last_error.c_str() : nullptr,
             co2_history);
      return;
    }
    const auto remaining = pomo_state == PomodoroState::kRunning
                                ? std::chrono::duration_cast<std::chrono::seconds>(
                                      pomo_deadline - std::chrono::steady_clock::now())
                            : pomo_state == PomodoroState::kPaused ? pomo_paused_remaining
                                                                    : std::chrono::seconds(0);
    RenderPomodoro(fb, pomo_state, pomo_phase, remaining, stats);
  };

  auto ReadSensor = [&] {
    if (!sensor_present) {
      return;
    }
    try {
      last_reading = sensor->Read();
      last_reading_time = std::chrono::steady_clock::now();
      last_error.clear();
    } catch (const std::exception &exc) {
      std::fprintf(stderr, "aerolith: sensor read failed: %s\n", exc.what());
      last_error = exc.what();
    }
  };

  auto RecordHistory = [&] {
    const auto now = std::chrono::steady_clock::now();
    if (last_reading) {
      co2_history.push_back({now, last_reading->co2_ppm});
    }
    while (!co2_history.empty() && now - co2_history.front().time > kHistoryWindow) {
      co2_history.pop_front();
    }
  };

  ReadSensor();
  RecordHistory();
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
  std::chrono::steady_clock::time_point shared_reading_time = last_reading_time;
  std::string shared_error = last_error;
  std::atomic<bool> reading_dirty{false};

  std::thread sensor_thread;
  if (sensor_present) {
    sensor_thread = std::thread([&] {
      while (g_running) {
        for (int i = 0; i < static_cast<int>(args.interval * 10) && g_running; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!g_running) {
          break;
        }
        // On failure, shared_reading/shared_reading_time are left untouched
        // so the last successful reading survives a transient error.
        try {
          Reading reading = sensor->Read();
          std::lock_guard<std::mutex> lock(reading_mutex);
          shared_reading = reading;
          shared_reading_time = std::chrono::steady_clock::now();
          shared_error.clear();
        } catch (const std::exception &exc) {
          std::fprintf(stderr, "aerolith: sensor read failed: %s\n", exc.what());
          std::lock_guard<std::mutex> lock(reading_mutex);
          shared_error = exc.what();
        }
        reading_dirty = true;
      }
    });
  }

  while (g_running) {
    if (touch.valid()) {
      pollfd pfd{touch.fd(), POLLIN, 0};
      poll(&pfd, 1, 100);
      switch (touch.Poll()) {
        case TouchEvent::kShortTap:
          // A tap no longer rotates -- that was too easy to trigger by
          // accident. On the Pomodoro screen, a tap during a session still
          // pauses/resumes it.
          if (current_screen == Screen::kPomodoro && pomo_state == PomodoroState::kRunning) {
            pomo_paused_remaining = std::max(
                std::chrono::seconds(0), std::chrono::duration_cast<std::chrono::seconds>(
                                              pomo_deadline - std::chrono::steady_clock::now()));
            pomo_state = PomodoroState::kPaused;
            last_rendered_pomo_seconds = -1;
            RenderCurrent();
          } else if (current_screen == Screen::kPomodoro && pomo_state == PomodoroState::kPaused) {
            pomo_deadline = std::chrono::steady_clock::now() + pomo_paused_remaining;
            pomo_state = PomodoroState::kRunning;
            last_rendered_pomo_seconds = -1;
            RenderCurrent();
          }
          break;
        case TouchEvent::kTripleTap:
          fb.SetRotation(NextRotation(fb.rotation()));
          RenderCurrent();
          break;
        case TouchEvent::kLongPress:
          switch (pomo_state) {
            case PomodoroState::kIdle:
              // Start a work session.
              pomo_state = PomodoroState::kRunning;
              pomo_phase = PomodoroPhase::kWork;
              pomo_deadline = std::chrono::steady_clock::now() + kPomodoroDuration;
              break;
            case PomodoroState::kRunning:
            case PomodoroState::kPaused:
              // Stop: discard all progress on the current session/break, back
              // to idle ready to start a fresh one.
              pomo_state = PomodoroState::kIdle;
              pomo_phase = PomodoroPhase::kWork;
              break;
            case PomodoroState::kFinished:
              if (pomo_phase == PomodoroPhase::kWork) {
                // Work session done -- start the break.
                pomo_phase = PomodoroPhase::kBreak;
                pomo_state = PomodoroState::kRunning;
                pomo_deadline = std::chrono::steady_clock::now() + kBreakDuration;
              } else {
                // Break done -- back to idle, ready for the next session.
                pomo_phase = PomodoroPhase::kWork;
                pomo_state = PomodoroState::kIdle;
              }
              break;
          }
          // Surface the result of the gesture even if the sensor screen was
          // showing -- the session keeps running/counting down regardless of
          // which screen is visible, but the user should see what they just did.
          current_screen = Screen::kPomodoro;
          last_rendered_pomo_seconds = -1;
          RenderCurrent();
          break;
        case TouchEvent::kCornerLongPress:
          // Destructive, so only honored from the idle Pomodoro screen
          // (not mid-session, and not blindly from the sensor screen).
          if (pomo_state == PomodoroState::kIdle && current_screen == Screen::kPomodoro) {
            stats = DailyStats{};
            SaveStats(args.stats_path, stats);
            RenderCurrent();
          }
          break;
        case TouchEvent::kSwipeLeft:
        case TouchEvent::kSwipeRight:
          // Only one screen exists without a sensor, so there's nothing to
          // swipe to.
          if (sensor_present) {
            current_screen =
                current_screen == Screen::kSensor ? Screen::kPomodoro : Screen::kSensor;
            RenderCurrent();
          }
          break;
        case TouchEvent::kNone:
          break;
      }

      // Brightness slider: dragging in the rightmost strip of the *physical*
      // panel sets the level directly from finger height, like a physical
      // fader -- top of the strip is full brightness, bottom is dimmest.
      // Deliberately NOT rotation-aware: the device sits in one fixed
      // physical position and SetRotation just picks how the content reads
      // from there, so a hand reaching for "the edge of the screen" always
      // reaches for the same physical spot regardless of the current
      // rotation. Starting a drag anywhere else never engages it, and
      // lifting the finger or leaving the panel disengages it, so it can't
      // be triggered by the center tap/long-press gestures above.
      double fx = 0.0, fy = 0.0;
      if (touch.Touching() && touch.NormalizedPosition(&fx, &fy)) {
        if (std::getenv("AEROLITH_DEBUG_TOUCH") != nullptr) {
          std::fprintf(stderr, "touch: raw=(%.3f,%.3f) dragging=%d\n", fx, fy,
                       brightness_dragging);
        }
        if (!brightness_dragging && fx >= kBrightnessEdgeZone) {
          brightness_dragging = true;
        }
        if (brightness_dragging) {
          const double level = 1.0 - fy;
          if (std::abs(level - fb.brightness()) > 0.01) {
            fb.SetBrightness(level);
            RenderCurrent();
          }
        }
      } else {
        brightness_dragging = false;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (reading_dirty.exchange(false)) {
      const auto previous_reading_time = last_reading_time;
      {
        std::lock_guard<std::mutex> lock(reading_mutex);
        last_reading = shared_reading;
        last_reading_time = shared_reading_time;
        last_error = shared_error;
      }
      // Only a genuinely fresh reading (not a re-affirmed stale one) belongs
      // in the CO2 history graph.
      if (last_reading_time != previous_reading_time) {
        RecordHistory();
      }
      if (current_screen == Screen::kSensor) {
        RenderCurrent();
      }
    }

    if (pomo_state == PomodoroState::kRunning) {
      const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
          pomo_deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        pomo_state = PomodoroState::kFinished;
        if (pomo_phase == PomodoroPhase::kWork) {
          RollStatsIfNewDay(&stats, args.stats_path);
          stats.sessions += 1;
          stats.minutes +=
              static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(kPomodoroDuration).count());
          SaveStats(args.stats_path, stats);
        }
        // Alert the user even if they're currently looking at the sensor
        // screen -- a finished session (or break) is worth surfacing.
        current_screen = Screen::kPomodoro;
        last_rendered_pomo_seconds = -1;
        RenderCurrent();
      } else if (remaining.count() != last_rendered_pomo_seconds) {
        last_rendered_pomo_seconds = remaining.count();
        // Bookkeeping happens every tick regardless of visibility so the
        // Pomodoro screen shows the exact remaining time the moment the
        // user swipes back to it; the redraw itself is skipped otherwise.
        if (current_screen == Screen::kPomodoro) {
          RenderCurrent();
        }
      }
    }
  }

  if (sensor_thread.joinable()) {
    sensor_thread.join();
  }

  fb.Clear(kBlack);
  fb.Flush();

  return 0;
}

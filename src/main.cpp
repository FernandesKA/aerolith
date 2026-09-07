// Aerolith: reads a Sensirion SCD41 and shows CO2/temp/humidity on the
// Sipeed M1S's onboard MIPI-DBI display.
//
// Run directly (`aerolith`) or install buildroot/S99aerolith as an init
// script for auto-start at boot.
#include "aerolith/font5x7.hpp"
#include "aerolith/framebuffer.hpp"
#include "aerolith/scd41.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

namespace {

using aerolith::Color;
using aerolith::FrameBuffer;
using aerolith::Reading;
using aerolith::SCD41;

constexpr Color kWhite{255, 255, 255};
constexpr Color kBlack{0, 0, 0};

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

struct Args {
  std::string fb = "/dev/fb0";
  std::string iio_path;
  double interval = 5.0;
  bool once = false;
};

void PrintUsage(const char *prog) {
  std::printf(
      "Usage: %s [--fb DEV] [--iio-path PATH] [--interval SECONDS] [--once]\n"
      "\n"
      "  --fb DEV            framebuffer device (default: /dev/fb0)\n"
      "  --iio-path PATH     explicit /sys/bus/iio/devices/iio:deviceN path\n"
      "                      (auto-detected by driver name otherwise)\n"
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

  while (g_running) {
    try {
      const Reading reading = sensor.Read();
      Render(fb, &reading, nullptr);
    } catch (const std::exception &exc) {
      std::fprintf(stderr, "aerolith: sensor read failed: %s\n", exc.what());
      Render(fb, nullptr, exc.what());
    }

    if (args.once) {
      break;
    }
    const int ticks = static_cast<int>(args.interval * 10);
    for (int i = 0; i < ticks && g_running; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  fb.Clear(kBlack);
  fb.Flush();

  return 0;
}

#include "aerolith/scd41.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace aerolith {
namespace {

double ReadNum(const std::string &path) {
  std::ifstream f(path);
  if (!f) {
    throw std::runtime_error("failed to open " + path);
  }
  double value;
  f >> value;
  if (!f) {
    throw std::runtime_error("failed to parse number from " + path);
  }
  return value;
}

} // namespace

std::string FindScd4xDevice(const std::string &iio_root) {
  std::error_code ec;
  std::vector<fs::path> entries;
  if (fs::exists(iio_root, ec)) {
    for (const auto &entry : fs::directory_iterator(iio_root, ec)) {
      if (entry.path().filename().string().rfind("iio:device", 0) == 0) {
        entries.push_back(entry.path());
      }
    }
  }
  std::sort(entries.begin(), entries.end());

  for (const auto &entry : entries) {
    std::ifstream name_file(entry / "name");
    if (!name_file) {
      continue;
    }
    std::string name;
    std::getline(name_file, name);
    if (name.rfind("scd4", 0) == 0) {
      return entry.string();
    }
  }
  throw std::runtime_error("no scd4x IIO device found under " + iio_root +
                            " (is the sensor wired to i2c2 and detected by the kernel?)");
}

SCD41::SCD41(std::string device_path)
    : device_path_(device_path.empty() ? FindScd4xDevice() : std::move(device_path)) {}

std::string SCD41::Path(const std::string &attr) const {
  return device_path_ + "/" + attr;
}

Reading SCD41::Read() const {
  const double co2_raw = ReadNum(Path("in_concentration_co2_raw"));
  const double co2_scale = ReadNum(Path("in_concentration_co2_scale"));
  // IIO concentration channels report percent = raw * scale;
  // ppm = percent * 10000, which for this driver's scale (1e-4)
  // collapses to ppm == raw, but we keep the sysfs scale in the
  // formula so this still works if the driver ever changes it.
  const double co2_ppm = co2_raw * co2_scale * 10000.0;

  const double temp_raw = ReadNum(Path("in_temp_raw"));
  const double temp_scale = ReadNum(Path("in_temp_scale"));
  const double temp_offset = ReadNum(Path("in_temp_offset"));
  const double temperature_c = (temp_raw + temp_offset) * temp_scale / 1000.0;

  const double hum_raw = ReadNum(Path("in_humidityrelative_raw"));
  const double hum_scale = ReadNum(Path("in_humidityrelative_scale"));
  const double humidity_rh = hum_raw * hum_scale / 1000.0;

  return Reading{co2_ppm, temperature_c, humidity_rh};
}

} // namespace aerolith

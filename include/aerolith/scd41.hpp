// Reads CO2/temperature/humidity from the in-kernel IIO scd4x driver.
//
// The Sipeed M1S board (sipeed_m1s_ext_defconfig) declares the SCD41 in
// its devicetree (i2c2, address 0x62), so the mainline `scd4x` IIO driver
// already talks to the sensor over I2C -- this module only needs to read
// the resulting sysfs attributes, no direct I2C/smbus access required.
#pragma once

#include <string>

namespace aerolith {

struct Reading {
  double co2_ppm;
  double temperature_c;
  double humidity_rh;
};

// Locates the iio:deviceN directory whose driver name starts with "scd4".
// Throws std::runtime_error if none is found.
std::string FindScd4xDevice(const std::string &iio_root = "/sys/bus/iio/devices");

class SCD41 {
public:
  // If device_path is empty, auto-detects via FindScd4xDevice().
  explicit SCD41(std::string device_path = {});

  // Reads the sensor. Throws std::runtime_error on I/O failure.
  Reading Read() const;

private:
  std::string device_path_;
  std::string Path(const std::string &attr) const;
};

} // namespace aerolith

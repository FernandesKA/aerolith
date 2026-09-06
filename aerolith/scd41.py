"""Reads CO2/temperature/humidity from the in-kernel IIO scd4x driver.

The Sipeed M1S board (sipeed_m1s_ext_defconfig) declares the SCD41 in
its devicetree (i2c2, address 0x62), so the mainline `scd4x` IIO driver
already talks to the sensor over I2C -- this module only needs to read
the resulting sysfs attributes, no direct I2C/smbus access required.
"""

import glob
import os
from collections import namedtuple

Reading = namedtuple("Reading", ["co2_ppm", "temperature_c", "humidity_rh"])

_IIO_ROOT = "/sys/bus/iio/devices"


def find_device(iio_root=_IIO_ROOT):
    """Locate the iio:deviceN directory whose driver name starts with 'scd4'."""
    for entry in sorted(glob.glob(os.path.join(iio_root, "iio:device*"))):
        name_path = os.path.join(entry, "name")
        try:
            with open(name_path) as f:
                name = f.read().strip()
        except OSError:
            continue
        if name.startswith("scd4"):
            return entry
    raise FileNotFoundError(
        f"no scd4x IIO device found under {iio_root} "
        "(is the sensor wired to i2c2 and detected by the kernel?)"
    )


def _read_num(path):
    with open(path) as f:
        return float(f.read().strip())


class SCD41:
    def __init__(self, device_path=None):
        self.device_path = device_path or find_device()

    def _path(self, attr):
        return os.path.join(self.device_path, attr)

    def read(self):
        co2_raw = _read_num(self._path("in_concentration_co2_raw"))
        co2_scale = _read_num(self._path("in_concentration_co2_scale"))
        # IIO concentration channels report percent = raw * scale;
        # ppm = percent * 10000, which for this driver's scale (1e-4)
        # collapses to ppm == raw, but we keep the sysfs scale in the
        # formula so this still works if the driver ever changes it.
        co2_ppm = co2_raw * co2_scale * 10000.0

        temp_raw = _read_num(self._path("in_temp_raw"))
        temp_scale = _read_num(self._path("in_temp_scale"))
        temp_offset = _read_num(self._path("in_temp_offset"))
        temperature_c = (temp_raw + temp_offset) * temp_scale / 1000.0

        hum_raw = _read_num(self._path("in_humidityrelative_raw"))
        hum_scale = _read_num(self._path("in_humidityrelative_scale"))
        humidity_rh = hum_raw * hum_scale / 1000.0

        return Reading(co2_ppm=co2_ppm, temperature_c=temperature_c, humidity_rh=humidity_rh)

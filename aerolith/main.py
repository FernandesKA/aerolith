#!/usr/bin/env python3
"""Aerolith: reads a Sensirion SCD41 and shows CO2/temp/humidity on the
Sipeed M1S's onboard MIPI-DBI display.

Run directly (`python3 -m aerolith.main`) or install buildroot/S99aerolith
as an init script for auto-start at boot.
"""

import argparse
import signal
import sys
import time

from .framebuffer import FrameBuffer
from .scd41 import SCD41

WHITE = (255, 255, 255)
BLACK = (0, 0, 0)

# CO2 thresholds (ppm) -> background color, roughly following common
# indoor air quality guidance (EN 13779 / REHVA).
_CO2_LEVELS = (
    (800, (20, 140, 20)),    # good
    (1200, (170, 150, 0)),   # acceptable
    (2000, (180, 90, 0)),    # poor
    (float("inf"), (150, 20, 20)),  # bad
)


def status_color(co2_ppm):
    for threshold, color in _CO2_LEVELS:
        if co2_ppm < threshold:
            return color
    return _CO2_LEVELS[-1][1]


def render(fb: FrameBuffer, reading=None, error=None):
    fb.clear(BLACK)
    fb.draw_text_centered(6, "AEROLITH", WHITE, scale=2)

    if error is not None:
        fb.draw_text_centered(40, "SENSOR", (200, 40, 40), scale=3)
        fb.draw_text_centered(70, "ERROR", (200, 40, 40), scale=3)
        fb.flush()
        return

    band = status_color(reading.co2_ppm)
    fb.fill_rect(0, 30, fb.xres, 90, band)

    co2_text = str(int(round(reading.co2_ppm)))
    scale = 6
    fb.draw_text_centered(45, co2_text, WHITE, scale=scale)
    fb.draw_text_centered(45 + 7 * scale + 4, "PPM CO2", WHITE, scale=2)

    fb.draw_text_centered(135, f"TEMP {reading.temperature_c:.1f}C", WHITE, scale=2)
    fb.draw_text_centered(160, f"HUM {reading.humidity_rh:.0f}%", WHITE, scale=2)

    fb.flush()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fb", default="/dev/fb0", help="framebuffer device")
    parser.add_argument("--iio-path", default=None,
                         help="explicit /sys/bus/iio/devices/iio:deviceN path "
                              "(auto-detected by driver name otherwise)")
    parser.add_argument("--interval", type=float, default=5.0,
                         help="seconds between reads (SCD41 updates ~every 5s)")
    parser.add_argument("--once", action="store_true",
                         help="read and render a single frame, then exit")
    args = parser.parse_args(argv)

    running = True

    def _stop(_signum, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    sensor = SCD41(args.iio_path)

    with FrameBuffer(args.fb) as fb:
        while running:
            try:
                reading = sensor.read()
            except OSError as exc:
                print(f"aerolith: sensor read failed: {exc}", file=sys.stderr)
                render(fb, error=exc)
            else:
                render(fb, reading=reading)

            if args.once:
                break
            for _ in range(int(args.interval * 10)):
                if not running:
                    break
                time.sleep(0.1)

        fb.clear(BLACK)
        fb.flush()

    return 0


if __name__ == "__main__":
    sys.exit(main())

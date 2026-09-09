# Aerolith

Reads a Sensirion SCD41 (CO2 / temperature / humidity) and shows the
readings on a Linux framebuffer -- verified on the Sipeed M1S's onboard
1.69" MIPI-DBI display.

Target board: Sipeed M1S (Bouffalo Lab BL808), built from
[FernandesKA/buildroot_custom](https://github.com/FernandesKA/buildroot_custom)
using `sipeed_m1s_ext_defconfig`.

C++17, cross-compiled with the `riscv64-buildroot-linux-gnu` SDK toolchain
from that same buildroot_custom project (`BR2_INSTALL_LIBSTDCPP=y` provides
`libstdc++.so.6` on the target rootfs, and the SDK provides
`riscv64-unknown-linux-gnu-g++`).

## How it talks to the hardware

- **Sensor**: the board's devicetree already declares the SCD41
  (`co2-sensor@62` on `i2c2`, address `0x62`, pins GPIO0_6/GPIO0_7), and the
  kernel's mainline `scd4x` IIO driver handles the I2C protocol. This
  project just reads the resulting sysfs attributes under
  `/sys/bus/iio/devices/iio:deviceN/` (auto-detected by driver name) --
  no smbus/i2c-dev code needed.
- **Display**: the devicetree also declares a `panel-mipi-dbi-spi` display
  on `spi1`, which the kernel exposes as a standard Linux framebuffer at
  `/dev/fb0`. This project writes to it directly via `FBIOGET_VSCREENINFO`
  + raw pixel writes, with its own tiny embedded 5x7 bitmap font.

Verified against a running board:

```
$ cat /sys/bus/iio/devices/iio:device0/name
scd41
$ ls /sys/bus/iio/devices/iio:device0/
in_concentration_co2_raw  in_concentration_co2_scale
in_temp_raw in_temp_scale in_temp_offset
in_humidityrelative_raw in_humidityrelative_scale
$ cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel
280,240
32
```
Framebuffer bitfields (`FBIOGET_VSCREENINFO`) confirm XRGB8888: red
offset 16, green offset 8, blue offset 0, 8 bits each.

## Layout

```
include/aerolith/
  scd41.hpp         # IIO sysfs -> Reading{co2_ppm, temperature_c, humidity_rh}
  framebuffer.hpp   # /dev/fb0 access + 5x7 text/rect drawing + rotation
  font5x7.hpp       # hand-drawn bitmap font
  touch.hpp         # evdev touchscreen -> tap/triple-tap/long-press/corner/swipe gesture detection
src/                # matching .cpp implementations + main.cpp (CLI entry point)
cmake/toolchain-riscv64-m1s.cmake
buildroot/
  S99aerolith       # sysvinit-style init script (BusyBox init)
package/aerolith/
  Config.in         # Buildroot menuconfig entry
  aerolith.mk       # Buildroot package build/install rules
external.desc, external.mk, Config.in   # BR2_EXTERNAL tree root files
```

## Building manually

```
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-m1s.cmake \
  -DTOOLCHAIN_ROOT=/path/to/riscv64-buildroot-linux-gnu_sdk-buildroot
cmake --build build -- -j$(nproc)
```

`TOOLCHAIN_ROOT` can also be set via the `AEROLITH_TOOLCHAIN_ROOT`
environment variable instead of `-D` (needed because CMake's compiler-ABI
`try_compile` step doesn't forward arbitrary `-D` cache variables).

This produces `build/aerolith`, a dynamically-linked riscv64 ELF binary
(needs `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6` --
all already present in the buildroot_custom target rootfs).

Sanity-check the binary without hardware using `qemu-user`:

```
qemu-riscv64-static -L /path/to/toolchain/riscv64-buildroot-linux-gnu/sysroot \
  build/aerolith --help
```

(`--once`/normal runs will still fail under qemu since there's no real
`/sys/bus/iio` SCD41 device or `/dev/fb0` framebuffer on the host -- that
part can only be verified on the actual board.)

### Running it on the board

```
scp build/aerolith root@<board-ip>:/usr/bin/aerolith
ssh root@<board-ip> /usr/bin/aerolith --once   # single frame, to try it
```

Flags: `--fb DEV` (default `/dev/fb0`), `--iio-path PATH` (auto-detected
otherwise), `--touch-path PATH` (default `/dev/input/event0`), `--stats-path
PATH` (default `/var/lib/aerolith/pomodoro_stats`), `--interval SECONDS`
(default 5), `--once` (render a single frame and exit).

### Rotating the display via a triple tap

The board's onboard CST816x touchscreen (`/dev/input/event0`) is polled
continuously. Rotation is triggered by three short taps near the center in
quick succession, cycling the display 0° -> 90° -> 180° -> 270° -> 0° ...
A single tap deliberately does *not* rotate the display (it was too easy
to trigger by accident, and an earlier "trace a circle" gesture that
replaced it turned out unreliable in practice); the single tap is instead
reserved for pausing/resuming a Pomodoro session (see below).

A completed tap doesn't fire right away -- `TouchInput::Poll` in
[touch.cpp](src/touch.cpp) holds it for a short window (350ms) to see
whether another tap follows. A third tap within that window fires the
rotation immediately; letting the window lapse with only one (or two, which
has no meaning of its own) pending tap resolves it as a plain tap instead.
That short hold is the trade-off for a triple tap being distinguishable
from three separate plain taps at all -- pausing/resuming a running
Pomodoro session via tap is very slightly delayed for the same reason.

Rendering is done in a logical coordinate space that swaps width/height
at 90°/270° and is then mapped onto the panel's fixed physical pixel
layout (`FrameBuffer::SetRotation`/`Rotation` in
[framebuffer.hpp](include/aerolith/framebuffer.hpp)), so drawing code
doesn't need to know about the current orientation. If no touch device is
present (or `--touch-path` points at nothing), this feature is silently
unavailable and the app otherwise runs as normal.

### Switching screens via swipe

When a sensor is present, a quick left or right swipe (released within
600ms, moving at least a quarter of the panel's width, staying roughly
horizontal) toggles the display between the CO2 screen and the Pomodoro
screen. A Pomodoro session keeps running in the background regardless of
which screen is currently showing -- swiping away from it doesn't pause
the countdown, and the app automatically swipes back to the Pomodoro
screen when a session or break finishes so the alert isn't missed. A
swipe starting in the brightness slider's edge strip or the stats-reset
corner defers to that gesture instead. Like the rotation triple tap, swipe
recognition isn't itself rotation-aware -- it's measured in the panel's
fixed physical coordinate space.

### Pomodoro screen

If no SCD41 is detected at startup (`FindScd4xDevice` finds nothing under
`/sys/bus/iio/devices`), there's no CO2 reading to show, so the app falls
back to the Pomodoro screen as its default (and only) view instead of a
permanent "SENSOR ERROR" display. When a sensor *is* present, reach the
Pomodoro screen either by swiping or with a long press near the center of
the touch surface, which also starts a 25-minute work session.

The Pomodoro flow includes a break, pause, and daily stats:

- **Idle**: shows today's completed session count and total focused
  minutes. A long press near the center starts a 25-minute work session.
- **Running (work or break)**: counts down. A short tap pauses it, and a
  long press stops it -- discarding the remaining time, back to idle ready
  to start a fresh session (a completed work session was already logged to
  today's stats the moment it finished, so stopping it early doesn't undo
  that).
- **Paused**: the countdown freezes at whatever time was left. A short tap
  resumes it from there; a long press stops it the same as while running.
- **Finished (work)**: the session is logged to today's stats immediately;
  a long press starts a 5-minute break.
- **Finished (break)**: a long press returns to idle, ready for the next
  session.

Daily stats persist across restarts at `--stats-path` (default
`/var/lib/aerolith/pomodoro_stats`) and reset automatically at midnight.
To reset them manually, hold a long press (about 1.5s) in the bottom-left
corner of the touch surface while the Pomodoro screen is idle -- kept as a
separate, out-of-the-way gesture (and disabled outside the idle screen)
since it's destructive.

To auto-start at boot without a full package build, install the init
script directly:

```
cp buildroot/S99aerolith /etc/init.d/S99aerolith
chmod +x /etc/init.d/S99aerolith
/etc/init.d/S99aerolith start
```

## Buildroot package integration

This repo is a self-contained [`BR2_EXTERNAL`](https://buildroot.org/downloads/manual/manual.html#outside-br-custom)
tree, so it can be built into a Buildroot image without copying any files
into `buildroot_custom`:

```
cd /path/to/buildroot_custom
make BR2_EXTERNAL=/path/to/aerolith sipeed_m1s_ext_defconfig
make menuconfig    # enable "aerolith" under External options -> aerolith
make
```

(If you already pass other `BR2_EXTERNAL` trees, Buildroot accepts a
colon-separated list, or accumulates them from a prior invocation via
`.br-external.conf` in the output directory.)

Enabling `BR2_PACKAGE_AEROLITH`:

- fetches this repo (see `AEROLITH_VERSION`/`AEROLITH_SITE` in
  [package/aerolith/aerolith.mk](package/aerolith/aerolith.mk) -- pin this
  to a tag/commit once one exists, instead of tracking `main`),
- builds it with the target CMake toolchain via Buildroot's
  `cmake-package` infrastructure,
- installs the binary to `/usr/bin/aerolith` on the target,
- and installs `buildroot/S99aerolith` to `/etc/init.d/S99aerolith` so it
  starts automatically (BusyBox/sysvinit style).

For local package development, use Buildroot's package override
mechanism (`BR2_PACKAGE_AEROLITH_OVERRIDE_SRCDIR` in a
`local.mk`/override file, or `make aerolith-rebuild`) to point it at a
local working copy instead of re-fetching from GitHub each time.

## A note on the serial console port

The task description said the board's console is at `/dev/ttyACM2`, but
on the machine used to verify this, the Bouffalo USB-serial adapter
enumerated its console UART as `/dev/ttyACM1` (interface 00) with
`/dev/ttyACM2` (interface 02) staying silent -- both show up as
`Bouffalo_Serial` in udev, so which one is the shell console can depend on
enumeration order on your host. If `/dev/ttyACM2` doesn't respond, try the
adjacent `ttyACM` device.

## CO2 color bands

The background behind the big CO2 number changes with the reading
(rough EN 13779 / REHVA indoor air-quality bands):

| ppm       | color  |
|-----------|--------|
| < 800     | green  |
| 800-1200  | yellow |
| 1200-2000 | orange |
| > 2000    | red    |

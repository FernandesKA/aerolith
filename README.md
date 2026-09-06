# Aerolith

Reads a Sensirion SCD41 (CO2 / temperature / humidity) and shows the
readings on the Sipeed M1S's onboard 1.69" MIPI-DBI display.

Target board: Sipeed M1S (Bouffalo Lab BL808), built from
[FernandesKA/buildroot_custom](https://github.com/FernandesKA/buildroot_custom)
using `sipeed_m1s_ext_defconfig`.

## Why there's no C code or extra libraries

The defconfig ships no target C compiler and no Pillow/numpy, but it does
build a full Python 3.14 for the target (confirmed live on the board), so
this project is plain Python 3 stdlib only:

- **Sensor**: the board's devicetree already declares the SCD41
  (`co2-sensor@62` on `i2c2`, address `0x62`, pins GPIO0_6/GPIO0_7), and the
  kernel's mainline `scd4x` IIO driver handles the I2C protocol. This
  project just reads the resulting sysfs attributes under
  `/sys/bus/iio/devices/iio:deviceN/` (auto-detected by driver name) --
  no smbus/i2c-dev code needed.
- **Display**: the devicetree also declares a `panel-mipi-dbi-spi` display
  on `spi1`, which the kernel exposes as a standard Linux framebuffer at
  `/dev/fb0`. This project writes to it directly via `FBIOGET_VSCREENINFO`
  + raw pixel writes, with its own tiny embedded 5x7 bitmap font (no
  FreeType/Pillow available on target).

Everything above was verified against a running board, not just read out
of the defconfig:

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
aerolith/
  scd41.py        # IIO sysfs -> Reading(co2_ppm, temperature_c, humidity_rh)
  framebuffer.py  # /dev/fb0 access + 5x7 text/rect drawing
  font5x7.py      # hand-drawn bitmap font (digits, A C E H I L M O P R T U, . - %)
  main.py         # render loop, CLI entry point
buildroot/
  S99aerolith     # sysvinit-style init script (BusyBox init, matches this
                  # board's /etc/init.d/S* convention)
```

## Running it on the board

The board runs a Buildroot rootfs (BusyBox init, dropbear SSH, no systemd,
47MB RAM total -- keep it lightweight).

1. Copy the `aerolith/` package to the board, e.g. over the network
   (dropbear/SSH is enabled: `S50dropbear`):

   ```
   scp -r aerolith root@<board-ip>:/usr/local/lib/aerolith
   ```

   Or, with no network available yet, copy it in over the serial console
   (`/dev/ttyACM2` at 2000000 baud per the board's `stdout-path` /
   bootargs) using any base64-paste-and-decode approach, since there's no
   file transfer protocol over a plain serial line here.

2. Run it directly to try it out:

   ```
   cd /usr/local/lib && python3 -m aerolith.main
   ```

   Useful flags: `--interval SECONDS` (default 5, matching the SCD41's own
   update cadence), `--once` (render a single frame and exit, handy for
   testing), `--fb /dev/fbN`, `--iio-path /sys/bus/iio/devices/iio:deviceN`
   (only needed if auto-detection by driver name picks the wrong device).

3. To start it automatically at boot, install the init script:

   ```
   cp buildroot/S99aerolith /etc/init.d/S99aerolith
   chmod +x /etc/init.d/S99aerolith
   /etc/init.d/S99aerolith start
   ```

   To bake this into the image permanently instead, add both
   `aerolith/` (under `/usr/local/lib/aerolith`) and `S99aerolith` to
   `buildroot_external/board/sipeed/m1s/rootfs-overlay/` in
   `buildroot_custom` (the same overlay that already ships
   `S01growfs` etc.), so `BR2_ROOTFS_OVERLAY` picks them up on the next
   image build.

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

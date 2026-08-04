# Reflow oven firmware (Zephyr, ESP32)

Modular firmware for a reflow oven: MAX6675 thermocouple front-end, SSR heater
driven by slow PWM, PID + profile state machine, ST7789 320x240 local UI with a
rotary encoder, and a Wi-Fi web UI. Every feature except the control core can be
switched off with a single Kconfig symbol.

## Architecture

```
                  +---------------------------+
                  |      control core         |   always built
   MAX6675 ---->  |  temp -> PID -> profile   |
   (SPI)          |         -> heater (SSR)   |
                  +------------+--------------+
                               |  publishes reflow_telemetry_chan (zbus)
        +----------------------+----------------------+-------------+
        |                      |                      |             |
   display UI             web UI (HTTP/SSE)       shell cmds     (your module)
   ST7789 + LVGL          Wi-Fi station           UART console
        |                      |                      |             |
        +----------------------+----------------------+-------------+
                               |  reflow_cmd_post()
                        back into the core
```

The core is the only writer of the telemetry channel and the only consumer of
commands. Feature modules never call each other and the core never references
them: they are self-registering threads/listeners. Adding a feature means
adding a file, a Kconfig symbol and one line in `CMakeLists.txt`; removing one
means flipping the symbol to `n`.

| Path | Role |
| --- | --- |
| `src/core/pid.c` | PID, derivative on measurement, conditional anti-windup. Pure C99 |
| `src/core/profile.c` | Profile table + stage state machine. Pure C99 |
| `src/core/temp.c` | MAX6675 read, plausibility window, spike rejection |
| `src/core/heater.c` | Slow-PWM SSR output, minimum pulse, stale-request cut-off |
| `src/core/controller.c` | The control thread; owns state, faults, telemetry |
| `src/ui/display_ui.c` | LVGL screen (`CONFIG_REFLOW_UI_DISPLAY`) |
| `src/ui/input_ui.c` | Encoder + button via the input subsystem (`CONFIG_REFLOW_UI_INPUT`) |
| `src/net/wifi.c`, `src/net/httpd.c` | Wi-Fi station + web UI (`CONFIG_REFLOW_NET`) |
| `src/shell_cmds.c` | `reflow status\|start\|stop\|clear\|profile` (`CONFIG_REFLOW_SHELL`) |
| `tests/logic/` | ztest suite for the PID and the profile machine |
| `tools/host_sim.c` | Closed-loop simulation of the same logic on the build host |

The two pure-C99 files carry all the logic that decides whether a board gets
soldered or cooked, which is why they have no Zephyr dependency: they are
testable on the host in milliseconds.

## Targets and wiring

Nothing in the C code hardcodes a pin: every target is an overlay plus a Kconfig
fragment in `boards/`.

### Raspberry Pi Pico 2 — `rpi_pico2/rp2350a/m33` (bring-up target)

Control core plus shell only: no panel, no networking. Fewest variables for a
first run on real hardware.

| Signal | Pin | Notes |
| --- | --- | --- |
| SPI0 SCK / MOSI / MISO | GP18 / GP19 / GP16 | board default pinctrl, untouched |
| MAX6675 CS | GP20 | 1 MHz, read every 250 ms |
| SSR gate | GP15 | active high, 3V3 logic input |
| Console + log + shell | USB | CDC ACM, same cable as power |

GP17 is deliberately unused: the board pinctrl muxes it as the SPI0 hardware
CSn, and the MAX6675 needs chip select held low for the whole 16-bit frame, so
a plain GPIO does the job instead.

```sh
west build -b rpi_pico2/rp2350a/m33 .
west flash            # or copy build/zephyr/zephyr.uf2 with BOOTSEL held
```

Then open the USB serial port (`/dev/ttyACM0`, or the new COM port on Windows)
and use `reflow status`, `reflow profile`, `reflow start`, `reflow stop`. The
port only appears after the firmware boots, and log lines emitted before the
host enumerates the device are lost — that is normal for a USB console.

Adding the panel and encoder later, without touching the board files:

```sh
west build -b rpi_pico2/rp2350a/m33 . -- \
  -DEXTRA_DTC_OVERLAY_FILE=overlays/rpi_pico2_display_encoder.overlay \
  -DCONFIG_REFLOW_UI_DISPLAY=y -DCONFIG_REFLOW_UI_INPUT=y
```

The RP2350 has no radio, so `CONFIG_REFLOW_NET` stays off on this target. The
web UI itself is link agnostic (it is plain BSD sockets); giving it a USB
Ethernet link with the CDC ECM class is a small module away, with the caveat
that CDC ECM has no native Windows driver.

### ESP32-DevKitC — `esp32_devkitc_wroom/esp32/procpu` (full featured)

| Signal | Pin | Notes |
| --- | --- | --- |
| SPI2 SCLK / MISO / MOSI | GPIO14 / 12 / 13 | HSPI defaults |
| MAX6675 CS | GPIO15 | 2 MHz, read every 250 ms |
| ST7789 CS / DC / RST / BL | GPIO5 / 21 / 27 / 22 | 20 MHz |
| SSR gate | GPIO4 | active high, 3V3 logic input |
| Encoder A / B | GPIO32 / GPIO33 | `gpio-qdec`, internal pull-ups |
| Encoder button | GPIO25 | to GND, `gpio-keys` |

```sh
west blobs fetch hal_espressif        # once, needed for Wi-Fi
west build -b esp32_devkitc_wroom/esp32/procpu .   # older trees: esp32_devkitc/...
west flash && west espressif monitor
```

Set the credentials before flashing (`prj.conf` or `west build -t menuconfig`):

```
CONFIG_REFLOW_WIFI_SSID="my-network"
CONFIG_REFLOW_WIFI_PSK="my-passphrase"
```

## Removing or replacing a module

Each optional module is a leaf: one source file, one Kconfig symbol, one line in
`CMakeLists.txt`. It reaches the core only through the telemetry channel and
`reflow_cmd_post()`, and the core has no reference to it in either direction.

```sh
# no Wi-Fi: drops wifi.c, httpd.c, the TCP/IP stack and the ESP32 Wi-Fi driver
west build -b esp32_devkitc_wroom/esp32/procpu . -- -DCONFIG_REFLOW_NET=n

# console-only bring-up: no display, no LVGL, no networking
west build -b esp32_devkitc_wroom/esp32/procpu . -- \
  -DCONFIG_REFLOW_UI_DISPLAY=n -DCONFIG_REFLOW_NET=n

# standalone oven: display and encoder only, no console, no network
west build -b esp32_devkitc_wroom/esp32/procpu . -- \
  -DCONFIG_REFLOW_NET=n -DCONFIG_REFLOW_SHELL=n
```

Use `west build -t menuconfig` to see the same switches under
*Reflow oven -> Optional features*, and `west build -t ram_report` /
`rom_report` to check what each one costs.

`CONFIG_REFLOW_UI_DISPLAY=n` drops `src/ui/display_ui.c` from the build, so its
zbus observer stops existing and the core publishes to one subscriber less. The
subsystems the feature needed go with it: `DISPLAY` and `LVGL` are `select`ed by
the feature symbol and the LVGL tuning is applied by `configdefault ... if
REFLOW_UI_DISPLAY` at the bottom of `Kconfig`, never by `prj.conf`. The `st7789`
node stays in the overlay and is simply not used; with `DISPLAY=n` the panel
driver is not compiled either.

`CONFIG_REFLOW_NET=n` works the same way: `wifi.c` and `httpd.c` leave the
build, and with them `NETWORKING`, `NET_TCP`, `NET_SOCKETS`, DHCP, the packet
pools, the extra heap and `WIFI_ESP32`. The Wi-Fi credentials are deliberately
declared outside `if REFLOW_NET`, so leaving them in `prj.conf` does not produce
Kconfig warnings in a build without networking. `boards/*.conf` holds no
networking symbols at all, for the same reason.

To replace a module - a monochrome OLED instead of LVGL, an MQTT publisher
instead of the web UI - write the new file with the same three lines of
ceremony:

```c
#include "../core/app.h"

ZBUS_SUBSCRIBER_DEFINE(my_sub, 4);                          /* or ZBUS_LISTENER_DEFINE */
ZBUS_CHAN_ADD_OBS(reflow_telemetry_chan, my_sub, 3);
/* ... and reflow_cmd_post() if the module also commands the oven */
```

then add a `config REFLOW_MY_FEATURE` symbol and one
`target_sources_ifdef(CONFIG_REFLOW_MY_FEATURE app PRIVATE src/...)`. Nothing in
`src/core/` changes. The only place that knows the feature list is the banner
`main.c` prints at boot, which is cosmetic.

## Tests

```sh
west twister -T tests -p native_sim          # 12 unit tests, PID + profile
```

Closed-loop check with a first-order oven model, no hardware and no Zephyr:

```sh
cc -std=c99 -Wall -Wextra -O2 -o host_sim tools/host_sim.c \
   src/core/pid.c src/core/profile.c -Isrc/core -lm
./host_sim -v
KP=80 KI=5 KHEAT=3.0 TAU=8 ./host_sim     # gain and oven sweeps
```

With the default gains (Kp 60, Ki 2, Kd 250 permille/degC) and an oven that
manages 4.5 degC/s at full power, all three profiles track within ~4 degC RMS
and peak at 250 degC against a 245 degC target. Re-run the sweep for your oven
and move the winning values into `Kconfig`.

## Operating it

- Console: `reflow profile` lists profiles, `reflow profile 1` selects, `reflow
  start`, `reflow stop`, `reflow status`, `reflow clear` after a fault.
- Encoder: turn to pick a profile while idle, short press starts/stops, long
  press (1 s) clears a fault.
- Web UI: `http://<oven-ip>/` - live temperature/setpoint plot fed by
  server-sent events, profile selector, start/stop/clear.

Profiles built in: SAC305 lead-free, Sn63Pb37 leaded, and a 120 degC bake. They
live in `src/core/profile.c` as plain tables.

## Safety

The firmware fails safe in these cases, always by cutting the SSR and latching
a fault that needs an explicit clear:

- thermocouple open, SPI error, reading outside -20..400 degC (`FAULT_SENSOR`);
- temperature above `CONFIG_REFLOW_ABS_MAX_TEMP_C` (270 degC default) or above
  the running profile's own abort level (`FAULT_OVERTEMP`);
- a stage that overruns its nominal time by more than the grace period, i.e. the
  oven cannot follow the profile (`FAULT_TIMEOUT`);
- the control loop stopping: the heater output disarms itself if no duty request
  arrives within `CONFIG_REFLOW_HEATER_STALE_MS`.

None of this replaces hardware protection. A mains oven needs a thermal fuse in
series with the element, and the SSR sized and heatsinked for the load. Also
note that this firmware has no hardware watchdog enabled and no persistent
storage: enable `CONFIG_WDT_*` and move the Wi-Fi credentials to settings/NVS
before it runs unattended.

## Status / what is verified

- Verified: PID and profile logic, by 12 unit tests and by the closed-loop host
  simulation above (both were run and pass).
- Not verified here: compilation and behaviour on the real board. The devicetree
  overlay, the ST7789 panel parameters and the ESP32 Wi-Fi Kconfig are the
  parts most likely to need adjustment for your Zephyr version and panel batch.
  Bring it up in this order: `CONFIG_REFLOW_SHELL` only, then the display, then
  the network.

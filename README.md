# Reflow oven firmware (Zephyr, RP2350 and ESP32)

Modular firmware for a reflow oven: MAX6675 thermocouple front-end, SSR heater
driven by slow PWM, PID + profile state machine, ST7789 320x240 local UI with a
rotary encoder, a console shell, and a web UI reachable over Wi-Fi or over the
USB cable. Every feature except the control core can be switched off with a
single Kconfig symbol.

Targets: Raspberry Pi Pico 2 (RP2350) and ESP32-DevKitC.

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
   ST7789 + LVGL          on any link:            UART console
        |                  Wi-Fi or USB ECM           |             |
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
| `src/net/httpd.c` | HTTP server and server-sent events, on plain BSD sockets (`CONFIG_REFLOW_NET`) |
| `src/net/l4.c` | Link-agnostic "network is usable" gate that the server waits on |
| `src/net/wifi.c` | Link: Wi-Fi station (`CONFIG_REFLOW_LINK_WIFI`) |
| `src/net/usb_net.c` | Link: USB Ethernet, CDC ECM plus a one-address DHCP server (`CONFIG_REFLOW_LINK_USB_ECM`) |
| `src/telemetry_json.c` | The one telemetry-to-JSON formatter, shared by the server, the shell and host tools |
| `src/net/index.html`, `index_html.h` | The page, in both transports; regenerate the header with `tools/gen_page.py` |
| `src/shell_cmds.c` | `reflow status\|start\|stop\|clear\|profile` (`CONFIG_REFLOW_SHELL`) |
| `snippets/usb-webui/` | `-S usb-webui`: the overlay and conf for the USB link |
| `tests/logic/` | ztest suite for the PID and the profile machine |
| `tests/boot/` | ztest suite for the SSR gate's state at boot, on the GPIO emulator |
| `tools/host_sim.c` | Closed-loop simulation of the same logic on the build host |
| `tools/test_page.js` | Optional: `node tools/test_page.js` smoke tests the page's transport choice and rendering |

The web UI is split in two on purpose: `httpd.c` only knows sockets, so the link
underneath it is a Kconfig choice. That is what made the USB Ethernet link an
added file rather than a rewrite.

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
west build -p always -b rpi_pico2/rp2350a/m33 reflow_oven
west flash            # or copy build/zephyr/zephyr.uf2 with BOOTSEL held
```

Then open the USB serial port (`/dev/ttyACM0`, or the new COM port on Windows)
and use `reflow status`, `reflow profile`, `reflow start`, `reflow stop`. The
port only appears after the firmware boots, and log lines emitted before the
host enumerates the device are lost — that is normal for a USB console.

Adding the panel and encoder later, without touching the board files:

```sh
west build -p always -b rpi_pico2/rp2350a/m33 reflow_oven -- \
  -DEXTRA_DTC_OVERLAY_FILE=overlays/rpi_pico2_display_encoder.overlay \
  -DCONFIG_REFLOW_UI_DISPLAY=y -DCONFIG_REFLOW_UI_INPUT=y
```

#### Web UI over the USB cable (no radio)

The RP2350 has no radio, but the web UI does not need one: `httpd.c` talks plain
BSD sockets, so the link is a separate, swappable module. `CONFIG_REFLOW_NET`
enables the server and `choice REFLOW_LINK` picks how it is reached -
`REFLOW_LINK_WIFI` or `REFLOW_LINK_USB_ECM`.

```sh
west build -p always -b rpi_pico2/rp2350a/m33 reflow_oven -S usb-webui
```

`-S usb-webui` is a [snippet](https://docs.zephyrproject.org/latest/build/snippets/index.html):
`snippets/usb-webui/snippet.yml` pulls in the devicetree overlay and the conf
fragment for you. The equivalent long form still works, but note that a shell
that splits the argument at the dot will silently drop the file extension, so
quote it:

```sh
west build -p always -b rpi_pico2/rp2350a/m33 reflow_oven -- \
  "-DEXTRA_DTC_OVERLAY_FILE=overlays/rpi_pico2_usb_webui.overlay" \
  "-DEXTRA_CONF_FILE=overlays/rpi_pico2_usb_webui.conf"
```

Snippet discovery needs one line of help: `SNIPPET_ROOT` contains only the
zephyr repository by default, so `CMakeLists.txt` appends this application's
directory to it before `find_package(Zephyr)`. If you ever see
`snippets not found: usb-webui`, that line is missing or the build was
configured from a stale cache — rebuild with `-p always`.

The oven becomes a composite USB device: the CDC ACM function keeps the shell,
and a CDC ECM function provides Ethernet. The oven holds `192.168.7.1/24` and
runs a one-address DHCPv4 server, so the host configures itself; open
`http://192.168.7.1/`. All of it is on the cable that already powers the board.

Host support is the catch, and it is a host problem, not a firmware one:

| Host | CDC ECM |
| --- | --- |
| Linux | in-box (`cdc_ether`), appears as a normal interface |
| macOS | in-box |
| Windows | **no in-box driver**; needs a third party one |

Zephyr's USB device stack implements ECM only - not RNDIS or NCM, which are the
classes Windows binds natively. On Windows, use the Web Serial route in the next
section instead: same page, no driver, no network stack.

Two consequences of `usb_net.c` owning the USB stack, both handled by the conf
fragment above: `CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` must be off (a controller
can hold several device contexts but only one may be enabled), and the VID/PID
default to Zephyr's development pair - replace them before distributing
hardware.

#### Web UI from a local page, on any operating system (Web Serial)

This one needs **no network stack in the firmware and no driver on the host**: it
works on the plain bring-up build, over the CDC ACM serial port every OS already
binds. The page opens the port itself with the
[Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API).

1. Build and flash the normal target - nothing extra, no snippet:
   `west build -p always -b rpi_pico2/rp2350a/m33 reflow_oven`
2. Close any terminal holding the COM port. Only one program can own it.
3. Open `src/net/index.html` in Chrome or Edge. Double-clicking the file works;
   if the browser refuses the port, serve it instead and use the localhost URL:
   `cd src\net && python -m http.server 8000` then `http://localhost:8000`.
4. Click **Conectar por USB** and pick the oven in the port chooser. The chooser
   is filtered by USB vendor ID, so the oven is usually the only entry.

It is the same `index.html` the firmware serves - one file, two transports. The
page decides by where it was loaded from: an `http://` host that is not localhost
means the oven is serving it, so it uses server-sent events; anything else means
it is a local page, so it offers the USB button. Over the serial port it polls
`reflow json` once a second, sends `reflow start` and friends for commands, and
picks the reply out of the shell's echo, prompt and colour codes by taking the
first line that starts with `{`.

Limits: Chromium desktop browsers only (Chrome, Edge, Opera) - not Firefox, not
Safari, and **not Chrome on Android**, so this does not give you phone access.
Web Serial also requires a secure context, which is why `file://` or `localhost`
are the two options above.

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
west build -p always -b esp32_devkitc_wroom/esp32/procpu reflow_oven
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
# no network: drops httpd.c, l4.c, the link module, the TCP/IP stack and the radio driver
west build -b esp32_devkitc_wroom/esp32/procpu reflow_oven -- -DCONFIG_REFLOW_NET=n

# console-only bring-up: no display, no LVGL, no networking
west build -b esp32_devkitc_wroom/esp32/procpu reflow_oven -- \
  -DCONFIG_REFLOW_UI_DISPLAY=n -DCONFIG_REFLOW_NET=n

# standalone oven: display and encoder only, no console, no network
west build -b esp32_devkitc_wroom/esp32/procpu reflow_oven -- \
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

## Build-time options

Anything below can go in `prj.conf`, in a conf fragment, or on the command line
after `--`. Command line assignments land in `extra_kconfig_options.conf`, which
is merged last, so they beat both `prj.conf` and `boards/<board>.conf`. Use
`west build -t menuconfig` to browse the same options under *Reflow oven*.

```sh
west build -p always -b <target> reflow_oven -- -DCONFIG_REFLOW_UI_DISPLAY=n
```

String options are awkward to quote on a Windows shell (`-DCONFIG_X=\"text\"`);
prefer putting those in `prj.conf` or a fragment.

### Features

| Option | Default | Effect |
| --- | --- | --- |
| `CONFIG_REFLOW_SHELL` | `y` | `reflow` commands on the console |
| `CONFIG_REFLOW_UI_DISPLAY` | board | LVGL screen on the `zephyr,display` panel |
| `CONFIG_REFLOW_UI_DISPLAY_STACK_SIZE` | 4096 | display thread stack |
| `CONFIG_REFLOW_UI_INPUT` | board | rotary encoder and button via the input subsystem |
| `CONFIG_REFLOW_NET` | board | web UI (HTTP + server-sent events) |
| `CONFIG_REFLOW_LINK_WIFI` | `y` when `REFLOW_NET` | link: Wi-Fi station |
| `CONFIG_REFLOW_LINK_USB_ECM` | - | link: USB Ethernet, needs the ECM devicetree node |

The two link options are a `choice`: setting one clears the other.

### Control core

| Option | Default | Range | Meaning |
| --- | --- | --- | --- |
| `CONFIG_REFLOW_CTRL_PERIOD_MS` | 100 | 10-1000 | control thread tick |
| `CONFIG_REFLOW_SAMPLE_PERIOD_MS` | 250 | 100-2000 | thermocouple sampling; a MAX6675 conversion needs ~220 ms |
| `CONFIG_REFLOW_PUBLISH_PERIOD_MS` | 500 | 100-5000 | telemetry publish rate |
| `CONFIG_REFLOW_ABS_MAX_TEMP_C` | 270 | 60-400 | firmware over-temperature cut-out |
| `CONFIG_REFLOW_CTRL_STACK_SIZE` | 2048 | - | control thread stack |
| `CONFIG_REFLOW_CTRL_PRIORITY` | 5 | - | control thread priority; keep it above the UI and network threads |

### Heater output

| Option | Default | Range | Meaning |
| --- | --- | --- | --- |
| `CONFIG_REFLOW_HEATER_WINDOW_MS` | 1000 | 100-10000 | slow PWM window |
| `CONFIG_REFLOW_HEATER_MIN_PULSE_MS` | 50 | 0-1000 | shorter pulses snap to 0 % or 100 % |
| `CONFIG_REFLOW_HEATER_STALE_MS` | 2000 | 200-10000 | output disarms if no duty request arrives |

### PID gains

Milli-units, so 60000 means 60.0. Sweep them with `tools/host_sim.c` before
touching them here.

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_REFLOW_PID_KP_MILLI` | 60000 | permille of duty per degC of error |
| `CONFIG_REFLOW_PID_KI_MILLI` | 2000 | integral gain |
| `CONFIG_REFLOW_PID_KD_MILLI` | 250000 | derivative gain (acts on the measurement) |
| `CONFIG_REFLOW_PID_I_CLAMP` | 1000 | integral clamp, permille |
| `CONFIG_REFLOW_PID_D_ALPHA_MILLI` | 200 | derivative low-pass; 1000 disables filtering |

### Web UI

| Option | Default | Range | Meaning |
| --- | --- | --- | --- |
| `CONFIG_REFLOW_NET_HTTP_PORT` | 80 | - | listening port |
| `CONFIG_REFLOW_NET_MAX_CLIENTS` | 4 | 1-8 | simultaneous connections |
| `CONFIG_REFLOW_NET_PUSH_PERIOD_MS` | 1000 | 200-10000 | telemetry push interval |
| `CONFIG_REFLOW_NET_STACK_SIZE` | 4096 | - | HTTP thread stack |
| `CONFIG_REFLOW_WIFI_SSID` | `"changeme"` | - | Wi-Fi link only |
| `CONFIG_REFLOW_WIFI_PSK` | `""` | - | empty means an open network |

### USB Ethernet link

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_REFLOW_USB_IPV4_ADDR` | `"192.168.7.1"` | the oven's address; this is what you browse to |
| `CONFIG_REFLOW_USB_IPV4_MASK` | `"255.255.255.0"` | netmask of the USB link |
| `CONFIG_REFLOW_USB_DHCP_POOL_START` | `"192.168.7.2"` | first address handed to the host |
| `CONFIG_REFLOW_USB_VID` | `0x2fe3` | Zephyr's development VID - replace before distributing |
| `CONFIG_REFLOW_USB_PID` | `0x0007` | likewise |
| `CONFIG_REFLOW_USB_MANUFACTURER` | `"Reflow oven project"` | USB string descriptor |
| `CONFIG_REFLOW_USB_PRODUCT` | `"Reflow oven"` | USB string descriptor |

### Logging

`CONFIG_REFLOW_LOG_LEVEL_{OFF,ERR,WRN,INF,DBG}` sets the level for every module
in this application (`reflow_ctrl`, `reflow_temp`, `reflow_heater`, ...).
`prj.conf` uses `INF`; `DBG` adds the HTTP request lines.

### Zephyr options this application already handles

These are set for you, as `select` or as `configdefault ... if <feature>` at the
bottom of `Kconfig`, so you normally do not touch them. They are listed because
they are the ones that break things when a build goes wrong:

`DISPLAY`, `LVGL`, `LV_Z_MEM_POOL_SIZE`, `LV_Z_VDB_SIZE`, `LV_FONT_MONTSERRAT_28`,
`LV_USE_LOG`, `INPUT`, `INPUT_GPIO_QDEC`, `INPUT_GPIO_KEYS`, `SHELL`,
`SHELL_STACK_SIZE`, `NETWORKING`, `NET_IPV4`, `NET_TCP`, `NET_SOCKETS`,
`NET_MGMT`, `NET_MGMT_EVENT`, `NET_CONNECTION_MANAGER`, `NET_L2_ETHERNET`,
`NET_DHCPV4`, `NET_DHCPV4_SERVER`, `ZVFS_POLL_MAX`, `NET_MAX_CONTEXTS`,
`NET_MAX_CONN`, `NET_PKT_{RX,TX}_COUNT`, `NET_BUF_{RX,TX}_COUNT`,
`HEAP_MEM_POOL_SIZE`, `WIFI`, `WIFI_ESP32`, `USB_DEVICE_STACK_NEXT`, `HWINFO`.

Two Zephyr options you may still need by hand:

| Option | When |
| --- | --- |
| `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` | `y` for a USB console without the USB Ethernet link; must be `n` with `REFLOW_LINK_USB_ECM`, which owns the USB stack |
| `CONFIG_SENSOR_LOG_LEVEL_WRN` | already in `prj.conf`; the MAX6675 driver logs at INFO on every failed read |

## Tests

```sh
west twister -T tests -p native_sim          # unit tests: PID, profile, and the boot state of the SSR gate
```

On Windows `native_sim` is filtered out ("Native platform requires Linux"); both
suites also run on `qemu_x86`, which is what the process prescribes locally:

```sh
west twister -T tests -p qemu_x86 --timeout-multiplier 6
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
- `reflow json` prints the same state as one line of JSON - that is what the
  local page and any bench script read, instead of parsing human text.
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

### The gate needs a pull-down, in hardware

**Fit a 10k resistor from the SSR gate pin to ground.** This is a requirement,
not a suggestion, and no firmware change can replace it.

The firmware drives the gate low from a `SYS_INIT` hook in `heater.c`, at
`POST_KERNEL`, which is the earliest level where the GPIO controller exists on
these targets — before that there is no device to configure. So the window from
power-on reset until the GPIO driver comes up is covered by nothing but the
SoC's own reset state, and on both targets that state is an input: high
impedance. Opto-coupled SSR modules commonly hold a pull-up on their input and
read high impedance as ON, so a board in a reset loop (brownout, or a watchdog
that keeps firing) can heat continuously with no control loop running at all.
An external pull-down is what makes that window safe.

Before RFO-B06 the window was far worse than a boot: nothing claimed the pin
until `reflow_heater_init()` ran from the control thread, which
`K_THREAD_DEFINE` starts 100 ms after the kernel.

None of this replaces hardware protection. A mains oven needs a thermal fuse in
series with the element, and the SSR sized and heatsinked for the load. Also
note that this firmware has no hardware watchdog enabled and no persistent
storage: enable `CONFIG_WDT_*` and move the Wi-Fi credentials to settings/NVS
before it runs unattended.

## Status / what is verified

Toolchain used so far: Zephyr 4.4.99, Zephyr SDK 1.0.1, west 1.5.0.

| Level | What |
| --- | --- |
| Logic verified by tests | PID and profile state machine: 12 ztest cases plus the closed-loop host simulation above |
| Builds and flashes | `rpi_pico2/rp2350a/m33`, minimal target (core + shell over USB CDC ACM) |
| Runs on hardware | thermocouple read path: live readings, open-circuit detection, latched fault. Bench validation of the heater output still pending |
| Verified without hardware | the page, by `node tools/test_page.js`: transport selection, rendering, and picking JSON out of shell noise |
| Written, never compiled | display UI, encoder, Wi-Fi link, HTTP server, USB Ethernet link, and the whole ESP32 target |

Bring-up order that this repo is set up for: `CONFIG_REFLOW_SHELL` only, then the
display, then the network. The devicetree overlays, the ST7789 panel parameters
and the ESP32 Wi-Fi Kconfig are the parts most likely to need adjustment for
your Zephyr version and your panel batch.

Bench checks to do before mains power, with an LED on the SSR pin:

1. No thermocouple attached: `reflow status` must report `fault: sensor`. A
   plausible temperature with no thermocouple means the chip select is wrong.
2. Thermocouple attached, `reflow clear`: `reflow status` must track room
   temperature, and rise when you warm the junction.
3. `reflow profile 2` (120 degC bake), `reflow start`: the LED must show the 1 s
   window modulation, and `reflow status` must show a stage advancing.
4. Pull the thermocouple mid-run: the output must cut immediately and latch
   `fault: sensor` until `reflow clear`. If this one fails, do not wire mains.

### Planned next

**A host bridge for the browsers Web Serial leaves out.** With `reflow json` in
place, a single-file Python script (pyserial) serving `index.html` on localhost
and relaying to the serial port is roughly 60 lines. It would cover Firefox,
Safari and phones on the local network, at the cost of needing Python on the
host. The firmware needs nothing more.

**Custom profiles with persistence.** Profiles are compile-time tables today.
Three steps, in this order: make the profile storable (the `const char *` names
have to become fixed-size arrays, plus a RAM layer over the built-ins and a
`reflow_profile_validate()` that a stored profile must pass — that one is a
safety requirement, not a nicety); then persist with settings over NVS, minding
that the RP2350 executes by XIP from the same flash it would be writing, and
that a stored blob needs magic, version and CRC so a firmware update cannot read
old bytes as a valid profile; then the editor, in the shell and in the page.
`reflow profile reset` back to the built-ins is not optional — without it one bad
stored profile bricks the oven until you reflash.

**A dedicated CDC ACM port for machine data.** Today the page shares the shell
port, which works but means closing your terminal to use the page, and the page
has to filter shell noise. A second `zephyr,cdc-acm-uart` instance would give a
clean line-oriented channel and let the firmware push telemetry instead of being
polled. It costs three USB endpoints and manual USB stack init - the block
already written in `usb_net.c`, which would move to a shared file.

### Known Zephyr version sensitivities

All found against Zephyr 4.4:

- `NET_SOCKETS_POLL_MAX` no longer exists; the sockets `poll()` sizing moved to
  `ZVFS_POLL_MAX`.
- `K_MSGQ_DEFINE` must not be prefixed with `static` (it declares its own buffer
  `static`, so you get a duplicate specifier).
- `net_mgmt_event_handler_t` takes the event mask as `uint64_t`. Declaring the
  handler with `uint32_t` truncates the `NET_EVENT_*` constants, so it does not
  merely warn — no case would ever match.
- `DT_NODE_HAS_STATUS(node, okay)` is spelled `DT_NODE_HAS_STATUS_OKAY(node)`.

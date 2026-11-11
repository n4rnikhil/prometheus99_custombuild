# ESP32-S3 port

The same HMI runtime as on Windows and Linux, on an ESP32-S3 board with an 800×480 RGB
panel and a GT911 touch controller (for example Waveshare ESP32-S3-Touch-LCD-4.3/7, Sunton
ESP32-8048S043/S070, Elecrow 5"/7" HMI boards).

**Status:** builds with ESP-IDF 5.3.2 (warnings are errors) and runs in Espressif's emulator
(QEMU): it boots, mounts its storage, joins the emulated network, finds two simulated devices and
polls them. It has **not been run on a board**: the display, touch, backlight power-up, Wi-Fi and
PSRAM are unverified. The portable core is tested with the same MCU profile (`build.bat test`,
`make test`).

## Build

```sh
cd ports/esp32
idf.py set-target esp32s3
idf.py menuconfig        # Prometheus HMI -> Wi-Fi SSID / password, runtime options
idf.py build flash monitor
```

Before the first build, check [main/board.h](main/board.h) against your board's schematic:
the defaults are the Waveshare 4.3" pin map and panel timing.

- **Backlight and resets.** Waveshare boards switch them through a CH422G I/O expander; the
  firmware sets it up (as Waveshare's examples do). Other boards (Sunton, Elecrow, ...): set
  `BOARD_IO_EXPANDER_CH422G` to 0 and `BOARD_PIN_BACKLIGHT` to the backlight pin. Anything else a
  board needs goes in `board_init()` in [main/main_esp32.c](main/main_esp32.c).
- **If the display does not start** (no PSRAM, wrong pins or timing), the firmware keeps running
  without it - polling, alarms, the horn output and the audit log continue - and the serial log
  (`idf.py monitor`) says why.
- The device key, PIN vault and alarm log live on the `storage` partition (`/data`).

## Run it in the emulator

Espressif's QEMU has no PSRAM, display or Wi-Fi, so this build uses its emulated Ethernet and runs
without the display. It finds devices on the PC at 10.0.2.2 (ports 1502-1503 in
[sdkconfig.qemu](sdkconfig.qemu)), prints its report after 60 s, and stops:

```sh
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa
idf.py -B build_qemu -D SDKCONFIG=build_qemu/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" qemu
```

## How it maps onto the ESP32

| Runtime part | On the ESP32-S3 |
|---|---|
| UI loop (frames, touch, rendering) | `app_main` task, core 0 |
| Fieldbus loop | pthread at FreeRTOS priority 10, pinned to core 1, 500 Hz (`CONFIG_FREERTOS_HZ=1000`) |
| Discovery | pthread, priority 2; 6 parallel probes (lwIP socket table) |
| Sockets, threads, clock | the same `src/hal/*_posix.c` files as Linux (lwIP, esp_pthread, FreeRTOS tick sleeps) |
| Screen | 4-bpp bands → RGB565, 8 rows at a time (12.5 KB) → PSRAM frame buffer |
| Alarm log, PIN vault | LittleFS partition `storage` mounted at `/data` (`--log /data/alarms.log`) |
| Alarm time stamps | SNTP (`pool.ntp.org` by default) |
| Static RAM of the runtime | 50.8 KB (MCU profile, measured on the x86-64 build) + 8 KB / 6 KB thread stacks |

Options are the desktop command line, set once in menuconfig, for example:
`--plc 192.168.1.10 --scan 192.168.1.0/24 --log /data/alarms.log --vault /data/security.vault`.

## Limits

* The dashboard layout is designed for 640×400 and up. Small SPI panels (480×320,
  320×240) need a compact layout first.
* Wi-Fi only on a board (the emulator build uses QEMU's Ethernet); an Ethernet board (W5500,
  LAN8720) needs its `esp_eth` set-up in `network_init()`.
* No buzzer: `hal_beep()` is empty; wire a GPIO there if the board has one.

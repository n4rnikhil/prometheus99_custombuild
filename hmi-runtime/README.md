# HMI runtime (`industrial_hmi`)

A lightweight operator panel in portable C99. It finds PLCs, drives and meters on Modbus TCP,
puts each one on the dashboard with a screen for its machine, and lets an operator control it.
The code has no heap allocation, is `-Werror` clean, and uses no mutexes between the UI and the
fieldbus threads.

See the [project README](../README.md) for the overview and the [User Manual](../docs/USER_MANUAL.md) for operation.

## Build

| Platform | Command | Output |
|---|---|---|
| Windows (MinGW-w64 gcc) | `build.bat` | `industrial_hmi.exe` |
| Windows tests | `build.bat test` | runs every test suite listed below |
| Windows snapshot tool | `build.bat snapshot` | BMP image of each screen |
| Linux / Raspberry Pi | `make` (cross: `make CC=aarch64-linux-gnu-gcc` or `CC=arm-linux-gnueabihf-gcc`) | `industrial_hmi` |
| Linux tests | `make test` | every suite below, under AddressSanitizer + UBSan (`make test-fast`: unit tests only) |
| ESP32 | ESP-IDF project in `ports/esp32/` | not yet compiled |

The Windows integration test links the PLC simulator's device models (`../plc-simulator/src/plc_models.c`).

## Run

```
industrial_hmi.exe [options]
```

Options can also be written in `hmi.cfg` next to the executable (or `--config <file>`), one or more
per line, `#` for comments. The command line adds to the file. Examples are in
[examples/](examples/): `bench.cfg` for the simulator, `lab-site.cfg` for a real network.

| Option | Meaning |
|---|---|
| `--plc <ip[:port][/uN][@module]>` | a device to attach even if it does not identify itself. `@module` forces a driver module (for example `192.168.1.30/u1@altivar`) |
| `--scan <spec>` | a discovery range: `192.168.1.0/24`, `10.0.0.5:502-510`, `/uN` for a unit id. Only devices that identify as a known PLC or drive are attached |
| `--no-discovery` | only the `--plc` devices, no scanning |
| `--config <file>` / `--no-config` | read options from a file instead of `hmi.cfg` / read none |
| `--remove-after-ms <n>` | offline devices leave the dashboard after n ms (0 = never) |
| `--log <path>`, `--no-log` | alarm and audit log file (default `alarms.log` next to the executable) |
| `--verify-log <path>` | check the log's signature chain with this installation's key and exit (0 = verified) |
| `--set-pin <role>` | set a role's site PIN (6-8 digits, not trivial; typed twice, never on the command line) and exit |
| `--key <path>`, `--vault <path>` | device key (default `hmi.key`) and PIN vault (default `security.vault`) |
| `--allow-factory-pins` | **bench only**: roles may sign in with the factory PINs |
| `--color-scheme status\|high-performance` | normal and running states in green (default, the IEC 60073 indicator convention) or in grey (strict ISA-101). Alarms and stop controls are the same in both |
| `--headless` | Linux: no display (Modbus, alarms and log only) |
| `--window`, `--fb <device>` | Linux: a desktop window (default when a desktop session is running; X11, or XWayland on Wayland desktops) / force the framebuffer. libX11 is loaded only when a window is opened, so the same binary runs on machines without it |
| `--horn-gpio <chip>:<line>` | Linux: drive a GPIO line high while unacknowledged alarms exist (buzzer, stack light), e.g. `gpiochip0:17`. The HMI refuses to start if the line cannot be driven |
| `--horn-fail-safe` | Linux: invert that line (high = healthy, low = alarm), so a dead panel or a cut wire also sounds a relay-driven horn |
| `--no-sound` | no alarm tone from the PC's speakers (a quiet bench). A wired horn is not affected |

**Alarm sound.** While unacknowledged alarms exist and the horn is not silenced, the panel plays
its own two-tone signal (960 / 760 Hz, 0.3 s each, looped) on the default audio output: Windows
through the sound mixer, Linux through ALSA (PulseAudio / PipeWire on desktops; `HMI_SOUND_DEVICE`
picks another device, e.g. `plughw:1,0`). It does not depend on the desktop's sound scheme. With
no audio device the HMI says so once and keeps trying (a headset plugged in later works).
Acknowledge or **Silence horn** stops it.

**One HMI per data folder.** A second start with the same key, log and vault is refused (exit code
4): two would break the signed log chain and fight over the PLCs. On Windows the running window
comes forward instead; the PLC simulator does the same.

## Operator interface design

The screens are designed for a worker under stress, possibly gloved, possibly colour-blind, who
must never wonder how to stop the machine or what a colour means (ISA-101, ISA-18.2, ISO 9241).

| Rule | How it shows |
|---|---|
| Colour has one meaning each | Green = normal / running / online (default "status" scheme, the convention of machine indicator lights, IEC 60073); `--color-scheme high-performance` makes those grey for sites that follow ISA-101 strictly. Amber and red are only for warnings, alarms and stop. Controls are neutral; the one primary action is a light button, not a coloured one |
| Red means stop | EMERGENCY and device Stop buttons are the only red controls. Resetting a latch is never red |
| Priority is never colour alone | Critical and stop: red diamond. Warning: amber triangle. Info: blue circle. Always with the word |
| EMERGENCY is always there | Same place on every screen, 40 px high, never covered by a dialog, keyboard key `E`; when pressed the whole panel turns red |
| Every alarm is reachable | The Alarms screen lists all active alarms by priority, then unacknowledged, then newest, four per page on a 7" panel with Previous / Next; History is a separate view |
| Acknowledge what you read | Each unacknowledged alarm has its own Ack button; Acknowledge all is for a known flood |
| Instructions are never cut | The banner gives the top alarm and both steps of its guidance on their own lines; a test fails the build if any device's guidance does not fit a 7" panel |
| Real time | Alarm and event times are local wall-clock time, as in the shift log |
| Big targets | Header controls 40 px, alarm Ack buttons 38 px, faceplate buttons up to 48 px |
| No unreadable screens | Below 640x400 the panel shows only the alarm state and EMERGENCY, and says which size it needs |
| Say why | Blocked controls name the interlock (*Blocked: Stop latch*); stale data and offline devices say so; an empty screen explains itself |

## HMI heartbeat (PLC side)

A module can name a heartbeat register (`has_heartbeat`, `heartbeat_reg`). While the device is
online the HMI writes a counter (1..32767, never 0) there every second, but only while its own
screen loop is alive: a frozen display stops the heartbeat, so the PLC stops the machine. The PLC program should
treat 5 s without a change as "HMI lost" and go to its safe state, and stay there until an
operator resets it. The simulated tank program does this on `%MW7`; the integration test checks
that the pumps stop when the HMI goes silent and do not restart when it comes back. A real PLC
program needs the same few rungs before its module sets `has_heartbeat`. Drives use their own
communication-loss fault instead.

## Driver modules

A module is a `const` table: how to recognise the device, its tags, its register map, its controls,
and its two screen layouts. Modules live one per file in `src/devices/`.

| Module | Kind | Attaches by itself? | Map status |
|---|---|---|---|
| `altivar` (Altivar drives) | product | yes, on identity `Schneider Electric / ATV` | status and command words verified in public Schneider material; ramps and fault code not verified |
| `em6400` (PowerLogic energy meter) | product, read only | no, list it with `--plc ...@em6400` | addresses **not verified**; see the header of `src/devices/em6400_meter.c` |
| `tank`, `conveyor`, `boiler`, `pumps`, `compressor`, `ahu`, `abb` | PLC application | only when the PLC runs the program with that name (FC43 UserApplicationName, for example `HMI-TANK`) | these are the maps of the simulator's programs, not of your PLC's program |
| `generic` | anything | no | holding registers 0-7; raw writes need the Engineer role |

Application modules refuse to attach to a PLC that runs another program. A real M241 or M251 with
its own program is therefore never driven with a guessed register map. To use an application
module on a real PLC, the PLC program must answer with the matching program name, or the module
must be forced with `--plc <ip>@<module>` after its map has been checked against the program.

Adding a module: copy an existing file in `src/devices/`, edit its tables, add it to
`s_profiles[]` in `src/devices/device_registry.c`, and to the source lists in `build.bat` and the
`Makefile`. The tests check every module: tags in range, a comm-loss alarm, every control reachable,
both layouts, and (for simulated devices) that every address and interlock matches the simulator.

## Operating a PLC

Every control works the same way, following the faceplate convention of ISA-101 operator panels:

1. **Find it.** Each device has quick buttons for its two most used controls. Tap the device header
   (**Controls >**) for its faceplate: every value on the left, every control on the right.
2. **Who may.** If your role is too low, the control asks for the PIN of the role it needs, then
   carries on. Viewer can watch only.
3. **Interlocks.** A start is refused while its interlock is active (E-stop, fault, jam, fire alarm).
   The button says *Blocked: ...* and the device enforces the same rule.
4. **Values.** Setpoints open a keypad that shows the current value and the allowed range. Entries
   outside the range are refused before anything is sent.
5. **Confirmation.** Mode changes, burner start, counter resets, compressor stop, raw register
   writes and stop-latch resets ask "Are you sure?" first.
6. **Record.** Every command goes into the signed audit log with the role, what and the value.

**EMERGENCY** (header, also key `E`) sends a stop request to every device and is never refused,
whatever the role. Commands still waiting to be sent are cancelled first (a start queued a moment
earlier is never sent after the stop), a pulse in progress is released, then each device gets its
stop latch, every Stop / Off control, and the mode selectors that stop it (`HMI_A_MODE_STOPS` in a
module) go to 0. The whole panel then turns red and lists every device with its outcome (stop
sent, nothing to stop, NOT STOPPED: offline / no stop control / not on this HMI). It stays latched:
no other control works, every device that comes back online or is found meanwhile is stopped too,
and the HMI heartbeat is withheld. **Reset EMERGENCY** needs the Maintenance role and a
confirmation; it only leaves the red screen and sends nothing to the machines. It is a software
stop, not the machine's emergency stop: that must stay a hardwired device (ISO 13850,
IEC 60204-1). **Reset stop** on the Alarms screen needs the Operator role, an acknowledged stop
alarm and a confirmation, and does not restart anything.

Roles: Viewer (watch only), Operator, Maintenance, Engineer. The factory PINs (`1111`, `2222`,
`3333`) are refused at sign-in until each role has a site PIN set with `--set-pin` (IEC 62443-4-2
CR 1.5). Run `--set-pin` as the account the HMI runs under. On the bench, `--allow-factory-pins`
lifts this and the Settings screen says BENCH MODE; once any site PIN exists the option is refused
and the refusal is logged.

**Alarms.** Each unacknowledged alarm on the Alarms screen has its own **Ack** button: acknowledge
the alarm you have read (ISA-18.2). **Acknowledge all** stays for a known flood.

### Security files

| File | Holds | Protection |
|---|---|---|
| `hmi.key` | 32 random bytes made on the first start; the log key and the credentials key are derived from it (HKDF), the master is not kept in memory | Linux: mode 0600, and when made by root it is handed to the owner of the install folder; Windows: protected DACL for SYSTEM, Administrators and the account that created it |
| `security.vault` | salted PIN hashes, keyed with `hmi.key` | HMAC-SHA256 under a key derived from `hmi.key`; same owner and rights as `hmi.key`; an edited vault, or one from an older build, is refused and every role falls back to the (locked) factory PINs |
| `alarms.log` | alarms, events and operator commands, stamped with the time of the event | each line carries an HMAC-SHA256 chained to the line before; `--verify-log` shows the first broken line. Records reach the OS at once and the disk at least every 250 ms (stop and critical events at once). Lines cut off the end of the newest file, or a deleted rotated file, cannot be detected by a chain: copy the log off the panel if that matters |

Losing `hmi.key` means old logs can no longer be verified and every PIN must be set again. Back it
up to a place as safe as the panel itself.

## Tests

`make test` (Linux, with AddressSanitizer and UBSan) and `build.bat test` (Windows) run (counts at
the time of writing):

| Suite | Checks | What it covers |
|---|---|---|
| `test_core` | 11,794 | tags, alarms (incl. per-alarm acknowledge and summary order), guidance fits a 7" banner, header / alarm-screen geometry and touch-target sizes, RBAC and the commissioning gate, HMAC-SHA256 (RFC 4231) and HKDF (RFC 5869) vectors, key separation, signed vault, device key, Modbus codec, layouts and alarm-row geometry, renderer bounds, audit log forgery / truncation / wrong-key detection and flush policy, UTC formatting, HAL |
| `test_integration` | 570 | the runtime driving the simulator's device models: module and device agreement, every control, Altivar run / reverse / fault / reset, meter float decoding, link loss and recovery, HMI heartbeat loss stopping the tank, EMERGENCY on every machine type (also offline and mid-pulse), a machine that comes back online during an EMERGENCY stopped too |
| `test_core` with the MCU profile | 9,280 | the same checks with the small-device limits |
| `test_modbus_faults` | 74 | malformed frames, timeouts, exceptions, stale transaction ids, recovery (real sockets on Linux and Windows) |

The Raspberry Pi targets (`make CC=aarch64-linux-gnu-gcc`, `make CC=arm-linux-gnueabihf-gcc`, see
the top of the Makefile) pass the same suites under qemu, and `cppcheck` runs clean.

The live discovery test (`tests/test_modbus_live.c`) needs `tests/modbus_plc_sim.py` running and
checks plug-and-play over real sockets: discovery, meter filtering, a valve write reaching the
device, outage detection and recovery. Run it with the commands at the top of that file.

Static analysis: `cppcheck --std=c99 --enable=warning,performance,portability` reports no findings
on `src/` and `include/`.

## Platforms

| Target | Status |
|---|---|
| Windows 10/11, Harmony P6 (Windows 10 IoT) | earlier builds run on Windows; the current code is built with MinGW and its tests pass under Wine, but it has not been run on Windows itself since the security changes (in particular the `--set-pin` console prompt). Not yet run on a P6 |
| Linux PC (x86-64) | built, all tests; runs as a desktop window (tested on a virtual X display: the stop button clicked and confirmed at the simulator), install / uninstall script tested |
| Raspberry Pi 2/3/4/5 (32- and 64-bit, framebuffer, evdev) | cross-built; all tests pass under qemu; not run on a real Pi screen |
| ESP32-S3 (ESP-IDF 5.3) | port written; not compiled |
| Harmony ST6 panel | not applicable: Schneider's runtime runs there |
| Harmony STW6 web panel | possible in future: it shows any HTML5 server; the runtime has no web front end yet |

See [docs/LAB_DEPLOYMENT.md](../docs/LAB_DEPLOYMENT.md).

## Folder map

`include/` is the public interface,
`src/core`, `src/renderer` and `src/devices` are portable, `src/fieldbus` is the Modbus side,
`src/hal` holds the OS-specific code behind `include/hal/`, and `src/platform` holds the window
or framebuffer shell. `src/app/app.c` is the application logic and has no OS header.

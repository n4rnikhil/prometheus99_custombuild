# HMI runtime (`industrial_hmi`)

A lightweight operator panel in portable C99. It finds PLCs, drives and meters on Modbus TCP,
puts each one on the dashboard with a screen for its machine, and lets an operator control it.
The code has no heap allocation, is `-Werror` clean, and uses no mutexes between the UI and the
fieldbus threads.

See the [root README](../README.md) for the two-app overview and for what is and is not verified.

## Build

| Platform | Command | Output |
|---|---|---|
| Windows (MinGW-w64 gcc) | `build.bat` | `industrial_hmi.exe` |
| Windows tests | `build.bat test` | runs every test suite listed below |
| Windows snapshot tool | `build.bat snapshot` | BMP image of each screen |
| Linux / Raspberry Pi | `make` (cross: `make CC=aarch64-linux-gnu-gcc`) | `industrial_hmi` |
| Linux tests | `make test` | unit tests only |
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
| `--log <path>`, `--no-log` | alarm log file (default `alarms.log` next to the executable) |
| `--verify-log <path>` | check the alarm log's hash chain and exit |
| `--set-pin <role> <pin>` | change a sign-in PIN in the credential vault and exit |

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
5. **Confirmation.** Mode changes, burner start, counter resets, compressor stop and raw register
   writes ask "Are you sure?" first.
6. **Record.** Every command goes into the hash-chained audit log with who, what and the value.

Roles and default PINs: Operator `1111`, Maintenance `2222`, Engineer `3333`. Change them with
`--set-pin` before any real use.

## Tests

`build.bat test` runs, in order:

| Suite | Checks | What it covers |
|---|---|---|
| `test_core` (with UBSan traps) | 8,251 | tags, alarms, RBAC, Modbus codec, layouts, renderer bounds, alarm log tamper detection, HAL |
| `test_integration` | 478 | the runtime driving the simulator's device models: module and device agreement, every control, Altivar run / reverse / fault / reset, meter float decoding, link loss and recovery |
| `test_core` with the MCU profile | 7,590 | the same checks with the small-device limits |
| `test_modbus_faults` | 74 | malformed frames, timeouts, exceptions, stale transaction ids, recovery |

The live discovery test (`tests/test_modbus_live.c`) needs `tests/modbus_plc_sim.py` running and
checks plug-and-play over real sockets: discovery, meter filtering, a valve write reaching the
device, outage detection and recovery. Run it with the commands at the top of that file.

Static analysis: `cppcheck --std=c99 --enable=warning,performance,portability` reports no findings
on `src/` and `include/`. The Linux sources are type-checked against stub headers (no Linux
toolchain on the development PC), so the Linux build is not yet run on real hardware.

## Platforms

| Target | Status |
|---|---|
| Windows 10/11 | built and run here |
| Linux PC / Raspberry Pi (framebuffer, evdev) | sources type-checked; not run on hardware |
| ESP32-S3 (ESP-IDF 5.3) | port written; not compiled |
| Harmony ST6 panel | not applicable: Schneider's runtime runs there. See [docs/LAB_DEPLOYMENT.md](../docs/LAB_DEPLOYMENT.md) |

## Folder map

See the [root README](../README.md#layout). The short version: `include/` is the public interface,
`src/core`, `src/renderer` and `src/devices` are portable, `src/fieldbus` is the Modbus side,
`src/hal` holds the OS-specific code behind `include/hal/`, and `src/platform` holds the window
or framebuffer shell. `src/app/app.c` is the application logic and has no OS header.

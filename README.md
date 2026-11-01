# Prometheus HMI

**A lightweight industrial operator panel (HMI runtime) for PLCs, drives and energy meters over
Modbus TCP, with a built-in PLC simulator for testing without a plant.**

Prometheus HMI finds the devices on a control network by itself, shows their live values, raises
and manages alarms, and lets operators run the process with role-based sign-in and a signed audit
log. It is written in portable C99, needs no runtime libraries, and runs on Windows PCs and panel
PCs, Linux desktops and panels, and Raspberry Pi.

<p align="center">
  <img src="docs/img/process.png" alt="Process overview with a drive, a tank skid and an energy meter" width="640">
</p>

---

## Contents

- [Overview](#overview)
- [Features](#features)
- [Screenshots](#screenshots)
- [Quick start](#quick-start)
- [Using the panel](#using-the-panel)
- [Supported devices](#supported-devices)
- [Configuration](#configuration)
- [Commissioning a real plant](#commissioning-a-real-plant)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Project structure](#project-structure)
- [Standards](#standards)
- [Status and limitations](#status-and-limitations)
- [Further documentation](#further-documentation)
- [License](#license)

---

## Overview

The repository contains two independent applications:

| Application | Folder | Purpose | Platforms |
|---|---|---|---|
| **HMI runtime** (`industrial_hmi`) | [`hmi-runtime/`](hmi-runtime/) | The operator panel: device discovery, process screens, alarms, operator commands, access control and audit log | Windows 10/11 and Windows IoT panel PCs; Linux desktops (window), Linux panels and Raspberry Pi (framebuffer), servers (headless) |
| **PLC simulator** (`plc_simulator`) | [`plc-simulator/`](plc-simulator/) | A bench of simulated PLCs, drives and meters. Each one is a real Modbus TCP server, so the HMI (or any Modbus client) talks to it exactly as it would to hardware | Windows (window and console); Linux and Raspberry Pi (console) |

The two applications share no code at run time. The simulator's device models are compiled into
the HMI's integration test only, to check that each driver matches the device it controls.

## Features

**Plug and play**
- Discovers PLCs and drives on the network by their Modbus identity (FC 43/14) and attaches the
  matching driver module; devices that come and go are tracked automatically.
- Communication loss is detected per device, raised as an alarm, and recovered without operator
  action.
- Up to 8 devices per HMI, paged on small screens. Devices listed with `--plc` get a place first;
  any further device is named, raised as a warning, and reported by STOP ALL as not stopped.

**Operator interface (ISA-101)**
- Process overview, per-device faceplates, diagnostics, telemetry, alarms and settings screens.
- One meaning per colour; alarm priority shown by shape, colour and word, never colour alone.
- Setpoint keypads that show the current value and the allowed range; out-of-range entries are
  refused before anything is sent.
- Interlocked controls say why they are blocked; critical actions ask for confirmation.
- Large touch targets (40–48 px), a high-contrast outdoor theme, and a safe single screen on
  displays smaller than 640×400.

**Alarms (ISA-18.2)**
- Per-alarm state machine (new, acknowledged, returned to normal), priorities, and a summary
  sorted by priority and time, paged so every alarm is reachable.
- Per-alarm and acknowledge-all, a separate history view, and two-step operator guidance for the
  top alarm.
- Alarm sound: the panel's own two-tone signal on the PC's speakers, plus an optional wired horn
  or stack-light output (Linux GPIO, fail-safe option).

**STOP ALL**
- Always visible, never blocked by a dialog or by sign-in, keyboard key `E`.
- Cancels queued commands, then sends every device its stop latch, its stop controls and its
  stopping mode; devices that are offline or cannot be stopped are named on screen.
- An HMI heartbeat lets the PLC program stop the machine itself if the HMI or its cable is lost.

**Security (IEC 62443-4-2)**
- Roles (Viewer, Operator, Maintenance, Engineer) with least privilege, auto-logout and PIN lockout.
- Site PINs are required before any control is allowed; PINs are stored as keyed hashes.
- Audit log of every command and alarm, chained and signed with HMAC-SHA256 under a per-device key,
  and checkable offline with `--verify-log`.

**Small footprint**
- About 0.3 MB executable, under 1 MB of private memory, and a 19 KB frame buffer.
- 1 kHz fieldbus loop, separated from the display loop by lock-free hand-off; a watchdog on both.
- No installer, runtime or framework required; one executable per platform.

## Screenshots

| | |
|:---:|:---:|
| <img src="docs/img/process.png" width="400"><br>**Process overview** | <img src="docs/img/faceplate.png" width="400"><br>**Device faceplate** |
| <img src="docs/img/stop_all.png" width="400"><br>**After STOP ALL: alarm banner with guidance** | <img src="docs/img/alarms.png" width="400"><br>**Alarm summary** |
| <img src="docs/img/diagnostics.png" width="400"><br>**Diagnostics: fieldbus loop and register map** | <img src="docs/img/settings.png" width="400"><br>**Access control and system status** |

<p align="center">
  <img src="docs/img/plc_lab.png" alt="PLC simulator" width="720"><br>
  <b>PLC simulator: add devices, change values, inject faults, unplug links</b>
</p>

## Quick start

The complete operating guide is the **[User Manual](docs/USER_MANUAL.md)**.

### Windows: run from this folder (no installation)

1. Download or clone the repository.
2. Double-click **`plc-simulator\plc_simulator.exe`**.
3. Press **Open HMI** in the simulator window.

The simulator starts with a tank skid; add a drive and a meter with its buttons. The HMI finds
them on its own within a few seconds. To start the HMI on its own:

```bat
cd hmi-runtime
industrial_hmi.exe --config examples\bench.cfg
```

### Windows: installer

Run `Prometheus-HMI-Setup-<version>.exe` (no administrator rights needed), then open
**Prometheus HMI** from the desktop or the Start menu. The simulator and the HMI start together.

### Linux PC and Raspberry Pi

```sh
tar -xzf prometheus-hmi-linux-x86_64.tar.gz       # or -raspberrypi-arm64 / -raspberrypi-armhf
cd prometheus-hmi-linux-x86_64
./install.sh                                       # per user, no root
```

Then open **Prometheus HMI (bench)** from the applications menu, or run `prometheus-hmi-bench`.
On a desktop the HMI opens as a window; on a panel without a desktop it draws to the screen
directly; on a server use `--headless`.

### Signing in (test mode)

In test mode (bench mode) the factory PINs are allowed. Tap **Viewer** at the top of the screen,
choose a role and enter its PIN:

| Role | PIN | Can |
|---|---|---|
| Viewer | none | watch only; STOP ALL is always allowed |
| Operator | `1111` | start / stop, setpoints, acknowledge alarms, reset a stop |
| Maintenance | `2222` | everything above, plus tuning parameters and watchdog reset |
| Engineer | `3333` | everything above, plus raw register writes and fault injection |

Test mode is for the bench only. See [Commissioning a real plant](#commissioning-a-real-plant).

## Using the panel

| To | Do |
|---|---|
| See a device in detail | tap **Details** on its card |
| Operate a device | tap the control on its faceplate; the panel asks for the PIN of the role it needs |
| See more devices | up to 8 devices per HMI; on a small screen use **Next >** at the bottom of the Process screen |
| Change a setpoint | tap **Set ...**, type the value in the keypad (the allowed range is shown) |
| Stop everything | press **STOP ALL** (top right) or the `E` key |
| Handle an alarm | read the banner guidance, then **Ack** the alarm on the Alarms screen |
| Silence the horn | **Silence horn** on the Alarms screen |
| Restart after STOP ALL | release any hardwired E-stop, acknowledge the alarm, then **Reset stop** on the Alarms screen. Nothing restarts by itself |

> **STOP ALL is not an emergency stop.** It sends stop requests over Modbus and depends on this
> software, the network and each PLC program. It never replaces the hardwired emergency stop that
> ISO 13850 and IEC 60204-1 require on the machine, which is why it is labelled STOP ALL, not
> E-STOP.

## Supported devices

Each device type is a driver module in [`hmi-runtime/src/devices/`](hmi-runtime/src/devices/)
describing its registers, controls, alarms and operator guidance. The simulator provides a model of
each one.

| Device | Driver module | Typical controller |
|---|---|---|
| Schneider Altivar ATV340 / ATV630 drive | `altivar_drive.c` | variable speed drive |
| Schneider EM6400NG energy meter | `em6400_meter.c` | power meter (read only) |
| Tank skid | `tank_skid.c` | Modicon M340 |
| Conveyor | `conveyor.c` | Modicon M241 |
| Boiler | `boiler.c` | Modicon M580 |
| Pump station | `pump_station.c` | Siemens S7-1200 |
| Compressor | `compressor.c` | Allen-Bradley Micro850 |
| Air handler | `air_handler.c` | Modicon M251 |
| ABB drive (demonstration map) | `abb_drive.c` | ABB drive |
| Unknown Modbus device | `generic.c` | raw register view and writes |

New devices are added by writing one module file; see
[hmi-runtime/README.md](hmi-runtime/README.md#driver-modules).

> Register maps other than the Altivar status and command words are **not verified against real
> hardware**. Each module header lists its unverified addresses; check them against the device's
> documentation before use on a plant.

## Configuration

Options are read from `hmi.cfg` next to the executable (one option per line, `#` for comments) and
from the command line. Ready-made files are in [`hmi-runtime/examples/`](hmi-runtime/examples/):
`bench.cfg` for the simulator and `lab-site.cfg` for a real network.

| Option | Meaning |
|---|---|
| `--scan <range>` | discovery range, e.g. `192.168.1.0/24` or `10.0.0.5:502-510` |
| `--plc <ip[:port][@module]>` | attach a device explicitly, optionally forcing its driver module |
| `--config <file>` / `--no-config` | read options from another file / from none |
| `--log`, `--key`, `--vault <path>` | audit log, device key and PIN vault locations |
| `--set-pin <role>` | set a role's site PIN (asked twice, never typed on the command line) |
| `--verify-log <path>` | check the audit log's signature chain and exit |
| `--color-scheme status\|high-performance` | green normal states (default) or strict ISA-101 grey |
| `--no-sound` | no alarm tone from the speakers (a wired horn is not affected) |
| `--headless`, `--window`, `--fb <device>` | Linux display mode |
| `--horn-gpio <chip>:<line>`, `--horn-fail-safe` | Linux wired alarm output |
| `--allow-factory-pins` | **bench only**: allow the factory PINs |

The complete list is in [hmi-runtime/README.md](hmi-runtime/README.md).

## Commissioning a real plant

1. **Set a site PIN for every role** (6–8 digits, no repeated or sequential patterns), as the
   account the HMI runs under, with the HMI closed:

   ```bat
   industrial_hmi.exe --set-pin operator
   industrial_hmi.exe --set-pin maintenance
   industrial_hmi.exe --set-pin engineer
   ```

   Once any site PIN exists the panel is commissioned: factory PINs and test mode are refused, and
   the attempt is logged.
2. **Remove `--allow-factory-pins`** from the site `hmi.cfg`.
3. **Protect the key.** The first start creates `hmi.key` next to the executable. It signs the
   audit log and the PIN vault: keep it on the panel, readable only by the HMI account, and never
   in version control.
4. **Segment the network.** Modbus TCP has no authentication or encryption; place the HMI and the
   PLCs in their own zone (IEC 62443-3-3).
5. **Add the heartbeat rungs** to each PLC program that should stop when the HMI is lost (see
   [HMI heartbeat](hmi-runtime/README.md#hmi-heartbeat-plc-side)).

To return a bench panel to test mode, close the HMI and delete `security.vault` (keep `hmi.key`).

## Building from source

### Requirements

| Platform | Toolchain |
|---|---|
| Windows | MinGW-w64 GCC (MSYS2 or standalone) or Microsoft Visual C++ (`cl`) |
| Linux / Raspberry Pi | GCC or Clang, GNU make; `libx11-dev` headers for the desktop window (optional) |
| Raspberry Pi from a PC | `aarch64-linux-gnu-gcc` (64-bit OS) or `arm-linux-gnueabihf-gcc` (32-bit OS) |
| Windows installer | NSIS 3 (`makensis`) |

### Windows

```bat
cd plc-simulator
build.bat test          REM plc_simulator.exe, plc_simulator_cli.exe and the model tests

cd ..\hmi-runtime
build.bat test          REM industrial_hmi.exe and every test suite
```

### Linux and Raspberry Pi

```sh
cd plc-simulator && make && make test
cd ../hmi-runtime && make test                    # every suite, under AddressSanitizer + UBSan
make CC=aarch64-linux-gnu-gcc industrial_hmi      # cross-compile for a 64-bit Raspberry Pi
```

A headless bench across two machines:

```sh
./plc_simulator_cli --add atv340 --add meter --network
./industrial_hmi --headless --allow-factory-pins --scan <simulator-ip>:1502-1509
```

### Release packages

```sh
hmi-runtime/installer/build_windows_release.sh dist   # exe files and the Windows installer
hmi-runtime/installer/build_linux_release.sh dist     # Linux PC and Raspberry Pi archives
```

Both write SHA-256 checksums to `dist/SHA256SUMS.txt`.

## Testing

| Suite | Checks | Covers |
|---|---:|---|
| `test_core` | 11,513 | tags, alarms and summary order, layouts and touch-target sizes, access control and commissioning, HMAC-SHA256 / HKDF test vectors, signed vault and audit log (forgery, truncation, wrong key), Modbus codec, renderer bounds, alarm tone |
| `test_core` (MCU profile) | 8,999 | the same checks with the small-device limits |
| `test_integration` | 558 | the runtime driving the simulator's device models: every control, STOP ALL on every machine type and on 8 devices at once, devices beyond the limit, reset pulses, drive faults and resets, link loss and recovery, heartbeat loss |
| `test_modbus_faults` | 74 | malformed frames, timeouts, exceptions, stale transaction ids, recovery over real sockets |
| `test_plc_models` | 1,243 | the simulator's device models |

The suites pass on Linux x86-64 (under AddressSanitizer and UBSan), on 64-bit and 32-bit ARM
(Raspberry Pi targets, run under qemu), and as Windows builds; `cppcheck` reports no issues.

## Project structure

```
.
├── hmi-runtime/                  the operator panel
│   ├── include/                  public headers (core, devices, fieldbus, hal, renderer)
│   ├── src/
│   │   ├── app/                  the portable application (no OS headers)
│   │   ├── core/                 tags, alarms, access control, signed audit log, device key
│   │   ├── devices/              one driver module per device type
│   │   ├── fieldbus/             Modbus TCP client, device manager, discovery
│   │   ├── hal/                  OS interfaces: Windows and POSIX implementations
│   │   ├── platform/             Windows and Linux shells: window, framebuffer, input, sound
│   │   └── renderer/             band renderer, layouts, fonts
│   ├── ports/esp32/              ESP-IDF port (not yet compiled)
│   ├── tests/                    unit, integration, MCU-profile and fault-injection tests
│   ├── examples/                 site configuration files
│   ├── installer/                Windows installer and Linux / Raspberry Pi packages
│   ├── industrial_hmi.exe        ready to run (Windows)
│   └── build.bat, Makefile
├── plc-simulator/                the bench of simulated PLCs
│   ├── include/, src/            device models, Modbus TCP servers, window and console apps
│   ├── tests/                    device-model tests
│   ├── plc_simulator.exe         ready to run (Windows); plc_simulator_cli.exe for a console
│   └── build.bat, Makefile
├── docs/
│   ├── USER_MANUAL.md            the user manual
│   ├── LAB_DEPLOYMENT.md         running the HMI on lab panels
│   └── img/                      screenshots
├── LICENSE                       MIT license
└── README.md
```

`hmi-runtime` builds without the simulator folder, except for its integration test, which links
the simulator's device models.

## Standards

The design follows the standards below. **Nothing is certified, and this is not a safety system:**
no IEC 61508, IEC 62061 or ISO 13849 claim is made.

| Standard | Implemented | Not implemented |
|---|---|---|
| **ISA-101** (HMI design) | colour discipline, priority by shape + colour + word, neutral controls with red reserved for stop, large touch targets, untruncated guidance, small-screen fallback, outdoor theme | review with operators on a real panel; a site style guide |
| **ISA-18.2 / IEC 62682** (alarm management) | per-alarm state machine, priorities, sorted and paged summary, history, per-alarm acknowledge, horn with silence and wired output, operator guidance, communication-loss alarms | shelving, out-of-service, master alarm database |
| **ISO 13850 / IEC 60204-1** (stop functions) | STOP ALL never gated or covered; stop-latch reset needs a role, an acknowledged alarm and a confirmation, and never restarts anything; HMI heartbeat to the PLC | STOP ALL is a software stop, not an emergency stop |
| **IEC 62443-4-2** (component security) | roles with least privilege, auto-logout, PIN lockout, keyed PIN hashes, mandatory site PINs, signed audit log, per-purpose keys (HKDF), secret files restricted to the HMI account | Modbus TCP security (segment the network); per-person accounts; remote log forwarding |
| **Modbus** Application Protocol v1.1b3, TCP/IP v1.0b | FC 01, 03, 05, 06, 43/14 with strict frame checks; fault-injection tested | FC 02, 04, 15, 16 (no current module needs them) |

## Status and limitations

- **Tested:** all suites above; the Windows build and installer (run under Wine); the Linux desktop
  window, STOP ALL and the alarm tone end to end against the simulator; discovery over real sockets.
- **Not yet run on target hardware:** a physical Windows panel PC, a Raspberry Pi with a touch
  screen, and the ESP32 port (not compiled).
- **Not verified against real devices:** register maps other than the Altivar status and command
  words.
- **Not supported:** OPC UA and an HTML5 web client. Schneider Harmony ST6 panels run Schneider's
  own runtime; Harmony P6 (Windows 10 IoT) can run the Windows build. See
  [docs/LAB_DEPLOYMENT.md](docs/LAB_DEPLOYMENT.md).

## Further documentation

| Document | Contents |
|---|---|
| **[User Manual](docs/USER_MANUAL.md)** | installation, operation, alarms, STOP ALL, simulator, configuration, commissioning, troubleshooting |
| [hmi-runtime/README.md](hmi-runtime/README.md) | all options, operator guide, interface design rules, driver modules, heartbeat, tests |
| [plc-simulator/README.md](plc-simulator/README.md) | simulator apps, options, devices, network mode |
| [docs/LAB_DEPLOYMENT.md](docs/LAB_DEPLOYMENT.md) | running the HMI on lab panels: what works and what does not |

## License

Prometheus HMI is released under the [MIT License](LICENSE). Copyright (c) 2026 Nikhil Nuguri.

The software is provided "as is", without warranty of any kind. It is not a safety system; see
[Status and limitations](#status-and-limitations).

---

<sub>Prometheus HMI · Nikhil Nuguri</sub>

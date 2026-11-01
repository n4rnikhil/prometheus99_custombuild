# Prometheus Industrial HMI

Two independent applications in one repository:

| App | Folder | What it is | Runs on |
|---|---|---|---|
| **HMI runtime** (`industrial_hmi`) | [hmi-runtime/](hmi-runtime/) | The operator panel. Finds PLCs, drives and meters on the network, shows them, raises alarms, and sends operator commands (start/stop, setpoints, modes, resets) with role-based sign-in and a signed audit log | Windows (installer; incl. Harmony P6 / Windows IoT); Linux desktops (a normal window), Linux panels and Raspberry Pi (framebuffer), servers (headless); small MCUs (ESP32 class, not yet compiled) |
| **PLC simulator** (`plc_simulator`) | [plc-simulator/](plc-simulator/) | A bench of simulated PLCs, drives and meters. Each one is a real Modbus TCP server, so the HMI or any Modbus client talks to it as it would to hardware. Has RUN/STOP, fault injection and an activity log | Windows (window and console); Linux / Raspberry Pi (console) |

The two apps share no code at run time. The HMI does not link the simulator. The simulator's
device models are compiled into the HMI's integration test only, to check that each driver
matches the device it is meant to control.

## Layout

```
industrial_hmi/
├── hmi-runtime/                 the operator panel
│   ├── include/                 public headers
│   │   ├── core/                tags, alarms (ISA-18.2 journal), RBAC, signed audit log, device key, seqlock
│   │   ├── devices/             driver-module format (device_profile.h)
│   │   ├── fieldbus/            Modbus TCP codec, device manager, discovery
│   │   ├── hal/                 the only OS-facing interfaces (net, fs, sys, thread, timer)
│   │   └── renderer/            micro renderer: 4-bpp bands, layouts, bytecode fonts
│   ├── src/
│   │   ├── app/app.c            the portable application (no OS headers)
│   │   ├── core/ renderer/      portable modules
│   │   ├── devices/             one driver module per file (altivar, em6400, tank, ...)
│   │   ├── fieldbus/            Modbus TCP client, device slots, discovery
│   │   ├── hal/                 *_win32.c and *_posix.c implementations
│   │   └── platform/            win32 and linux shells: window / framebuffer and input only
│   ├── ports/esp32/             ESP-IDF project (not yet compiled)
│   ├── tests/                   unit, integration, MCU-profile and fault-injection tests
│   ├── examples/                site configuration files (hmi.cfg)
│   ├── build.bat, Makefile      Windows and Linux builds
│   └── measure.bat              runtime audit (loop rate, memory)
├── plc-simulator/               the bench of simulated PLCs
│   ├── include/                 device models and the bench API
│   ├── src/
│   │   ├── plc_models.c         device models and Modbus request handling (no sockets)
│   │   ├── modbus_server.c      the bench: non-blocking Modbus TCP servers
│   │   ├── main_win32.c         window app (Windows)
│   │   └── main_console.c       console app (Windows, Linux, Raspberry Pi)
│   ├── tests/                   device-model tests
│   └── build.bat, Makefile
├── .github/workflows/ci.yml     Linux, Raspberry Pi (qemu) and Windows builds and tests
├── docs/
│   ├── LAB_DEPLOYMENT.md        running the HMI on the lab panels: what works, what does not
│   ├── img/                     screenshots
│   └── superpowers/plans/       the portable-runtime plan
└── README.md                    this file
```

`hmi-runtime` builds without the simulator folder. Its integration test is the one exception: it
links the simulator's device models, so `build.bat test` needs `plc-simulator/` next to it.

## Install and run

| Computer | Install | Then |
|---|---|---|
| **Windows 10/11** | run `Prometheus-HMI-Setup-<version>.exe` (no administrator rights needed) | double-click **Prometheus HMI** on the desktop: the PLC simulator opens with a drive, a tank and a meter, then the HMI |
| **Linux PC** (Ubuntu, Debian, Fedora, ...) | `tar -xzf prometheus-hmi-linux-x86_64.tar.gz` then `./install.sh` (no root) | open **Prometheus HMI (bench)** from the applications menu |
| **Raspberry Pi** (64-bit or 32-bit OS) | the same with `prometheus-hmi-raspberrypi-arm64` / `-armhf` | with a desktop: the menu entry; on a bare panel the HMI draws to the screen directly |

Sign in: tap **Viewer** at the top, choose **Operator**, PIN **1111** (bench only). The release files
are built by `installer/build_windows_release.sh` and `installer/build_linux_release.sh` (and by CI).

## Build from source

**Bench on one Windows PC**

```bat
cd plc-simulator
build.bat test
plc_simulator.exe --add atv340 --add tank --add meter

cd ..\hmi-runtime
build.bat test
industrial_hmi.exe --config examples\bench.cfg
```

**Headless bench** (Raspberry Pi, Linux, or a Windows console)

```sh
cd plc-simulator && make && make test
./plc_simulator_cli --add atv340 --add meter --network
cd ../hmi-runtime && make test        # every suite, under AddressSanitizer + UBSan
./industrial_hmi --headless --allow-factory-pins --scan <simulator-ip>:1502-1509
```

**Before a real plant** (once per installation)

```sh
industrial_hmi --set-pin operator       # asked twice, never typed on the command line
industrial_hmi --set-pin maintenance
industrial_hmi --set-pin engineer
```

Until a role has a site PIN it cannot sign in (`--allow-factory-pins` lifts this on the bench
only). The first start creates `hmi.key` next to the executable: it signs the audit log and the
PIN vault. Keep it on the panel, readable only by the HMI account, and out of version control.

## What is verified and what is not

- **Verified here:** the unit, integration and fault-injection tests (counts in
  [hmi-runtime/README.md](hmi-runtime/README.md#tests)) on Linux x86-64 under AddressSanitizer and
  UBSan, on 32-bit and 64-bit ARM (Raspberry Pi targets, run under qemu), and the Windows build
  under Wine; a live discovery test over real sockets; the simulator's Modbus replies over TCP;
  the headless Linux HMI discovering simulated devices end to end and writing a signed log;
  `cppcheck` clean. The HMI showing simulated devices and sending them commands (screenshots in
  `docs/img/`).
- **Not verified against real hardware.** Only the Altivar status and command words (3201, 3202,
  8501, 8502) appear in public Schneider material. Every other real-device register address is
  listed as unverified in its module's header. Check them against the device's own documentation
  before relying on them.
- **Not run on the target hardware:** the ESP32 port (not compiled), the Linux build on a real
  Raspberry Pi with a framebuffer and touch screen (built and tested under emulation only), and
  the Windows build on a real panel PC (tested under Wine, not on Windows itself).
- **Not supported:** OPC UA. Harmony ST6 panels run Schneider's own runtime, which this project
  does not replace. Harmony P6 (Windows 10 IoT) can run the Windows build, and Harmony STW6 web
  panels can show any HTML5 server, which this runtime does not have yet. See
  [docs/LAB_DEPLOYMENT.md](docs/LAB_DEPLOYMENT.md).

## Operating a PLC

Each device has a panel with its live values and its controls. Controls are checked the same way
on every device: the HMI asks for the PIN of the role a control needs, a start is refused while
its interlock is active, and every command is written to the signed audit log. The
operator guide is in [hmi-runtime/README.md](hmi-runtime/README.md#operating-a-plc).

**STOP ALL is not an emergency stop.** The red header button sends a stop request to every
device over Modbus. It depends on this software, the network and each PLC program, so it can
never replace the hardwired emergency stop that ISO 13850 and IEC 60204-1 require on the
machine. It is labelled STOP ALL, not E-STOP, for that reason.

## Upgrading an installation from Build-5 or earlier

1. Stop the HMI and keep a copy of `alarms.log` and `security.vault`.
2. Install the new build. On its first start it creates `hmi.key`; the old log is moved to
   `alarms.log.v2` (still checkable with `--verify-log`, as integrity only) and a new signed log starts.
3. The old vault is unsigned and is refused: every role is locked until its PIN is set again.
   Run, **as the account the HMI runs under**:
   `industrial_hmi --set-pin operator`, then `maintenance`, then `engineer`.
4. Remove `--allow-factory-pins` from any site `hmi.cfg` (it is refused on a commissioned panel anyway).

## Standards

The design follows the standards below. **Nothing is certified**, and this is not a safety system:
no IEC 61508 / IEC 62061 / ISO 13849 claim is made.

| Standard | What the runtime does | Not done |
|---|---|---|
| ISA-101 (HMI design) | see [Operator interface design](hmi-runtime/README.md#operator-interface-design): green normal states (or grey with `--color-scheme high-performance`), amber / red only for abnormal ones, priority by shape + colour + word, neutral controls with red kept for stop, 40-48 px touch targets, guidance never truncated, a safe single screen on displays below 640x400, an outdoor theme | not yet reviewed with operators on a real panel; no site style guide |
| ISA-18.2 / IEC 62682 (alarms) | per-point state machine (unack, acked, return-to-normal unack), priorities, summary sorted by priority then time and paged so every alarm is reachable, separate history view, per-alarm and acknowledge-all, wall-clock times, horn with silence and a wired output (GPIO / ESP32 pin), two-step operator guidance for the top alarm, comm-loss alarms, journal sized for floods | shelving, out-of-service, alarm rationalisation database (master alarm database) |
| ISO 13850 / IEC 60204-1 (stop functions) | STOP ALL never gated, always visible, never covered by a dialog; the PLC stop latch is named as such on every screen; stop-latch reset needs a role, an acknowledged alarm and an explicit confirmation, and never restarts anything; an HMI heartbeat lets the PLC program stop by itself when the HMI or its cable is lost | STOP ALL is a software stop, not an emergency stop (see above) |
| IEC 62443-4-2 (security, component) | roles with least privilege, auto-logout, PIN lockout, keyed PIN hashes, site PINs required before control and bench mode refused once commissioned (CR 1.5), audit log signed with HMAC-SHA256 (CR 2.8, 3.9), one key per purpose derived with HKDF from a per-device key, secret files limited to the HMI account (POSIX 0600 with owner hand-off; Windows protected DACL) | Modbus TCP has no authentication or encryption: segment the control network (IEC 62443-3-3 zones and conduits); no per-person accounts; no remote log forwarding; PIN lockout is not kept across restarts |
| Modbus Application Protocol v1.1b3, Messaging on TCP/IP v1.0b | client for FC 01, 03, 05, 06, 43/14; strict frame checks; fault-injection tested | FC 02, 04, 15, 16 (no current driver module needs them) |

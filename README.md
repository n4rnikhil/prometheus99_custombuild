# Prometheus Industrial HMI

Two independent applications in one repository:

| App | Folder | What it is | Runs on |
|---|---|---|---|
| **HMI runtime** (`industrial_hmi`) | [hmi-runtime/](hmi-runtime/) | The operator panel. Finds PLCs, drives and meters on the network, shows them, raises alarms, and sends operator commands (start/stop, setpoints, modes, resets) with role-based sign-in and an audit log | Windows; Linux (PC, Raspberry Pi); small MCUs (ESP32 class, not yet compiled) |
| **PLC simulator** (`plc_simulator`) | [plc-simulator/](plc-simulator/) | A bench of simulated PLCs, drives and meters. Each one is a real Modbus TCP server, so the HMI or any Modbus client talks to it as it would to hardware. Has RUN/STOP, fault injection and an activity log | Windows (window and console); Linux / Raspberry Pi (console) |

The two apps share no code at run time. The HMI does not link the simulator. The simulator's
device models are compiled into the HMI's integration test only, to check that each driver
matches the device it is meant to control.

## Layout

```
industrial_hmi/
├── hmi-runtime/                 the operator panel
│   ├── include/                 public headers
│   │   ├── core/                tags, alarms (ISA-18.2 journal), RBAC, hash-chained log, seqlock
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
├── docs/
│   ├── LAB_DEPLOYMENT.md        running the HMI on the lab panels: what works, what does not
│   ├── img/                     screenshots
│   └── superpowers/plans/       the portable-runtime plan
└── README.md                    this file
```

`hmi-runtime` builds without the simulator folder. Its integration test is the one exception: it
links the simulator's device models, so `build.bat test` needs `plc-simulator/` next to it.

## Quick start

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
cd plc-simulator && make
./plc_simulator_cli --add atv340 --add meter --network
```

Then start the HMI on the same network with `--scan <simulator-ip>:1502-1509`.

## What is verified and what is not

- **Verified here:** the unit, integration and fault-injection tests (counts in
  [hmi-runtime/README.md](hmi-runtime/README.md#tests)); a live discovery test over real sockets;
  the simulator's Modbus replies over TCP; the HMI showing simulated devices and sending them
  commands (screenshots in `docs/img/`).
- **Not verified against real hardware.** Only the Altivar status and command words (3201, 3202,
  8501, 8502) appear in public Schneider material. Every other real-device register address is
  listed as unverified in its module's header. Check them against the device's own documentation
  before relying on them.
- **Not compiled for the target:** the ESP32 port, and the Linux binary on real hardware. The Linux
  sources pass a type check against stub headers only.
- **Not supported:** OPC UA. Harmony ST6 panels run Schneider's own runtime, which this project
  does not replace. See [docs/LAB_DEPLOYMENT.md](docs/LAB_DEPLOYMENT.md).

## Operating a PLC

Each device has a panel with its live values and its controls. Controls are checked the same way
on every device: the HMI asks for the PIN of the role a control needs, a start is refused while
its interlock is active, and every command is written to the hash-chained audit log. The
operator guide is in [hmi-runtime/README.md](hmi-runtime/README.md#operating-a-plc).

## Standards

The design follows ISA-101 (faceplates and display), ISA-18.2 (alarm journal, acknowledge and
return-to-normal states), role-based access in the style of IEC 62443, and Modbus Application
Protocol v1.1b3 / Modbus Messaging on TCP/IP v1.0b. The implementation is not certified.

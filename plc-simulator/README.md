# PLC simulator (`plc_simulator`)

A bench of simulated PLCs, drives and meters. Each device is a real Modbus TCP server on its own
port, so an HMI, a Modbus client or a SCADA tool connects to it as it would to hardware.
Use it to test without a plant: plug devices in and out, switch PLCs between RUN and STOP,
change process values, force inputs, and break the link.

It is a standalone application with its own build. It needs nothing else to build or run.

## Apps

| App | Platform | Use |
|---|---|---|
| `plc_simulator.exe` | Windows | window: add devices, plug/unplug, freeze, RUN/STOP, activity log, **Open HMI** |
| `plc_simulator_cli` | Windows, Linux, Raspberry Pi | headless: devices on the network, values printed on request |

## Build and run

```bat
REM Windows (MinGW-w64)
build.bat            REM plc_simulator.exe and plc_simulator_cli.exe
build.bat test       REM and the device-model tests
plc_simulator.exe --add atv340 --add tank --add meter
```

```sh
# Linux / Raspberry Pi
make                 # plc_simulator_cli
make test
./plc_simulator_cli --add atv340 --add meter --status 10
```

| Option | Meaning |
|---|---|
| `--add <type>[:n]` | device(s) on the bench (repeatable, 128 at most). `tank:20` adds 20 tanks; `mixed:100` adds 100 PLCs and drives of every type in turn. Types: `tank atv340 atv630 conveyor boiler pumps compressor ahu abb meter unknown` |
| `--port <n>` | first TCP port (default 1502; devices use n, n+1, ...). Ports below 1024 need root on Linux |
| `--network` | listen on every network interface instead of 127.0.0.1 only. **Must be combined with `--confirm-network`** (the window app asks instead) |
| `--run-for <s>`, `--status <s>` | CLI only: stop after s seconds; print every device's values every s seconds |

**Network mode is unprotected.** Modbus TCP has no password or encryption: in network mode every
computer that can reach this one can read and write every simulated register. Use it only on an
isolated test network. That is why it needs the extra confirmation.

Up to 128 devices, on ports 1502-1629 (the HMI scans these by default). A port that another
program already uses is skipped (the device takes the next free one, and the log says so).
More than 128: start several simulators on different first ports (`--port 2000`, `--port 2200`, ...).

Many devices in the window app:
- **+ 10 mixed** adds ten PLCs and drives of different types; **Shift**-click a type to add ten of it.
- With more than 8 devices the cards are compact (name, port, status, buttons); **Details** on a
  card shows its registers and switches.
- Scroll with the wheel, the scroll bar, Page Up / Page Down or Home / End. **Remove all** empties
  the bench.

## Devices

| Type | Identity (FC43) | Modbus map |
|---|---|---|
| `tank` | Schneider Electric / BMX P34 2020, program `HMI-TANK` | tank-skid program: level, pump, valve, mode, E-stop |
| `atv340`, `atv630` | Schneider Electric / ATV340U07N4E, ATV630U07N4 | Altivar communication words: ETA 3201, RFR 3202, FRH 3203, LCR 3204, LFT 7121, CMD 8501, LFR 8502, ACC 9001, DEC 9002. CiA402 state machine |
| `meter` | Schneider Electric / EM6400NG | IEEE floats, two registers each: current 2999+, voltage 3019+, power 3059, frequency 3109, energy 2699. These addresses are **unverified** against a real meter |
| `conveyor`, `boiler`, `pumps`, `compressor`, `ahu`, `abb` | per program (`HMI-CONVEYOR` etc.), ABB without a program name | application programs |
| `unknown` | none (no FC43) | holding registers 0-3 |

Whether a map matches a real device is a separate question: check the device documentation.

## Behaviour

- **RUN / STOP.** A PLC in STOP does not scan its program, but still answers Modbus. Writes go
  through; the process stays put.
- **Interlocks.** A start is refused while its interlock is set (E-stop, fault, jam). The refusal
  is logged on the device.
- **Heartbeat.** The tank program sets its stop latch when the HMI heartbeat register (%MW7)
  stops changing for 5 s, after it has been seen once.
- **Faults.** Freeze the link (no replies), answer with exception 04, or trip a drive or
  overload from the window.
- **Events.** Every write from a client is recorded in the activity log, for example
  `HMI wrote CMD = 16#0007` or `Drive state: switched on`.
- **Open HMI** starts `industrial_hmi.exe` when it is next to this app or in
  `..\industrial_hmi\hmi-runtime`.

## Tests

`build.bat test` / `make test` run `tests/test_plc_models.c` through Modbus frames: the register
tables, the Modbus codec, every device's process model, the Altivar state machine and fault
logic, the meter's floats and energy, and RUN/STOP.

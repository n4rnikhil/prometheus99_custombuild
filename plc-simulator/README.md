# PLC simulator (`plc_simulator`)

A bench of simulated PLCs, drives and meters. Each device is a real Modbus TCP server on its own
port, so the HMI runtime, a Modbus client or a SCADA tool connects to it as it would to hardware.
Use it to test the HMI without a plant: plug devices in and out, switch PLCs between RUN and STOP,
change process values, force inputs, and break the link.

It is an independent application. It does not need the HMI runtime to build or run.

## Apps

| App | Platform | Use |
|---|---|---|
| `plc_simulator.exe` | Windows | window: add devices, plug/unplug, freeze, RUN/STOP, activity log, **Open HMI** |
| `plc_simulator_cli` | Windows, Linux, Raspberry Pi | headless: devices on the network, values printed on request |

## Build and run

```bat
REM Windows
build.bat            REM plc_simulator.exe and plc_simulator_cli.exe
build.bat test       REM and the device-model tests
plc_simulator.exe --add atv340 --add tank --add meter --network
```

```sh
# Linux / Raspberry Pi
make                 # plc_simulator_cli
make test
./plc_simulator_cli --add atv340 --add meter --network --status 10
```

| Option | Meaning |
|---|---|
| `--add <type>` | a device on the bench (repeatable). Types: `tank atv340 atv630 conveyor boiler pumps compressor ahu abb meter unknown` |
| `--port <n>` | first TCP port (default 1502; devices use n, n+1, ...). Ports below 1024 need root on Linux |
| `--network` | listen on all interfaces (default: 127.0.0.1 only) |
| `--run-for <s>`, `--status <s>` | CLI only: stop after s seconds; print every device's values every s seconds |

## Devices

| Type | Identity (FC43) | Modbus map |
|---|---|---|
| `tank` | Schneider Electric / BMX P34 2020, program `HMI-TANK` | the tank-skid program: level, pump, valve, mode, E-stop |
| `atv340`, `atv630` | Schneider Electric / ATV340U07N4E, ATV630U07N4 | the Altivar communication words: ETA 3201, RFR 3202, FRH 3203, LCR 3204, LFT 7121, CMD 8501, LFR 8502, ACC 9001, DEC 9002. CiA402 state machine. The same words the runtime's `altivar` module uses |
| `meter` | Schneider Electric / EM6400NG | IEEE floats, two registers each: current 2999+, voltage 3019+, power 3059, frequency 3109, energy 2699. Same addresses as the runtime's `em6400` module, which are **unverified** |
| `conveyor`, `boiler`, `pumps`, `compressor`, `ahu`, `abb` | per program (`HMI-CONVEYOR` etc.), ABB without a program name | the programs the HMI's application modules expect |
| `unknown` | none (no FC43) | holding registers 0-3 |

The Altivar map is the one used in the HMI tests. The addresses are the same ones the runtime
uses, so a green simulator device and a green runtime module agree by construction. Whether they
match a real drive is a separate question; see the root README.

## Behaviour

- **RUN / STOP.** A PLC in STOP does not scan its program, but still answers Modbus. Writes go
  through; the process stays put.
- **Interlocks.** A start is refused while its interlock is set (E-stop, fault, jam). The refusal
  is logged on the device.
- **Faults.** Freeze the link (no replies), answer with exception 04, or trip a drive or
  overload from the window.
- **Events.** Every write from a client is recorded in the activity log, for example
  `HMI wrote CMD = 16#0007` or `Drive state: switched on`.

## Tests

`build.bat test` / `make test` run `tests/test_plc_models.c`: 1,229 checks through Modbus frames
covering the register tables, the Modbus codec, every device's process model, the Altivar state
machine and fault logic, the meter's floats and energy, and RUN/STOP.

The network layer was smoke-tested over TCP: the Altivar status word, a live CMD write, and the
meter's float current come back as expected. The end-to-end run with the HMI is described in the
root README.

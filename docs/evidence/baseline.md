# Baseline measurements

Measured on 2026-11-11 before the Build-19 changes, at commit `ae33db5`, on the development PC
(Windows 11 Home, x86-64). These are the reference numbers that later builds are compared with.

## Method

- Built with `hmi-runtime\build.bat test` (MinGW-w64 GCC, `-Os`, static).
- The bench was three simulated Modbus TCP devices on the same PC: an Altivar ATV340 drive, a tank
  skid PLC and an EM6400NG energy meter.
- The HMI ran for 20 s with `--config examples\bench.cfg --no-sound`, temporary key, log and vault
  files, and `--report` to write its own measured statistics.
- Process memory was read with `Get-Process` 12 s after start.

## Results

| Measure | Value |
|---|---|
| Executable size | 359,936 bytes |
| Start-up to first input-ready window | 110 ms |
| Working set | 1.2 MB |
| Private memory | 2.08 MB |
| Threads | 7 |
| Fieldbus loop rate | 1,000 Hz |
| Fieldbus period: average / min / max | 1,000 / 33 / 1,693 µs |
| Late fieldbus cycles in 20 s | 44 of 20,045 |
| UI frame rate | 30 frames/s |
| Slowest UI frame | 44 ms |
| Devices online | 3 of 3 |

## Test suites at baseline

| Suite | Checks | Result |
|---|---|---|
| Unit tests | 11,794 | passed |
| Integration tests | 570 | passed |
| Unit tests, MCU profile | 9,280 | passed |
| Modbus fault-injection tests | 74 | passed |

## Not measured here

- Data-to-screen latency: there is no probe yet. One is added in Build-19.
- Behaviour with more than 8 devices: the baseline caps devices at 8.

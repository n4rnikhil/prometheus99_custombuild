# ADR-0007: Device knowledge lives in constant-data driver modules

**Status:** accepted

## Context
Each device type has its own registers, controls, interlocks, alarm limits and operator
guidance. Spreading that knowledge through the code would make every new device a risky change
to the core.

## Decision
One file per device type in `src/devices/`, holding only constant data:
- the identity match (vendor, product, program name);
- readings: names, units, ranges, alarm limits, and two lines of guidance;
- register and coil maps, and the actions (start, stop, set, pulse, modes);
- the stop latch, the heartbeat register, and two screen layouts.

`device_registry.c` lists them. The device manager, alarm engine and renderer know nothing about
any particular device.

## Consequences
- **Adding a device is one new file and one line in the registry.** The integration test checks
  every module against a device model.
- Modules are read-only, so the threads read them without locks.
- Behaviour that does not fit the format (low alarm limits, per-device poll rates) needs the
  format extended first.

# ADR-0003: No heap; every table sized at build time

**Status:** accepted (capacity raised in Build-19)

## Context
An HMI that runs for months must not fragment memory or fail an allocation at the worst moment.
The same code must fit a microcontroller.

## Decision
- No `malloc`. Every table (tags, alarm points, devices, journal, rings) is static, sized from
  `include/config.h`.
- **Tag ids are 16-bit.** `HMI_MAX_DEVICES` is 128 by default, which gives 3,104 tags. A build
  can raise it to 250, the limit of 8-bit slot numbers. The MCU profile keeps 2.
- `--max-devices` lowers the limit at run time without a rebuild.

## Consequences
- Memory use is known before the program starts. Measured: 3.3 MB private memory with 96 devices
  attached ([benchmarks](../benchmarks/README.md)).
- The tables are zero-filled and do not need to be in the executable file. The Windows release
  build therefore does not use `-fdata-sections`, which would place about 1 MB of zeros in it.
- Capacity is a build-time choice. More than 250 devices needs a second HMI or wider slot numbers.

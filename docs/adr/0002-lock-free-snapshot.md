# ADR-0002: Lock-free hand-off between threads

**Status:** accepted

## Context
The fieldbus thread runs 1,000 times a second and must never wait for the screen. The UI thread
must never wait for the network. A mutex between them would let a slow frame delay device I/O,
or a slow device delay the screen, and makes priority inversion possible.

## Decision
- **Fieldbus → UI:** the fieldbus thread publishes its whole state into one shared block under a
  sequence lock every 10 ms. The UI copies it and retries if the copy was torn
  (`include/core/seqlock.h`).
- **UI → fieldbus:** atomic command bits, a per-alarm acknowledge bitmask, and single-producer
  single-consumer rings for device commands, alarm actions and audit events.
- A CRC over the tag table lets the UI check every snapshot end to end.

## Consequences
- Neither thread ever blocks on the other. Measured at 96 devices: 1 seqlock retry in 120 s and
  0 CRC errors ([benchmarks](../benchmarks/README.md)).
- The snapshot is about 160 KB at 128 devices. It is copied every 10 ms rather than every 1 ms,
  which adds at most 10 ms to the data-to-screen time.
- Every new piece of shared state must follow these rules. There is no shortcut.

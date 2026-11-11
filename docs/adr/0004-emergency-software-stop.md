# ADR-0004: EMERGENCY is a latched software stop that anyone can press

**Status:** accepted

## Context
In an emergency nobody should have to sign in, close a dialog or find the right screen. Workers
must never be unsure whether a machine has stopped. The HMI cannot be a certified emergency stop
either: it depends on a PC, a network and each PLC program.

## Decision
- One red **EMERGENCY** button, always in the same place, above every dialog, with no role
  needed (or the `E` key).
- Pressing it discards queued commands, then sends every device its stop latch or stop controls.
  The whole screen turns red, with one line per machine: stopped, or **NOT stopped** and why.
  Machines NOT stopped are listed first.
- The screen stays latched. Machines that come back online meanwhile are stopped too, and the HMI
  heartbeat is withheld. Only Maintenance can reset, after a confirmation, and the reset restarts
  nothing.
- The button, the red screen and the documents all say that it is a software stop, and that the
  hardwired E-stop must be pressed too.

## Options considered
- Gate it behind Operator sign-in: rejected. It slows the stop.
- Call it "E-STOP": rejected. That would suggest a certified device.

## Consequences
- The fastest path to stop is always one tap.
- A stop that did not reach a machine is visible at once, never silent.
- It does not replace the hardwired emergency stop that ISO 13850 and IEC 60204-1 require.

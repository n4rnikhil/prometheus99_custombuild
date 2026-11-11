# Test results — Build-19

Run on 2026-11-11 on Windows 11 (AMD Ryzen 7 7735HS), MinGW-w64 GCC 16.1.0, with the same
commands as `hmi-runtime\build.bat test`: `-std=c99 -Wall -Wextra -Wshadow -Werror`, unit and
integration suites with UBSan (`-fsanitize=undefined -fsanitize-trap=all`).

| Suite | Checks | Failed | Baseline (Build-18) |
|---|---:|---:|---:|
| `test_core` (unit) | 1,354,236 | 0 | 11,794 |
| `test_core` (MCU profile) | 10,239 | 0 | 9,280 |
| `test_integration` | 816 | 0 | 570 |
| `test_modbus_faults` (real sockets) | 83 | 0 | 74 |
| `tools/render_snapshot` (pedantic C99 build) | builds | — | builds |
| `cppcheck` (C99; warning, performance, portability; `src/`, `include/`, `tests/fixtures/`) | 0 findings | — | 0 findings |

**Why the unit count grew so much:** the layout test checks every device count from 1 to
`HMI_MAX_DEVICES` at six screen sizes, with and without the banner. With 128 devices instead of 8,
it now runs more than a million geometry and hit-test checks.

## What the testing block added

- **Unit (`test_core`):**
  - 16-bit tag ids and the 128-device table;
  - shelving: hidden from the list, counts and horn; still evaluated; comes back after its time,
    also across the 32-bit timer wrap; early unshelve;
  - out of service, and both together; stop alarms cannot be hidden;
  - acknowledge-all skips hidden alarms;
  - a limit of 0 means "no limit";
  - flood start, its end and Info silence during it; rate buckets across minutes and after a gap;
  - chatter flag and its reset;
  - the 512-record journal and lookup by sequence number;
  - alarm summary order with 60 alarms above tag id 255, including the first-n result;
  - Readings rows, filters, Search, pages: drawn rectangles equal hit areas;
  - search keyboard: 40 keys inside the dialog, each hit as itself;
  - alarm dialog buttons; EMERGENCY still reachable above both dialogs;
  - three alarm tabs clear of the title buttons at 640 to 1920 px;
  - six dock tabs; tiles never smaller than 176 x 88;
  - the new commands' roles; the protected-folder path.
- **Integration:** `--max-devices` limit and its fallback.
- **Fault injection:** an identity swap over a real socket. The other device is blocked, the
  comm-loss alarm is raised, nothing is read or written while blocked, a device that stops
  identifying itself is blocked too, and the right device is accepted again.

## Bugs found by these tests, and fixed

1. **Alarms screen at 640 px wide:** the History tab lay under the Reset stop button, so a tap
   on it opened the "Reset the stop latch?" confirmation instead. Narrow screens now get narrower tabs and buttons
   (`src/renderer/layout.c`).
2. **Test hang:** tests looped over the journal with an 8-bit counter. With 512 records it never
   ended. This was a test bug, fixed in three test files.

The security review ([`C:\Users\nikhi\audits\build19\REPORT.md`] on the build PC) found two more,
also fixed: a protected folder pre-created by another account was trusted (F1, medium), and shelve
records did not name the role (F2, low).

## Not run

- Linux, Raspberry Pi and ESP32 builds and tests (see [platforms.md](platforms.md)).
- `tests/test_modbus_live.c` with `tests/modbus_test_device.py`, and `tests/ui_modal_stall_test.py`.
- The Windows installer build (NSIS is not installed here).

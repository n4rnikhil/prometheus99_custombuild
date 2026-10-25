# Prometheus Industrial HMI

A lightweight HMI runtime for Schneider Modicon M340/M580 PLCs, written in pure C99.
It talks Modbus TCP and runs on Windows today. The rendering and logic core has no
OS dependencies, so it can move to Linux framebuffers and microcontrollers.

![Process overview](docs/img/process.png)

| | Measured on the dev laptop (Windows 11, `measure.bat`) |
|---|---|
| Executable | **160 KB**, statically linked, no DLLs beyond Windows itself |
| RAM | **3.6 MB** working set, **1.7 MB** private |
| Frame buffer | one **19,200-byte** band buffer, reused for every band of every frame |
| Heap | no `malloc`/`free` in application code (the C runtime's start-up code imports them) |
| Fieldbus loop | **1,000 Hz**, average period 1,000 µs, worst case about 1.6 ms |
| UI | 30 fps; anti-aliased Inter text and shapes through a 16-colour palette |
| Modbus TCP | about 1 ms round trip to a local PLC simulator; auto-reconnect after a link loss |
| Tests | 3,373 unit checks (UBSan traps), live Modbus outage test, cppcheck clean, `-Werror` clean |

---

## Quick start

```bat
build.bat test          :: build industrial_hmi.exe and run the unit tests
industrial_hmi.exe      :: start with the built-in process simulator
measure.bat             :: 8-second runtime audit (RAM, loop rate, jitter, log check)
```

Requirements: MinGW-w64 GCC on `PATH` (MSVC also works from a Developer Prompt).

> **Smart App Control.** Windows 11's Smart App Control can block a freshly
> compiled, unsigned exe ("An Application Control policy has blocked this file").
> It decides per build, so one build may run and the next may not. Fixes, in order of
> preference: sign the exe with a trusted code-signing certificate, or run it on a
> machine where Smart App Control is off (it is normally off on managed or
> embedded devices). `measure.bat` reports this case explicitly.

### Connect a real PLC

```bat
industrial_hmi.exe --plc 192.168.1.50          :: port 502, unit 1
industrial_hmi.exe --plc 192.168.1.50:502/3    :: unit id 3
```

No PLC to hand? `python tests\modbus_plc_sim.py --port 1502` starts a Modbus TCP
server that behaves like the plant. Then run `industrial_hmi.exe --plc 127.0.0.1:1502`.
Add `--outage-at 10 --outage-for 5` to watch the HMI ride through a link loss.

PLC data map (0-based Modbus addresses; on an M340/M580 `%MWn` is holding register n, `%Mn` is coil n):

| Address | Tag | Unit | Access |
|---|---|---|---|
| %MW0 | Tank 01 level | 0.1 % | read |
| %MW1 | Feed pump A speed | RPM | read |
| %MW2 | Valve V-101 position | % | read / **write** (FC06) |
| %MW3 | Line pressure | kPa | read |
| %MW4 | Coolant temperature | °C | read |
| %M0 | Pump A run | – | read / **write** (FC05) |
| %M1 | Coolant pump B | – | read / write |
| %M2 | E-stop latch | – | read / write (1 = trip, 0 = reset) |

---

## Using it

The HMI starts as **Viewer**, which can monitor only. Tap the role chip (or press `L`) to sign in.

| Role | Factory PIN | Can do |
|---|---|---|
| Viewer | none | watch; trip the E-STOP |
| Operator | 1111 | acknowledge / silence alarms, start/stop pump, move valve, reset E-stop |
| Maintenance | 2222 | + reset the watchdog failover latch |
| Engineer | 3333 | + watchdog fault-injection test |

**Change the factory PINs before real use** (Settings shows a warning until you do):

```bat
industrial_hmi.exe --set-pin operator 482913
industrial_hmi.exe --set-pin engineer 77310455
```

Three wrong PINs lock sign-in for 30 s. After 5 minutes without input the HMI drops back to Viewer.

| Action | Touch / mouse | Keyboard |
|---|---|---|
| Switch screen | tab bar, or swipe left/right | `1`–`5`, `Tab`, arrows |
| Emergency stop | red **E-STOP** (always visible, works over dialogs) | `E` |
| Acknowledge / silence | banner button or Alarms screen | `Space`/`A`, `S` |
| Pump / valve | buttons on the process cards | `P`, `V` |
| Reset E-stop | Alarms screen (after acknowledging) | `X` |
| Sign in / out | role chip, Settings | `L` |
| Outdoor (light) theme | moon button, Settings | `T` |
| Watchdog test / reset | Settings | `F` / `W` |

Actions fire when you lift your finger, not when you press, so a swipe that starts on a
button never operates it. Holding a key down sends one command, not a stream.

![Alarms](docs/img/alarms.png)

### Command line

| Option | Meaning |
|---|---|
| `--plc <ip[:port][/unit]>` | Modbus TCP instead of the simulator (numeric IPv4 or `localhost`) |
| `--log <file>` / `--no-log` | alarm audit log location (default `alarms.log` next to the exe) |
| `--vault <file>` | PIN vault location (default `security.vault` next to the exe) |
| `--set-pin <role> <pin>` | store a new 4–8 digit PIN, then exit |
| `--verify-log <file>` | check the log's hash chain; exit 0 = intact, 2 = tampered |
| `--exit-after-ms <n>`, `--report <file>` | timed run with a measured-statistics report (used by `measure.bat`) |

---

## How it works

```
  PLC (Modbus TCP) ──► Fieldbus thread, 1 kHz ─────────────► seqlock ──► UI thread, 30 Hz
                       drivers · ISA-18.2 alarms · trend         │         snapshot + CRC check
                       watchdog for the UI loop                  │         renderer → band buffer → screen
                       ◄── command bits / audit-event ring ──────┘         RBAC · PIN login · gestures
                                                                           watchdog for the fieldbus loop
                                                                           hash-chained alarm log
```

* **Two loops that watch each other.** Each thread publishes a heartbeat and supervises
  the other's. Settings → *Test watchdog* stalls the fieldbus thread for 750 ms. The UI
  trips, freezes the data as *stale*, and latches failover until Maintenance resets it.
* **Lock-free hand-over.** The fieldbus thread publishes a complete snapshot through a
  seqlock with C11-style fences (correct on ARM, not just x86). The UI checks a CRC32-C
  of every snapshot it reads.
* **ISA-18.2 alarms.** Every tag is an alarm point with its own latched state
  (Unacknowledged → Acknowledged → Normal, or *Cleared, unacknowledged*). The points have
  deadbands, re-annunciate on escalation, and E-stop outranks everything. Silence and
  acknowledge are separate actions. Tags with bad quality freeze rather than falsely clear.
* **Audit trail.** Every alarm transition, operator command, sign-in and access denial goes
  into `alarms.log`. Each line's CRC covers the previous line's CRC, so editing, deleting
  or reordering lines is detected (`--verify-log`). The chain continues across restarts
  and log rotation.
* **Device-described UI.** A driver ships a *profile*: tag metadata plus a manifest of
  4-byte widget tokens. The whole process screen is 14 tokens (56 bytes,
  `src/drivers/schneider_profile.c`). The renderer draws only what the manifest lists.
* **Band renderer.** Frames are drawn one horizontal band at a time into a single
  19,200-byte 4-bpp buffer (10 bands at 800×480) and streamed out. Anti-aliasing works
  through a 16×16×4 palette blend table, so there's no extra colour depth and no
  floating point in the pixel paths. Text uses the **Inter** typeface, pre-rendered at
  build time into 26 KB of 2-bit glyphs (`tools/bake_fonts.py`), so there's no font engine.
* **Responsive.** All geometry comes from one module (`layout.c`) that both drawing and
  touch hit-testing use. It adapts from 640×400 up to 7680 px wide; narrow screens get a
  compact header.

### Portability and porting

| Portable C99 (no OS headers) | Windows-specific (the platform layer) |
|---|---|
| `core/` tags, alarms, security, sha256, crc32, watchdog | `main.c` (window, threads, CLI) |
| `renderer/` layout, micro_renderer, font_inter | `hal/display_win32.c`, `hal/timer.c` |
| `drivers/` modbus_proto, schneider_profile | `drivers/driver_modbus.c` (WinSock), `driver_host.c` |
| `hal/gesture.c` | `core/alarm_log.c` (Win32 file API) |

`build.bat snapshot` builds `tools/render_snapshot.c`, a complete port in about 150 lines
that uses only `<stdio.h>`. It renders every screen to BMP files; the screenshots in this
README come from it. A new target needs the same small surface:

1. a **display sink**: copy each 4-bpp band to the panel (framebuffer `memcpy`, SPI/DMA transfer, …)
2. a **millisecond clock** and a way to run the 1 kHz loop (thread, RTOS task, or timer interrupt)
3. **touch/key events** fed into `hmi_gesture_*` and `hmi_layout_hit_test`
4. a **socket** for Modbus TCP (`modbus_proto.c` already does the framing) and a **file**
   for the alarm log and vault (or flash pages)

The Linux framebuffer and microcontroller back-ends are not written yet. Everything they
would call is in the portable column above.

---

## Standards: what is implemented

| Standard | Implemented | Not implemented / limits |
|---|---|---|
| IEC 62443-4-2 | RBAC enforced on every command; PIN authentication (salted, iterated SHA-256, constant-time compare); lockout after 3 failures; inactivity logout; audit log of sign-ins and denials; PE hardening (ASLR, high-entropy VA, DEP, stack protector) | Role PINs, not personal user accounts; no TLS / Modbus security (plain Modbus TCP has none); the CRC chain detects tampering but is not a cryptographic signature |
| ISA-18.2 / IEC 62682 | per-point latching, priorities, deadband, re-annunciation, silence ≠ acknowledge, journal + persistent log | no shelving / suppression, no alarm-flood analytics |
| ISA-101 | grey-scale process graphics; colour only for abnormal states; consistent navigation | – |
| IEC 61508 (SIL-*ready* design practices) | no dynamic memory; bounded loops; cross-supervising watchdogs with latched failover; snapshot CRC | **not certified**. The HMI E-STOP is a *request* to the PLC: a real plant still needs a hardwired, certified E-stop circuit |

---

## Tests

```bat
build.bat test                                         :: 3,373 unit checks, UBSan traps
python tests\modbus_plc_sim.py --port 1502 --outage-at 4 --outage-for 3 --run-for 14
tests\test_modbus_live.exe 127.0.0.1:1502 12           :: values, write, outage, recovery
industrial_hmi.exe --verify-log alarms.log
```

The unit tests cover:
* CRC32-C and SHA-256 test vectors
* tag scaling and the trend ring
* every ISA-18.2 transition
* RBAC, PIN login and lockout, and the vault
* uint32 clock wrap and the watchdog edges
* the Modbus codec, including 20,000 random frames
* tap and swipe classification
* layout and hit-tests at six screen sizes, with dialogs open and closed
* rendering every screen at 13 sizes (1×1 up to 20000 px wide) with guard bytes around the band buffer
* alarm-log tamper detection, overrun, restart and rotation

## Layout

```
build.bat / measure.bat      build, test, snapshot, runtime audit
include/  src/               core · renderer · drivers · hal · main.c
tests/                       test_core.c, test_modbus_live.c, modbus_plc_sim.py
tools/                       bake_fonts.py (Inter → C), render_snapshot.c (portable port)
fonts/OFL.txt                Inter licence (SIL Open Font License 1.1)
docs/img/                    screenshots rendered by the portable tool
```

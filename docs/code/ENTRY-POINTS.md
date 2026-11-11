# Where the code starts

A guided tour for someone reading the HMI's source for the first time. Each part names the
function to open first and says in plain words what happens there. Paths are relative to
`hmi-runtime/`.

---

## 1. The program starts: `main`

| Platform | File | Function |
|---|---|---|
| Windows | `src/platform/win32/main_win32.c` | `WinMain` |
| Linux, Raspberry Pi | `src/platform/linux/main_linux.c` | `main` |
| ESP32 (never compiled) | `ports/esp32/` | `app_main` |

The platform file is a thin shell. It does only what the operating system must do:

1. `app_parse_args()` reads `hmi.cfg` and the command line (`src/app/app.c`). One-shot jobs
   such as `--set-pin` or `--verify-log` end here.
2. `app_init()` checks that no other copy of the HMI is running, loads the device key and the
   PIN vault, registers the HMI's own readings, and opens the audit log. Device readings are
   registered later, when each device is attached.
3. The shell opens a window (Windows, X11), or the framebuffer (Linux panel), or nothing
   (`--headless`).
4. `app_start_workers()` starts two threads: the **fieldbus** thread and the **discovery** thread.
5. The shell's own loop calls `app_tick()` about 30 times a second, and passes input to
   `app_pointer_down/move/up()` and `app_key()`.
6. On close: `app_stop()`, then `app_shutdown()`.

Everything below `src/app/` is portable C99 with no operating-system headers. The operating
system is reached only through `include/hal/` (network, files, threads, timers, sound).

## 2. Three threads, no locks between them

```
 discovery thread ──found devices──▶ fieldbus thread ──snapshot (seqlock)──▶ UI thread
                                          ▲                                     │
                                          └──── commands (atomic bits, rings) ──┘
```

| Thread | Function (`src/app/app.c`) | Rate | Job |
|---|---|---|---|
| UI | `app_tick()` → `app_frame()` → `app_render()` | 30 frames a second | read the latest snapshot, build the view, draw, handle touch and keys |
| Fieldbus | `fieldbus_thread()` | 1,000 loops a second | talk Modbus to every device, run the alarm engine, publish the snapshot |
| Discovery | `discovery_thread()` → `hmi_discovery_run()` | every 3 s | look for new PLCs on the configured addresses |

The UI never waits for the network. The fieldbus thread never waits for the screen.

- **Fieldbus → UI:** every 10 ms the fieldbus thread copies its whole state (tags, alarms,
  devices, timing) into one shared block under a sequence lock (`fb_publish()`). The UI copies it
  out (`ui_take_snapshot()`). If a copy was torn, the UI simply copies again.
- **UI → fieldbus:** atomic command bits (acknowledge, silence, EMERGENCY reset), a per-alarm
  acknowledge mask, and small one-way queues for device commands and alarm actions.

## 3. The fieldbus loop: `fieldbus_thread()`

One pass, every millisecond:

1. Heartbeats and the watchdog: is the UI thread still alive?
2. Operator commands from the UI: acknowledge, silence, stop latch reset, EMERGENCY,
   shelve, device commands.
3. `hmi_devmgr_poll()` (`src/fieldbus/device_manager.c`). For each device:
   - connect, and check its identity (FC43) after every connect;
   - send the next read;
   - collect the answer into the tag table;
   - send queued writes and the heartbeat;
   - mark the device offline when it stops answering.
4. Once a second, one trend sample per device.
5. Every 10 ms: `hmi_alarms_evaluate()`, the alarm engine.
6. Every 10 ms: publish the snapshot for the UI.

Device knowledge lives in `src/devices/*.c`, one file per device type. Each is a table of
constant data: identity, register map, readings with their limits and operator guidance,
controls, and screen layout. `device_registry.c` lists them. The device manager knows nothing
about any particular device.

## 4. The alarm engine: `hmi_alarms_evaluate()`

File: `src/core/alarms.c`. Called by the fieldbus thread only.

For every reading that has metadata:

1. Skip it while its data is bad (the device's comm-loss alarm covers it).
2. Work out the condition: high, high-high, or the digital alarm. Apply a deadband before an
   alarm clears.
3. Move the state machine: `NORMAL → UNACK → ACKED → NORMAL`, or `UNACK → RTN_UNACK` when it
   clears before anyone acknowledged it. A higher priority annunciates again.
4. Write every change to the in-memory journal. The UI thread copies the journal into the
   signed audit log (`hmi_alarm_log_sync()` in `src/core/alarm_log.c`).

Then it rebuilds the summary: counts, horn, the banner's guidance, the alarm rate and flood
state. Shelved and out-of-service alarms are evaluated but not shown or sounded. Details are in
[`docs/design/ALARMS.md`](../design/ALARMS.md).

## 5. The renderer: `app_render()` → `hmi_renderer_render_band()`

Files: `src/renderer/micro_renderer.c` (drawing) and `src/renderer/layout.c` (geometry).

The screen is drawn in horizontal **bands** into a single 19,200-byte buffer of 4 bits per
pixel. For each band, `hmi_renderer_render_band()` clears the band, then draws whatever crosses
it: the screen's content, the header, the alarm banner, the dock, and any open dialog. The
platform shell then copies that band to the screen. No full-screen frame buffer exists.

- **One geometry for drawing and touch.** Every rectangle comes from `layout.c`. The renderer
  draws at those rectangles, and `hmi_layout_hit_test()` answers "what did the finger touch?"
  with the same rectangles, so a tap always lands on what is drawn.
- **Expensive work once per frame, not once per band.** Sorting the alarms and building the
  Readings list happen in `ui_build_view()` (`src/app/app.c`). The results are passed in
  `hmi_view_t`.
- **Fonts and colours.** Anti-aliased Inter fonts (`font_inter.c`) blend through a 16-colour
  palette with a lookup table. There is no floating point in the pixel paths.

## 6. Touch and keys: from a tap to a command

1. The shell calls `app_pointer_up(x, y)`.
2. `hmi_layout_hit_test()` returns what is under the finger.
3. `ui_dispatch_hit()` acts on it. A machine command goes through `ui_start_action()`:
   - is the device online?
   - is the role high enough? If not, ask for the PIN of the role needed;
   - is an interlock set?
   - is a value needed? If so, open the keypad;
   - is a confirmation needed? If so, ask;
   - send.
4. `hmi_security_authorize()` (`src/core/security.c`) makes the role decision. Every refusal
   is written to the audit log.

**EMERGENCY** is the one exception: `ui_emergency_press()` sets the latch at once, from any
screen and any dialog, for any role.

## 7. Security in one paragraph

`hmi.key` (in the protected folder, see `hal_secure_dir()`) is the master key. Separate keys
for the audit log and the PIN store are derived from it with HKDF (`src/core/device_key.c`).
The audit log is an HMAC-SHA256 chain (`alarm_log.c`). PINs are stored as salted, keyed hashes
in `security.vault`, signed with the same key (`security.c`). Factory PINs work only with
`--allow-factory-pins`, and never once a site PIN is set.

## 8. Tests

| File | What it checks |
|---|---|
| `tests/test_core.c` | tags, alarms, layout and hit testing, security, crypto vectors, audit log, renderer bounds |
| `tests/test_integration.c` | the whole runtime against the device models in `tests/fixtures/`, over Modbus frames in one process |
| `tests/test_modbus_faults.c` | malformed frames, timeouts and exceptions over real sockets |
| `tests/test_modbus_live.c` | manual: against `tests/modbus_test_device.py` |
| `tests/ui_modal_stall_test.py` | Windows: the watchdog survives the window being dragged |

`build.bat test` (Windows) or `make test` (Linux) builds and runs them.

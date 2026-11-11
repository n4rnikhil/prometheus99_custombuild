# Operator Interface Design

How the Prometheus HMI screens are laid out, and why. Every rule here is implemented in
`hmi-runtime/src/renderer/layout.c` (geometry, shared by drawing and touch) and
`hmi-runtime/src/renderer/micro_renderer.c` (drawing).

> **Verified so far:** the screens below compile and were seen working on Windows against
> simulated devices, up to 96 devices and 1,074 readings, at 800 x 480 and at a larger window size.
> **Not verified yet:** automated layout tests for the new screens, a real 7" touch panel,
> and use with gloves. See [`docs/TEST-DEBT.md`](../TEST-DEBT.md).

---

## 1. Principles

The layout follows the ideas of ISA-101 (high-performance HMI). The HMI is not certified to it.

| Principle | What the HMI does |
|---|---|
| Normal is quiet | Normal values are drawn in plain text. Colour is kept for things that need attention. |
| Red means stop or alarm | Red is used only for the EMERGENCY button, stop controls and critical alarms. |
| Priority is never colour alone | Every alarm priority has its own shape: diamond (critical), triangle (warning), circle (info). |
| Show where a value sits | Readings show a range bar with the warning and trip limits marked. |
| Overview first, detail on demand | Overview tiles first, then readings, then each device's faceplate. |
| Same place, every time | Screen positions do not move when values or alarms change. |
| Nothing is cut off | Text that does not fit ends in "...". Lists page instead of overflowing. Hidden alarms are counted on the page buttons. |

## 2. Screen map

```
 Header:  name / device count | theme | health | alarm count | role | EMERGENCY
 Banner:  the newest alarm that needs attention, with its two guidance lines and Acknowledge
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │  Content: one of six screens                                                 │
 └──────────────────────────────────────────────────────────────────────────────┘
 Dock:  Process | Readings | Alarms | Diagnostics | Telemetry | Settings     (keys 1 to 6)
```

| Screen | Level (ISA-101) | Purpose |
|---|---|---|
| Process | 1 – overview | Every device at a glance. |
| Readings | 2 – unit detail | Every reading of every device, grouped, filtered and searchable. |
| Alarms | – | Active alarms (sorted by priority, then time) and the event history. |
| Diagnostics | 4 – support | Loop timing, data integrity, latency, device links, register map. |
| Telemetry | 4 – support | The host computer's own CPU, memory and temperature. |
| Settings | – | Sign in, theme, watchdog test and reset. |
| Faceplate | 3 – device detail | A dialog: all values of one device and all of its controls. |

The **EMERGENCY** screen replaces all of these while an EMERGENCY is active (see the user manual).

## 3. Process screen at any device count

The number of devices decides the layout. The rule is in `dev_grid()` and `hmi_layout_device_tiles()`.

| Devices | Layout |
|---|---|
| 1 to 4 | One large card per device, with the device's own widgets (gauges, tank, pump, buttons). |
| 5 or more, all fit as cards | A grid of cards (2 to 4 columns, 2 to 3 rows), each at least about 380 x 170 px. |
| More than fit as cards | **Overview tiles**: as many as the screen holds, each at least 176 x 88 px. |
| More than fit as tiles | Tiles plus **Previous / Next** pages. |

A tile shows:

- the status dot and the device name;
- the number of the device's alarms, with the shape of the highest priority;
- the name of the main value, and the run state ("Running", "Stopped", "Stop latch", "Offline");
- the main value with its unit, coloured only when it is in alarm.

The whole tile is one touch target. It opens the device's faceplate.

**Alarms on other pages are never hidden.** The page buttons count them, for example
"Next (6 in alarm) >". The header alarm count and the banner always cover every device.

Measured tile counts per page:

| Window | Banner | Tiles per page |
|---|---|---|
| 800 x 480 | no | 12 (4 x 3) |
| 800 x 480 | yes | 8 (4 x 2) |
| 1536 x 889 | yes | 48 (8 x 6) |

## 4. Readings screen

The Readings screen makes every value reachable, whatever the number of devices.

- **Grouped:** each device has a heading row with its status. Its readings follow. A page that
  starts in the middle of a device repeats the heading, marked "continued", and a heading never
  ends a page.
- **Short screens:** when fewer than 6 rows fit, the headings are left out. Each row then reads
  "Device: reading", like the Alarms list, so no row is wasted.
- **Columns:** reading name, value with unit, range bar, and status ("New alarm", "Acknowledged",
  "Cleared, unack.", "Bad data", "No data", "Stale"). Status is blank when all is normal.
- **Filters:** All / In alarm / Bad data. Each filter starts again from page 1.
- **Search:** the Search button opens an on-screen keyboard (4 rows of 10 keys). The list filters
  as each key is pressed. Search matches device names, product names, addresses, reading names and
  units, with or without capitals. A PC keyboard types into it as well.
- **Footer:** "144 of 1074 readings shown", then Previous, the page number and Next.
- **Touch:** a reading or heading opens that device's faceplate.

The list is built once per frame, and only while the Readings screen is open (`ui_build_readings()`
in `app.c`). The renderer draws only the rows of the page shown.

## 5. Touch targets

Sizes come from `layout.h` / `layout.c`. Millimetres are for a 7" 800 x 480 panel (about
133 px per inch, 0.19 mm per pixel).

| Control | Size (px) | About (mm) |
|---|---|---|
| EMERGENCY (header) | 104 x 40 (124 x 40 on wide screens) | 20 x 7.6 |
| Header pills (health, alarms, role, theme) | 40 high | 7.6 high |
| Dock tabs | screen width / 6 x 60 | 25 x 11.4 at 800 px |
| Banner Acknowledge | 140 x 38 | 27 x 7.2 |
| Standard buttons | 44 high | 8.4 high |
| Title-row buttons and filter chips | 36 high | 6.9 high |
| Alarm row Ack | 84 x 38 | 16 x 7.2 |
| Reading rows | 40 high (34 on short screens) | 7.6 (6.5) high |
| Overview tile | at least 176 x 88 | 33.6 x 16.8 |
| Search keyboard keys | (dialog width − 86) / 10 x 34 to 48 | about 12 x 9 at 800 x 480 |
| Faceplate controls | up to 48 high | up to 9.2 high |

Every touch area is 4 px larger on each side than what is drawn (`HMI_LAYOUT_TOUCH_SLOP`).

**Open point:** title-row buttons (36 px) and compact reading rows (34 px) are below 7 mm on a
7" panel. They have not been tried with gloves yet. If they prove too small, the next step is a
larger title row on panels shorter than 600 px.

## 6. Colour

| Colour | Used for |
|---|---|
| Red | EMERGENCY, stop controls, critical alarms, the trip limit on range bars |
| Amber | Warning alarms, the warning limit on range bars, data problems |
| Blue | Information alarms only |
| Green | Normal running (default scheme), or grey with `--color-scheme high-performance` |
| Grey / white | Everything else |

Two themes are available: dark, and a light "outdoor" theme.

## 7. Latency budget

The data-to-screen latency is measured in the running HMI. It runs from the moment a device's
read cycle lands in the fieldbus tag table (`hmi_device_info_t.data_us`) to the end of the
first frame drawn with that data.

| Stage | Bound by design |
|---|---|
| Device poll period | 100 ms (`HMI_MODBUS_POLL_MS`), not part of the measurement |
| Fieldbus to UI snapshot | at most 10 ms (`HMI_PUBLISH_PERIOD_MS`) |
| Snapshot to frame | at most one frame, about 33 ms at 30 frames a second |
| Frame drawing | measured on the Diagnostics screen |

Target: under 100 ms (`HMI_LATENCY_TARGET_MS`). The average and the worst case of the last
second are shown on Diagnostics as "Latency avg/max". The whole-run figures are written to the
`--report` file (`latency_avg_us`, `latency_max_us`, `latency_over_target`). The measurement stops
when the frame has been handed to the platform layer. The time the operating system takes to put
it on the glass is not included.

Measured figures are in `docs/benchmarks/`.

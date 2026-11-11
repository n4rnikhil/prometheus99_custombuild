# Test debt

Features and changes made during the build phase that still need tests. Every item is written
and run in the testing block before release.

## Status after the testing block (2026-11-11)

| Status | Items |
|---|---|
| **Automated test, passing** | 1, 2, 3, 8, 9, 10, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25 (journal size and lookup), 29, 30, 31, 32, 35, 36 (Windows) |
| **Seen in a live run only** (no automated test) | 5, 6, 7 (96 devices listed), 11, 12, 14, 16, 26, 27, 33 (isolated runs), 34 (move seen; chain re-verified after a move: not checked) |
| **Not tested** | 4 (pages past 255), 13 (E key in the search dialog), 28 (Shelved tab), 36 on Linux (no toolchain), 37 (installer not built: no NSIS here), 38 (Python tests not run) |

Results: [evidence/tests.md](evidence/tests.md).

| # | Area | What needs a test | Added in |
|---|---|---|---|
| 1 | Tags | 16-bit tag ids: every API at ids above 255, and the last id (`HMI_MAX_TAGS - 1`) | CP2 |
| 2 | Tags | `HMI_MAX_DEVICES` 128 (3,104 tags): table, alarms and snapshot sizes; the MCU profile still builds with 2 | CP2 |
| 3 | Alarms | `hmi_alarms_sorted` heap sort: same order as the old insertion sort (priority, then time, then tag id), with 0, 1 and 3,000+ active points and with `max` smaller than the count | CP2 |
| 4 | Alarms | Alarm and history paging past 255 pages (`alarm_page` is now 16-bit) | CP2 |
| 5 | Fieldbus | Snapshot published every 10 ms, and at once after an operator command; stale-data mark still appears after 500 ms | CP2 |
| 6 | Latency | `data_us` set when a read cycle completes; latency sample taken once per new cycle; window avg/max; report fields | CP2 |
| 7 | Report | `--report` lists every present device at 100+ devices (one line each, no truncation) | CP2 |
| 8 | Layout | Overview tiles: when tiles replace cards; tiles per page at 800 x 480 with and without the banner, and at 1920 x 1080; no tile below 176 x 88; tile tap opens the faceplate | CP2 |
| 9 | Layout | Readings: rows per page, compact rows below 360 px, footer and page buttons, filter chips and Search button rects match the hit test | CP2 |
| 10 | Layout | Search keyboard: 40 keys inside the dialog at 640 x 400 and larger; key characters; hit test | CP2 |
| 11 | Readings | Row builder: grouping, "continued" headings, no heading at the end of a page, flat mode under 6 rows, filters (All / In alarm / Bad data), search on device and reading names, totals | CP2 |
| 12 | Readings | Tapping a reading or heading opens that device's faceplate; blank rows and HMI rows do nothing | CP2 |
| 13 | Input | Key "E" presses EMERGENCY everywhere except in the search dialog, where it types; the on-screen EMERGENCY button works over the search dialog | CP2 |
| 14 | EMERGENCY | With more devices than rows: devices NOT stopped are listed first; "+ n more" counts the rest correctly | CP2 |
| 15 | Devices | `--max-devices n`: no more than n attached; the rest reported as not shown; out-of-range values refused | CP2 |
| 16 | Process | Page buttons count alarms on other pages correctly | CP2 |
| 17 | Tools | `tools/render_snapshot.c` and every test file updated to the 16-bit tag API and the six screens | CP2 |
| 18 | Alarms | Every state transition (NORMAL, UNACK, ACKED, RTN_UNACK; escalation; return), unchanged from before the rewrite of `alarms.c` | CP3 |
| 19 | Alarms | Limits: 0 and values at or below `min_val` mean "no limit" (e.g. air handler setpoint, min -100) | CP3 |
| 20 | Alarms | Shelve: hidden from summary, counts, banner and horn; still evaluated and journaled; comes back after `HMI_ALARM_SHELVE_MS` (wrap-safe); unshelve early; stop-priority points refused | CP3 |
| 21 | Alarms | Out of service and return to service; a point both shelved and out of service | CP3 |
| 22 | Alarms | Flood: starts above 10 in 10 min, ends below 5; journaled once each; INFO alarms do not sound the horn during a flood; bucket roll-over across minutes and after a long gap | CP3 |
| 23 | Alarms | Chatter: flagged at 3 in 60 s, journaled once, cleared after two quiet windows | CP3 |
| 24 | Alarms | Acknowledge all leaves hidden points alone | CP3 |
| 25 | Journal | 512-record journal: `journal_by_seq` O(1) lookup, overwritten records return NULL, audit log writer loses nothing when 300 alarms come in one evaluation | CP3 |
| 26 | Fieldbus | Alarm evaluation every 10 ms and at once after an operator command | CP3 |
| 27 | UI | Alarm dialog: opens from a row tap (not from the row button); Ack / Shelve / Out of service / Open device; Viewer is asked for the right PIN and the action carries on after sign-in | CP3 |
| 28 | UI | Alarms > Shelved: list, minutes left, Unshelve / Return button, paging | CP3 |
| 29 | Layout | Three alarm tabs and the three title buttons do not overlap at 640, 800 and 1920 px wide | CP3 |
| 30 | Security | New commands `HMI_CMD_SHELVE_ALARM` (Operator) and `HMI_CMD_ALARM_SERVICE` (Maintenance) in the role table and names | CP3 |
| 31 | Identity | After every TCP connect: FC43 asked before any read or write; match → normal polling; other vendor/product, a second exception or a malformed answer → link dropped, comm-loss alarm, one CRITICAL journal line, retried after backoff; regular category refused → basic category asked; devices attached without identity are not checked | CP4 |
| 32 | Identity | No command, heartbeat or reset release reaches a device before its identity matched (write queue refused while not ONLINE) | CP4 |
| 33 | Key | `hal_secure_dir` on Windows (folder created with a protected DACL; existing folder used as is) and Linux (root and user paths, mode 0700); key and vault in it by default; `--key` / `--vault` still override | CP4 |
| 34 | Key | Migration: an old `hmi.key` / `security.vault` next to the exe is copied, protected, then removed, once; not when `--key` / `--vault` are given; the audit chain and vault signature still verify after the move | CP4 |
| 35 | Fixture | `tests/fixtures/device_models.c` gives the same integration results as before (all existing integration checks pass) | CP6 |
| 36 | Build | The HMI, all tests and the release scripts build with no simulator folder present (Windows `build.bat test`, Linux `make test`) | CP6 |
| 37 | Installer | Windows installer and Linux archive contain no simulator files; `hmi.cfg` is the starter config (no factory PINs) | CP6 |
| 38 | Tests | `tests/ui_modal_stall_test.py` with the new arguments, and `tests/modbus_test_device.py` with `test_modbus_live` | CP6 |

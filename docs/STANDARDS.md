# Standards Traceability

Each standard the HMI follows, what it asks for, where the HMI does it, and how that is checked.
**Nothing here is certified.** The HMI is not a safety system: no IEC 61508, IEC 62061 or
ISO 13849 claim is made.

**Verification column:**
- **T** = automated test (suite named);
- **R** = seen in a live run on Windows against simulated devices;
- **D** = by design and code review only;
- **—** = not done.

Test names refer to the suites in `hmi-runtime/tests/`.

---

## ISA-101.01 — Human machine interfaces for process automation

| Topic | Prometheus HMI | Where | Check |
|---|---|---|---|
| Display hierarchy (overview → unit → detail) | Process (tiles / cards) → Readings → faceplate | `micro_renderer.c`, `layout.c` | R |
| Colour used for meaning only | Red: stop and critical alarms; amber: warning; blue: info only; normal values in plain text | `micro_renderer.c` (C_* colours) | R, D |
| Not by colour alone | Priority shapes: diamond, triangle, circle | `sev_icon()` | T (`test_core` layout), R |
| Analog values in context | Range bar with warning and trip ticks on Readings | `range_bar()` | R |
| Touch targets | 34 to 60 px, all listed in [HMI-DESIGN.md](design/HMI-DESIGN.md#5-touch-targets); hit areas 4 px larger | `layout.h` | T (`test_core` layout) for existing screens; new screens: — |
| Navigation always available | Dock with 6 tabs; keys 1 to 6 | `render_dock()` | R |
| Small displays | A single safe screen below 640 x 400 | `render_too_small()` | T |
| Style guide reviewed with operators | — | — | — |

## ISA-18.2 / IEC 62682 — Management of alarm systems

| Clause topic | Prometheus HMI | Where | Check |
|---|---|---|---|
| Alarm states and transitions | NORMAL, UNACK, ACKED, RTN_UNACK; escalation re-annunciates | `alarms.c` `transition()` | T (`test_core`, `test_integration`) |
| Priorities | Info, Warning, Critical, Stop | `config.h` | T |
| Acknowledge | Per alarm and all; the role is checked | `app.c`, `alarms.c` | T, R |
| Shelving (time-limited) | 60 min, comes back by itself, journaled; stop alarms excluded | `hmi_alarms_shelve()` | R (dialog); tests: — |
| Out of service | Maintenance; journaled | `hmi_alarms_set_out_of_service()` | tests: — |
| Suppressed by design | Bad-quality points frozen; one comm-loss alarm per device | `hmi_alarms_evaluate()` | T (`test_integration`) |
| Alarm flood | Above 10 in 10 min starts, below 5 ends; announced; Info alarms silent | `update_rate()` | R (36 alarms); tests: — |
| Chattering alarms | Flagged at 3 in 60 s | `count_annunciation()` | tests: — |
| Alarm rate monitoring | 10-minute rate and its peak; `--report` | `alarms.c`, `app.c` | R |
| Operator guidance | Two steps per alarm from the driver module; checked to fit the banner | `build_guidance()` | T (`test_core`) |
| Audit trail | Every transition in the signed log | `alarm_log.c` | T |
| Alarm philosophy, rationalisation, master database | — (limits live in the driver modules) | — | — |
| Low / low-low limits, on/off delays | — | — | — |

## ISO 13850 / IEC 60204-1 — Emergency stop and stop functions

| Topic | Prometheus HMI | Where | Check |
|---|---|---|---|
| Stop always available | EMERGENCY is always visible, needs no role, and is never covered by a dialog | `layout.c`, `app.c` `ui_emergency_press()` | T (`test_core` layout), R |
| Stop overrides other commands | Queued commands are discarded; new commands refused while latched | `fb_discard_device_commands()` | T (`test_integration`) |
| Latched until reset | The red screen stays until a confirmed Maintenance reset | `ui_emergency_reset()` | T, R |
| Reset does not restart | The reset sends nothing to the machines | `CMD_BIT_EMERG_RESET` handling | T |
| Result shown per machine | Stopped / NOT stopped (offline, no stop control, not on this HMI), not-stopped listed first | `render_emergency()` | T, R (96 devices) |
| Loss of the HMI | The heartbeat stops, so PLCs that watch it go to their safe state | `hmi_devmgr_heartbeat_enable()` | T (`test_integration`) |
| Certified emergency stop device (category 0/1, hardwired) | **Not provided.** EMERGENCY is a software stop over the network | — | — |

## IEC 62443-4-2 — Component security requirements

| Requirement (CR) | Prometheus HMI | Where | Check |
|---|---|---|---|
| CR 1.1 Human user identification and authentication | Role PINs; per-person accounts not supported | `security.c` | T |
| CR 1.5 Authenticator management | Site PINs mandatory; factory PINs refused once commissioned; set only on the console | `security.c`, `app.c` | T |
| CR 1.7 Strength of password-based authentication | 6 to 8 digits; repeated and sequential patterns refused | `security.c` | T |
| CR 1.11 Unsuccessful login attempts | 3 attempts, then 30 s lockout (not kept across a restart) | `security.c` | T |
| CR 2.1 Authorization enforcement | Minimum role per command; PIN on demand | `s_required_role` | T |
| CR 2.5 Session lock | Back to Viewer after 5 min without input | `security.c` | T |
| CR 2.8 / 2.9 Auditable events, audit storage | Every command, refusal and alarm; rotated at 1 MB | `alarm_log.c` | T |
| CR 3.1 Communication integrity | Not possible over Modbus TCP; FC43 identity check after reconnect | `device_manager.c` | R (swap blocked); tests: — |
| CR 3.4 Software and information integrity | Audit log HMAC chain; signed vault | `alarm_log.c`, `security.c` | T |
| CR 3.5 Input validation | Frame checks; setpoint ranges | `modbus_proto.c`, `app.c` | T (`test_modbus_faults`) |
| CR 4.1 Information confidentiality | Key and vault in a folder only the HMI account can open | `hal_secure_dir()` | R (isolated run); tests: — |
| CR 4.3 Use of cryptography | HMAC-SHA256, HKDF-SHA256 (test vectors) | `sha256.c` | T (`test_core`) |
| CR 7.1 Denial of service protection | Bounded queues and buffers; device limit; watchdogs | throughout | T |

## Modbus — Application Protocol v1.1b3, TCP/IP v1.0b

| Topic | Prometheus HMI | Check |
|---|---|---|
| Functions | FC 01, 03, 05, 06 and 43/14 (Read Device Identification) | T |
| Frame checks | MBAP length, protocol id, transaction id, unit, function, byte count | T (`test_modbus_faults`) |
| Not implemented | FC 02, 04, 15, 16 (no module needs them) | — |

The test counts for this build are recorded in `docs/evidence/` after the testing block.

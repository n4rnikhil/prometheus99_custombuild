# Alarm Management

How the Prometheus HMI raises, shows and handles alarms, checked item by item against
ISA-18.2 (IEC 62682). The HMI follows the standard's ideas. It is not certified to it.

> **Verified so far:** the engine compiles. It was run against simulated devices on Windows,
> where a burst of 36 stop alarms started an alarm flood and the alarm dialog opened from the list.
> **Not verified yet:** automated tests for shelving, out of service, flood hysteresis, chatter and
> the journal at 512 records (see [Known limitations](../KNOWN-LIMITATIONS.md)).

Code: `hmi-runtime/src/core/alarms.c` (engine, fieldbus thread), `src/app/app.c` (operator
actions), `src/renderer/micro_renderer.c` (Alarms screen, banner, alarm dialog).

---

## 1. Alarm life cycle

```
            condition                 acknowledge
 NORMAL ───────────────▶ UNACK ─────────────────▶ ACKED
   ▲                       │  ▲                     │
   │ acknowledge           │  │ condition returns   │ condition clears
   │                       ▼  │                     ▼
   └──────────────────── RTN_UNACK                NORMAL
                         (cleared, still to acknowledge)

 A higher priority while UNACK or ACKED: back to UNACK, and the horn sounds again.
```

| ISA-18.2 item | Prometheus HMI | Status |
|---|---|---|
| Alarm appears | Analog: at or above the high (warning) or high-high (critical) limit. Digital: the module's alarm priority while the input is on. | Implemented |
| Alarm clears | Analog clears after a deadband of 1 % of the span, so a value on the limit does not flicker. | Implemented |
| Return to normal | Cleared but not acknowledged: stays in the list as "Cleared" (latched) until acknowledged. | Implemented |
| Acknowledge | Per alarm (row button or alarm dialog) or all at once. Operator role. A Viewer is asked for the PIN. | Implemented |
| Priorities | Info, Warning, Critical, Stop. Each has its own shape (circle, triangle, diamond) as well as its own colour. | Implemented |
| Sorting | Priority first, then unacknowledged before acknowledged, then newest first. | Implemented |
| Horn | Sounds while any shown alarm needs acknowledging. Silence works without acknowledging, and a new alarm sounds the horn again. | Implemented |
| Guidance | Every alarm carries two operator steps from its driver module, shown in the banner and the alarm dialog. | Implemented |
| Shelving | An operator shelves one alarm for 60 minutes. It comes back by itself, and can be unshelved earlier. Every shelve and unshelve is journaled. | Implemented |
| Out of service | Maintenance takes one alarm out of service, for example a broken sensor, until it is returned. Journaled. | Implemented |
| Suppressed by design | While a reading has bad data, its alarm state is frozen. The device's single "comm loss" alarm is raised instead, so one lost device gives one alarm, not one per reading. | Implemented |
| Stop alarms | Stop-priority alarms (stop latches) can never be shelved or taken out of service. | Implemented |
| Alarm flood | More than 10 new alarms in 10 minutes starts a flood; fewer than 5 ends it. Both are journaled. During a flood, the Alarms screen says so in orange and Info alarms do not sound the horn. | Implemented |
| Chattering | An alarm that comes in 3 or more times within a minute is flagged and journaled. The flag clears after two quiet minutes. | Implemented |
| Alarm rate | New alarms in the last 10 minutes, and the peak, on the Alarms screen and in `--report`. | Implemented |
| Audit trail | Every alarm event goes to the signed audit log (`alarms.log`, HMAC chain). The in-memory journal holds 512 records, so a flood cannot overrun it between two writes. | Implemented |
| Low / low-low limits | The driver modules define high limits only. | **Not implemented** (needs low limits in the module format) |
| On-delay / off-delay timers | No per-alarm delay. Chatter is flagged and can be shelved instead. | **Not implemented** |
| Alarm rationalisation database | Limits and texts live in the driver modules (const data); there is no separate master alarm database. | Not applicable at this scale |
| Persisting shelves across a restart | Shelves and out-of-service marks are in memory and are lost on restart. | **Not implemented** |

## 2. Operator screens

- **Banner:** the highest-priority shown alarm, with its two guidance steps and Acknowledge.
  It stays on every screen except Alarms.
- **Alarms > Active:** the sorted list. The card title shows "n active – n unacknowledged – n new
  in 10 min", or "ALARM FLOOD – n new in 10 min – work from the top". A row's button acknowledges
  that alarm. A tap anywhere else on the row opens the alarm dialog.
- **Alarms > Shelved:** every shelved alarm (with the minutes left) and every out-of-service
  alarm, each with Unshelve or Return.
- **Alarms > History:** the newest journal events first.
- **Alarm dialog:** name, priority, state and time, the value now and its limits, the two
  guidance steps, a note (shelved, out of service, chattering, or "a stop alarm"), and four
  buttons: Acknowledge, Shelve for 60 min / Unshelve, Out of service / Return to service, Open device.

## 3. Roles

| Action | Role needed |
|---|---|
| See alarms | anyone |
| Acknowledge, silence the horn | Operator |
| Shelve, unshelve | Operator |
| Out of service, return to service | Maintenance |
| Reset a stop latch | Operator, after a confirmation |

A role that is too low is asked for the PIN of the role needed. The action then carries on.

## 4. Settings (`hmi-runtime/include/config.h`)

| Setting | Value | Meaning |
|---|---|---|
| `HMI_ALARM_EVAL_MS` | 10 | how often alarms are evaluated |
| `HMI_ALARM_SHELVE_MS` | 3,600,000 | how long a shelve lasts (1 hour) |
| `HMI_ALARM_FLOOD_ON` / `_OFF` | 10 / 5 | flood starts above / ends below, per 10 minutes |
| `HMI_ALARM_CHATTER_COUNT` / `_MS` | 3 / 60,000 | chattering: this many times within this window |
| `HMI_ALARM_JOURNAL_SIZE` | 512 | in-memory journal records |

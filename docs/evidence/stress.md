# Stress run — Build-19

**Partial: 36 minutes of a planned 62.** The run was stopped at minute 36 to free the PC for the
presentation. The one-hour requirement is **not met**.

| | |
|---|---|
| Date | 2026-11-11, Windows 11, AMD Ryzen 7 7735HS |
| Build | release flags; the build before the two security fixes and the 640 px layout fix (renderer and folder code only) |
| Load | 96 simulated Modbus TCP devices (12 groups of 8, eight device types), 1,074 readings |
| Faults injected | every 5 min one group of 8 devices unplugged for 60 s, then restored (minutes 5, 10, 20, 25, 35); every 15 min one group replaced by **different** devices (identity swap, minutes 15 and 30) |
| Sampled | the HMI's memory, handles, threads and CPU time every minute |

## Results

| Measure | Minute 1 | Minute 36 |
|---|---|---|
| Private memory | 3,174,400 B | 3,158,016 B |
| Working set | 2,097,152 B | 2,392,064 B |
| Handles | 291 | 289 (range 279–319 during the run) |
| CPU time used | 13.3 s | 547.1 s (about 25 % of one core on average) |

- **No crash.** The HMI ran the whole 36 minutes.
- **No memory growth.** Private memory stayed flat (no heap is used).
- **Handles returned to their level** after every unplug and swap.
- **Audit log:** 552 records, `--verify-log` → **VERIFIED** (chain intact).
- Every unplugged group came back by itself. The swapped groups were blocked by the identity
  check, then replaced by the new devices.

## Not measured in this run

- The full hour, and the HMI's own `--report` file (the process was stopped, so no report was
  written).
- Latency figures for this run. The 120-second run in [benchmarks](../benchmarks/README.md) has
  them.

Raw data: `samples.csv` from the run (kept on the build PC, not in the repository).

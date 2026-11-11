# Design decisions (ADRs)

Each record says what was decided, why, what else was considered, and what it costs.

| # | Decision | Status |
|---|---|---|
| [0001](0001-custom-band-renderer.md) | A purpose-built band renderer instead of a graphics library such as LVGL | accepted |
| [0002](0002-lock-free-snapshot.md) | Threads hand data over through a seqlock snapshot and one-way rings, not mutexes | accepted |
| [0003](0003-static-memory.md) | No heap: every table is sized at build time (128 devices, 16-bit tag ids) | accepted |
| [0004](0004-emergency-software-stop.md) | EMERGENCY is a latched software stop that anyone can press | accepted |
| [0005](0005-identity-check-on-reconnect.md) | Re-check every device's identity after each connect | accepted |
| [0006](0006-protected-key-folder.md) | Keep the device key and PIN vault in a protected system folder | accepted |
| [0007](0007-driver-modules-as-data.md) | Device knowledge lives in constant-data driver modules | accepted |

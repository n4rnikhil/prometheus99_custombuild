# Platforms — Build-19

What was built and run for this build, on which platform, and what was not.

| Platform | Built | Tests run | App run | Notes |
|---|---|---|---|---|
| **Windows 11 (x86-64)** | yes: release flags (`-Os`, static, hardened), `-Werror` | all suites (see [tests.md](tests.md)) | yes: 3, 60 and 96 devices; the one-hour stress run ([stress.md](stress.md)) | AMD Ryzen 7 7735HS, MinGW-w64 GCC 16.1.0 |
| Harmony P6 (Windows 10 IoT) | same executable | — | **not run** | Expected to run the Windows build; needs a run on the panel |
| Linux PC (x86-64) | **no** | **no** | **no** | No Linux toolchain on this PC (no WSL), and nothing may be installed. Static check only: `cppcheck` (C99, warning/performance/portability) over all of `src/`, including `src/hal/*_posix.c` and `src/platform/linux/`, found nothing. The portable code (app, core, devices, fieldbus, renderer) is the same code compiled for Windows with `-Werror` |
| Raspberry Pi (arm64 / armhf) | **no** | **no** | **no** | Same reason. Earlier builds were cross-built and tested under qemu; this one was not |
| ESP32-S3 | **no** | **no** | **no** | Never compiled (needs ESP-IDF) |
| **Harmony ST6 (HMIST6400, HMIST6600)** | not possible | — | — | **The ST6 cannot run this runtime.** It runs Schneider's own runtime (EcoStruxure Operator Terminal Expert) and offers no public way to run a third-party native program. See [LAB_DEPLOYMENT.md](../LAB_DEPLOYMENT.md) |

## Linux changes in this build that have not been compiled on Linux

- `hal_secure_dir()` POSIX branch (`src/hal/fs_stdio.c`): `mkdir`, `lstat`, owner and mode checks.
- Everything else Linux-specific is unchanged from Build-18.

**To close:** on a Linux PC, run `cd hmi-runtime && make test` and `make industrial_hmi`, then the
cross builds from the Makefile header. Then follow the [site test checklist](../SITE-TEST-CHECKLIST.md).

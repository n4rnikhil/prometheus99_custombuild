# Platforms — Build-19

What was built and run for this build, on which platform, and what was not.

| Platform | Built | Tests run | App run | Notes |
|---|---|---|---|---|
| **Windows 11 (x86-64)** | yes: release flags (`-Os`, static, hardened), `-Werror` | all suites (see [tests.md](tests.md)) | yes: 3, 60 and 96 devices; the one-hour stress run ([stress.md](stress.md)) | AMD Ryzen 7 7735HS, MinGW-w64 GCC 16.1.0 |
| Harmony P6 (Windows 10 IoT) | same executable | — | **not run** | Expected to run the Windows build; needs a run on the panel |
| Linux PC (x86-64) | yes: Ubuntu 24.04 (WSL 2), GCC 13.3, `-Werror` | all suites under AddressSanitizer + UBSan: `test_core` 1,354,597, MCU profile 10,600, `test_integration` 816, `test_modbus_faults` 83: all passed | yes: desktop window on a virtual 800 x 480 screen (Xvfb), 8 simulated devices: tiles, device view, EMERGENCY; fieldbus loop 1,000 /s, data-to-screen 22.7 ms average, 0 CRC errors | Framebuffer + touch (evdev) mode not run: WSL has no framebuffer device |
| Raspberry Pi 64-bit (arm64) | yes: cross-built, aarch64-linux-gnu-gcc 13.3 | all suites in `qemu-aarch64` (no sanitizers), same counts: all passed | the arm64 program in `qemu-aarch64`, as a desktop window on a virtual 800 x 480 screen, 8 simulated devices: fieldbus loop 1,001 /s, 0 CRC errors | **Not run on a Raspberry Pi** or its 7" touch display |
| Raspberry Pi 32-bit (armhf) | yes: cross-built, arm-linux-gnueabihf-gcc 13.3 | all suites in `qemu-arm`, same counts: all passed | not run | Not run on a Raspberry Pi |
| ESP32-S3 | yes: ESP-IDF 5.3.2, `-Werror`. 520 KB of the 3 MB app partition; 147 KB of the 342 KB internal RAM | the portable core is the MCU-profile suite above | in Espressif's QEMU (it has no PSRAM, display or Wi-Fi): boots, mounts its storage, gets an address on the emulated Ethernet, attaches 2 simulated devices and polls them for 60 s. Its report: 2 devices, 0 CRC errors, 7 audit records written, 0 lost. Without PSRAM the display does not start and the firmware says so and keeps running | **Not run on a board**: display, touch, backlight power-up, Wi-Fi, PSRAM and speed are unverified |
| **Harmony ST6 (HMIST6400, HMIST6600)** | not possible | — | — | **The ST6 cannot run this runtime.** It runs Schneider's own runtime (EcoStruxure Operator Terminal Expert) and offers no public way to run a third-party native program. See [LAB_DEPLOYMENT.md](../LAB_DEPLOYMENT.md) |

## How these were run

On the same PC, in Ubuntu 24.04 under WSL 2: `make test` and `make industrial_hmi`; the
Raspberry Pi builds with `make CC=aarch64-linux-gnu-gcc` / `CC=arm-linux-gnueabihf-gcc` in copies
of the tree, their tests in `qemu-aarch64` / `qemu-arm`; the window on `Xvfb` driven by `xdotool`.
ESP32: `idf.py build` in `ports/esp32`, and the emulator build described in its README.
Then follow the [site test checklist](../SITE-TEST-CHECKLIST.md) on the real hardware.

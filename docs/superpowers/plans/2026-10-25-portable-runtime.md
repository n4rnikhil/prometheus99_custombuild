# Portable HMI Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline). Steps use checkbox syntax.

**Goal:** One C99 code base that runs as a lightweight HMI runtime on Windows, Linux (PC and Raspberry Pi) and small MCUs (ESP32 class), with no behaviour change on Windows.

**Architecture:** Everything OS-specific moves behind a small HAL in `include/hal/` (atomics, threads, sockets, files, system, display/input). The portable core (`src/core`, `src/renderer`, `src/drivers`, new `src/app.c`) never includes an OS header. Each platform adds one shell (`src/platform/<os>/`) that owns the window or framebuffer, translates input into `app_*` calls and presents bands. The renderer keeps its single 19,200-byte band buffer, so RAM stays flat across targets.

**Tech Stack:** C99, GCC/MinGW (Windows, verified here), POSIX sockets/pthreads (Linux, ESP-IDF via lwIP + pthread), Linux fbdev + evdev (no X, no GUI library).

**Spec:** the project README (the spec file was removed)

## Global Constraints

- Zero dynamic allocation in application code; all state static.
- `-std=c99 -Wall -Wextra -Wshadow -Werror` clean on every file; cppcheck clean.
- Lock-free between the UI and fieldbus threads (seqlock, SPSC rings, atomic bit words); no mutexes.
- Portable modules stay OS-free: no `<windows.h>`, `<winsock2.h>`, `<sys/socket.h>` outside `src/hal/` and `src/platform/`.
- `build.bat test` must stay green after every task (4407 unit checks, 74 fault-injection checks, live discovery test).
- Do not commit or push (user commits).

## Review Focus

- 32-bit targets: no `long`/`size_t` size assumptions in shared code (ESP32 is 32-bit, `long` = 4 bytes).
- Unaligned access: Modbus frame parsing must use byte reads (Xtensa/ARM fault on unaligned 16/32-bit loads).
- POSIX socket semantics: `EINTR`, `EAGAIN`, `EINPROGRESS`, partial `send()`, `SIGPIPE` must not kill or stall the 1 kHz loop.
- Power loss during log/vault writes: append-only log keeps its torn-tail recovery; vault replace stays atomic.
- Small task stacks (ESP32: 4-8 KB): thread entry points keep large objects in statics.
- Tick wrap: all time compares stay `(uint32_t)(now - then)`.

---

### Task 1: Atomics HAL

**Files:** Create `include/hal/atomic.h`; Modify `src/main.c`; Test `tests/test_core.c`
**Interfaces:** Produces `typedef volatile int32_t hal_atomic_t;` and `int32_t hal_atomic_load(const hal_atomic_t*)`, `void hal_atomic_store(hal_atomic_t*, int32_t)`, `int32_t hal_atomic_exchange(hal_atomic_t*, int32_t)`, `int32_t hal_atomic_or(hal_atomic_t*, int32_t)` (returns old), `int32_t hal_atomic_add(hal_atomic_t*, int32_t)` (returns new), all sequentially consistent.
- [ ] Add a unit test: store/load/exchange/or/add return values.
- [ ] Implement with `__atomic_*` builtins (GCC/Clang), `Interlocked*` for MSVC.
- [ ] Replace every `LONG`/`Interlocked*` in `main.c` (heartbeats, command bits, rings, `g_running`).
- [ ] Run `build.bat test`; expect all pass.

### Task 2: Network HAL

**Files:** Create `include/hal/net.h`, `src/hal/net_win32.c`, `src/hal/net_posix.c`; Modify `src/drivers/device_manager.c`, `src/drivers/discovery.c`, `build.bat`
**Interfaces:** Produces `hal_sock_t` (opaque handle), `HAL_SOCK_INVALID`, `bool hal_net_init(void)`, `void hal_net_shutdown(void)`, `hal_sock_t hal_tcp_connect_start(uint32_t ip_host_order, uint16_t port)` (non-blocking, TCP_NODELAY, returns invalid on immediate failure), `int hal_tcp_connect_poll(hal_sock_t)` (1 connected, 0 pending, -1 failed), `int hal_tcp_send(hal_sock_t, const void*, size_t)` (bytes sent, -1 error), `int hal_tcp_recv(hal_sock_t, void*, size_t)` (bytes, 0 would-block, -1 closed/error), `void hal_tcp_close(hal_sock_t)`, `int hal_net_wait(const hal_sock_t *rd, int nrd, const hal_sock_t *wr, int nwr, uint32_t timeout_ms, uint32_t *rd_ready, uint32_t *wr_ready)` (bitmasks, up to 32 sockets), `bool hal_ip_parse(const char*, uint32_t *ip)`, `void hal_ip_format(uint32_t ip, char out[16])`.
- [ ] Move all socket calls out of the two driver files behind these functions; drivers hold `hal_sock_t`, never `SOCKET`/`int`.
- [ ] POSIX impl: `fcntl(O_NONBLOCK)`, `MSG_NOSIGNAL`, `select`/`poll`, retry on `EINTR`, map `EAGAIN/EINPROGRESS` to "pending".
- [ ] Run `build.bat test`; the live discovery + fault tests must pass unchanged.

### Task 3: Files, time and system HAL

**Files:** Create `include/hal/fs.h`, `include/hal/sys.h`, `src/hal/fs_stdio.c`, `src/hal/sys_win32.c`, `src/hal/sys_posix.c`; Modify `src/core/alarm_log.c`, `src/drivers/driver_host.c`, `src/main.c`, `tests/test_core.c`
**Interfaces:** Produces `hal_file_t`, `hal_file_t *hal_file_open(const char *path, hal_mode_t)` (`HAL_READ`, `HAL_APPEND`, `HAL_WRITE_NEW`), `int hal_file_read(hal_file_t*, void*, size_t)`, `bool hal_file_write(hal_file_t*, const void*, size_t)`, `bool hal_file_sync(hal_file_t*)` (flush + fsync/FlushFileBuffers), `bool hal_file_seek_end(hal_file_t*, int32_t back)`, `uint32_t hal_file_size(hal_file_t*)`, `void hal_file_close(hal_file_t*)`, `bool hal_file_replace(const char *tmp, const char *dst)` (atomic where the OS allows), `void hal_utc_now(char *out, size_t cap)` (ISO-8601 with ms), `bool hal_random(void *out, size_t n)` (OS CSPRNG), `void hal_exe_dir(char *out, size_t cap)`, `bool hal_host_stats(hal_host_stats_t *out)` (cpu %, ram %, process KB; false fields = unsupported).
- [ ] `hal_file_t` is a small static pool (4 handles), no heap.
- [ ] Port `alarm_log.c` (open/recover/rotate/verify), the vault load/save in `main.c`, and `driver_host.c` to these calls.
- [ ] Replace Win32 file calls in `tests/test_core.c` tamper tests with `hal_file_*`.
- [ ] Run `build.bat test`; log verify, tamper detection and torn-tail tests must pass.

### Task 4: Thread HAL and portable app core

**Files:** Create `include/hal/thread.h`, `src/hal/thread_win32.c`, `src/hal/thread_posix.c`, `include/app.h`, `src/app.c`, `src/platform/win32/main_win32.c`; Delete the portable parts out of `src/main.c`
**Interfaces:** Produces `hal_thread_t`, `bool hal_thread_start(hal_thread_t*, void (*fn)(void*), void *arg, hal_prio_t)`, `void hal_thread_join(hal_thread_t*, uint32_t timeout_ms)`; `app.h`: `int app_init(int argc, char **argv)` (-1 = run, else exit code), `void app_start_workers(void)`, `void app_frame(uint32_t now_ms)`, `void app_pointer(hmi_ptr_event_t, int x, int y, uint32_t now_ms)`, `void app_key(uint16_t key, bool shift, uint32_t now_ms)`, `void app_render(void (*present)(const uint8_t *band, int y, int rows, int w, const uint32_t *pal, void *ctx), void *ctx)`, `void app_resize(int w, int h)`, `bool app_running(void)`, `void app_stop(void)`, `void app_shutdown(void)`, `bool app_beep_due(void)`.
- [ ] Key codes: ASCII for letters/digits, `HMI_K_TAB/LEFT/RIGHT/ENTER/ESC/BACK/SPACE` for the rest; backends map numpad to digits.
- [ ] `src/platform/win32/main_win32.c` keeps WinMain, WndProc, EcoQoS opt-out, crash filter, console printing, modal-loop timer; everything else lives in `app.c`.
- [ ] Run `build.bat test`, then `measure.bat`; expect ~1 kHz loop, ~30 fps, same memory as before.

### Task 5: Linux platform (Raspberry Pi, PC)

**Files:** Create `src/platform/linux/main_linux.c`, `src/platform/linux/fb_display.c`, `src/platform/linux/evdev_input.c`, `Makefile`, `build.sh`
**Interfaces:** Consumes `app.h`. Produces a `industrial_hmi` Linux binary: `/dev/fb0` (16/32 bpp, scales from the 4-bpp bands through the palette), evdev touch/mouse/keyboard, `--fb`, `--input` options, SIGINT/SIGTERM clean shutdown, `--headless` (no display, for servers and CI).
- [ ] Present path converts one band at a time (no full-frame buffer).
- [ ] Syntax-check POSIX code here with stub headers (`-fsyntax-only`), since no Linux compiler is available; mark as "not run on hardware" in README.
- [ ] Makefile: `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections`, static link.

### Task 6: Small-MCU profile (ESP32 class)

**Files:** Modify `include/config.h`; Create `include/profile_mcu.h`, `ports/esp32/CMakeLists.txt`, `ports/esp32/main/hmi_main.c`, `docs/PORTING.md`
**Interfaces:** Produces compile-time switches `HMI_FEATURE_DISCOVERY` (0 on MCU), `HMI_MAX_DEVICES` (override), per-profile list of compiled-in driver modules (`HMI_DRIVER_TANK`, `HMI_DRIVER_DRIVE`, `HMI_DRIVER_GENERIC`), so a build only contains the drivers it needs.
- [ ] Measure RAM/flash per profile and publish only measured numbers in README.
- [ ] ESP-IDF component skeleton: lwIP sockets use `net_posix.c`, pthread use `thread_posix.c`, SPI LCD hooked via `app_render` present callback.

### Task 7: Docs and verification

- [ ] Update `README.md` (portability layers, per-target build commands, measured footprint).
- [ ] Final: `build.bat test`, `build.bat snapshot`, cppcheck, `measure.bat`; report which targets are verified on hardware (Windows) versus syntax-checked only (Linux, ESP32).

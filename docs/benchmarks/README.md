# Benchmarks

Every figure on this page was measured on one machine, on 2026-11-11, with the commands given.
Nothing here was measured on a panel PC, a Harmony P6, or a Raspberry Pi.

| | |
|---|---|
| Machine | AMD Ryzen 7 7735HS, 15 GB RAM, Windows 11 Home (build 26300) |
| Compiler | MinGW-w64 GCC 16.1.0 (x86-64, UCRT) |
| HMI build | release flags of `hmi-runtime/build.bat` (`-Os`, static, hardened) |
| Devices | simulated Modbus TCP devices on the same PC (eight device types, mixed) |

---

## 1. The running HMI

The HMI ran with `--report` and wrote its own measurements on exit. Memory, threads and CPU
were read with PowerShell `Get-Process` while it ran. Start-up is the time from starting the
process until its window accepts input (`WaitForInputIdle`).

| Measure | Baseline (Build-18, 3 devices) | Build-19, 3 devices | Build-19, 96 devices |
|---|---|---|---|
| Readings shown | not counted | not counted | **1,074** |
| Run length | 20 s | 20 s | 120 s |
| Executable size | 359,936 B | **252,928 B** | 252,928 B |
| Start-up to input-ready (warm) | 110 ms | 239 ms | 297 ms |
| Working set | 1.2 MB | 2.2 MB | 2.4 MB |
| Private memory | 2.08 MB | 3.1 MB | 3.3 MB |
| Threads | 7 | 7 | 9 |
| CPU (share of one core) | not measured | 13 % | 19 % |
| Fieldbus loop rate | 1,000 /s | 1,001 /s | 1,001 /s |
| Fieldbus period avg / max | 1,000 / 1,693 µs | 1,000 / 2,511 µs | 1,000 / 2,392 µs |
| Late fieldbus cycles (> 2 ms) | 44 of 20,045 | 17 of 19,630 | 549 of 117,275 (0.47 %) |
| UI frame rate | 30 /s | 31 /s | 31 /s |
| Longest gap between two frames | 44 ms | 109 ms | 106 ms |
| Data-to-screen latency, average | no probe | 24.3 ms | **24.5 ms** |
| Data-to-screen latency, worst | no probe | 98.2 ms | 138.6 ms |
| Latency samples over 100 ms | no probe | 0 of 583 | 182 of 107,514 (0.17 %) |
| Audit records lost | 0 | 0 | 0 |
| Snapshot CRC errors | 0 | 0 | 0 |

**What this shows.**
- With 96 devices and 1,074 readings, the fieldbus loop still ran 1,000 times a second.
- Data reached the screen in about 25 ms on average, against a target of 100 ms.
- The whole program used about 3.3 MB of private memory.

**What is not explained yet.**
- The longest gap between frames grew from 44 ms to about 106 ms, and 0.17 % of latency samples
  were over 100 ms (worst 138.6 ms). The cause has not been analysed. Candidates are the start-up
  period (devices being attached) and the audit-log flush, which runs on the UI thread.
- Start-up went from 110 ms to 239 ms. It has not been analysed either.
- The first start of a newly built executable took about 3 s. Later starts took 239 to 297 ms.
  The first-start delay is most likely Windows checking a file it has not seen before. That was
  not verified.

**Why the executable got smaller.** On Windows, `-fdata-sections` placed the zero-filled tables
(about 1 MB at 128 devices) inside the executable file. The release build no longer uses it on
Windows, so those tables only take memory when the program runs.

Reproduce (Windows PowerShell): start the simulated devices, then

```
industrial_hmi.exe --config bench.cfg --no-sound --key k --log a.log --vault v --report report.txt --exit-after-ms 120000
```

with `bench.cfg` holding `--scan 127.0.0.1:2000-2119` and `--allow-factory-pins`.

### Memory after the device-bound copies (later the same day)

The loops and snapshot copies were changed to touch only the device places and readings in use.
Task Manager's memory column (private working set), read 25 to 39 s after start:

| Devices | Before the change | After |
|---|---|---|
| 3 | 1,320 KB | **588 KB** |
| 24 | not measured | 736 KB |
| 48 | not measured | about 940 KB (788 to 944 KB) |
| 96 | 1,396 KB | 1,252 KB |

Private bytes (commit) stay about 3.1 MB: the tables for 128 devices are still reserved, only no
longer touched. 0 CRC errors and 0 audit records lost in these runs; data-to-screen latency average
23 ms at 3 and at 96 devices.

### 120 devices, and memory while a sound plays (later the same day)

Executable 259,584 B (tile charts, EMERGENCY sound added). 120 simulated Modbus TCP devices of
eight types ran on the same PC (ports 1502-1629) and the HMI ran for 90 s with `--report`:

| Measure | 120 devices |
|---|---|
| Devices online | 120 of 120, no timeouts |
| Task Manager memory, 25 / 32 / 39 s after start | 1,224 / 1,336 / 1,348 KB |
| Fieldbus loop rate | 899 /s (target 1,000) |
| Fieldbus period max | 102.6 ms (92 late cycles of 89,825) |
| Data-to-screen latency, average / worst | 25.2 ms / 230.4 ms |
| Latency samples over 100 ms | 20 of 102,591 (0.02 %) |
| UI frame rate / longest frame | 30 /s / 121 ms |
| Snapshot CRC errors, audit records lost | 0, 0 |

The fieldbus loop fell below 1,000 /s and had one stall of about 100 ms; the cause has not been
analysed (start-up, while 120 devices were being attached, is the likely candidate).

Memory while the EMERGENCY sound plays (3 devices, Task Manager memory, EMERGENCY pressed at 28 s):

| Sound | Before | 4 s after | 10 s after |
|---|---|---|---|
| Built-in EMERGENCY siren | 500 KB | 1,804 KB | 744 KB |
| `alarm.mp3` (MP3 through Windows' decoders) | 584 KB | 1,272 KB | 856 KB |

Playing a sound loads Windows' audio engine (and, for the MP3, its media decoders). The HMI's memory
trim (every 10 s) brings the figure back under 1 MB while the sound plays. `alarm.mp3` for alarms
uses the same path as the MP3 row. Memory after the sound stops was not measured. Both sounds were
confirmed audible by reading the process's output level from the Windows audio mixer (siren peak
0.79, `alarm.mp3` peak 0.81 of full scale).

## 2. The renderer alone

`hmi-runtime/tools/render_bench.c` draws whole frames of one screen with no window and no
network, and times each frame. Built with `-O2`. 300 frames after 10 warm-up frames.

Measured again after the tiles got their 60 s charts, with a full 60 s trend in every device:

| Screen | Size | Devices | Average | Fastest | Slowest |
|---|---|---|---|---|---|
| Process (cards) | 800 x 480 | 4 | 0.20 ms | 0.18 ms | 0.53 ms |
| Process (tiles) | 800 x 480 | 12 | 0.38 ms | 0.33 ms | 0.95 ms |
| Process (tiles, page 1) | 800 x 480 | 96 | 0.40 ms | 0.34 ms | 1.22 ms |
| Readings | 800 x 480 | 96 | 0.22 ms | 0.18 ms | 0.48 ms |
| Alarms | 800 x 480 | 96 | 0.28 ms | 0.23 ms | 0.62 ms |
| Process (cards) | 1920 x 1080 | 4 | 1.63 ms | 1.37 ms | 2.66 ms |
| Process (tiles, with charts) | 1920 x 1080 | 10 | 1.27 ms | 1.09 ms | 2.26 ms |
| Process (tiles) | 1920 x 1080 | 96 | 2.57 ms | 2.18 ms | 4.78 ms |

Before the charts (no trend data in the benchmark): 12 tiles at 800 x 480 took 0.33 ms, 96 tiles at
1920 x 1080 took 1.75 ms.

Renderer memory: 20,328 bytes (`sizeof(hmi_renderer_t)`), of which the band buffer is 19,200
bytes. It is the same at every screen size, because the screen is drawn in bands.

```
gcc -std=c99 -O2 -I include tools/render_bench.c src/core/tags.c src/core/alarms.c src/core/security.c ^
    src/core/sha256.c src/core/crc32.c src/core/watchdog.c src/renderer/layout.c ^
    src/renderer/micro_renderer.c src/renderer/font_inter.c src/devices/*.c src/hal/timer_win32.c -lwinmm
render_bench 800 480 12 0 300
```

## 3. Compared with LVGL, on the same machine

[LVGL](https://lvgl.io) is a widely used open-source graphics library for embedded screens.
LVGL v9.2.2 (git tag `v9.2.2`) was built from source with the same compiler and `-O2`. It used
its default configuration (`lv_conf_template.h`), with four changes: the configuration switched
on, the Montserrat 20 and 28 fonts added, and the examples left out. LVGL was downloaded into a
scratch folder only and is not part of this repository.

`docs/benchmarks/lvgl/lvgl_bench.c` builds the same kind of screen as the HMI's Process screen
with 12 devices, at 800 x 480:
- a header with the title, three status pills and an EMERGENCY button;
- 12 tiles, each with a name, a status dot, a caption, a run state and a value;
- a dock with six tabs.

It redraws the whole screen every frame (`lv_obj_invalidate` + `lv_refr_now`) into a partial
draw buffer, and a flush function reads the pixels as a display driver would.

| | Prometheus renderer | LVGL 9.2.2 (buffer 1/10 screen) | LVGL 9.2.2 (buffer = 19,200 B) |
|---|---|---|---|
| Full 800 x 480 frame, average | **0.33 ms** (0.38 ms with tile charts) | 0.97 ms | 1.73 ms |
| Fastest / slowest frame | 0.26 / 1.01 ms | 0.85 / 1.88 ms | 1.44 / 2.88 ms |
| Draw buffer | 19,200 B | 76,800 B | 19,200 B |
| Other renderer RAM | 1,128 B | 41,528 B of heap used (of a 57,584 B pool) | 41,600 B of heap used |
| Code and constant data linked (minus an empty program) | 170 KB* | 557 KB** | 557 KB** |

\* Includes more than the renderer: the tag table, the alarm engine, security and the 11 device
modules that the benchmark links. \*\* LVGL as linked by this screen, with three Montserrat fonts.

**Read these numbers with care.** The two do not do the same work:
- The Prometheus renderer draws with 16 colours (4 bits per pixel). Anti-aliasing goes through a
  colour lookup table. LVGL draws in 16-bit colour (65,536 colours) with full alpha blending.
- The fonts differ: Inter in four sizes against Montserrat in three.
- LVGL keeps an object tree with styles, layouts, events, scrolling and animation. Prometheus
  draws straight from a view structure each frame and has none of that.
- The screens look alike, but are not identical pixel for pixel.

So these numbers support one fair statement:

> For this kind of fixed operator screen, on this PC, the purpose-built renderer drew a full frame
> about 3 times faster than LVGL with LVGL's recommended buffer, and about 5 times faster with
> the same buffer size. It used about 20 KB of RAM against about 60 to 118 KB.

They do not show that it is a better general-purpose graphics library. It is not one.

## 4. Not measured

- Any embedded or panel hardware (Harmony P6, Raspberry Pi, ESP32).
- Long runs: the one-hour stress run is part of the testing block (`docs/evidence/stress.md`).
- The time the operating system needs to put a finished frame on the glass. The latency figure
  stops when the frame has been handed to the platform layer.

# ADR-0001: A purpose-built band renderer instead of a graphics library

**Status:** accepted

## Context
The HMI must run on a panel PC, a Raspberry Pi and, one day, a microcontroller with a few
hundred KB of RAM. It shows fixed operator screens (tiles, lists, faceplates, dialogs), and must
stay responsive however many devices are attached. It must also be portable C99 with no heap.

## Decision
Draw every screen with our own renderer (`src/renderer/`):
- a 4-bits-per-pixel band buffer of 19,200 bytes, so no full-screen frame buffer;
- 16-colour themes, with anti-aliasing through a colour lookup table;
- one geometry file (`layout.c`) shared by drawing and touch;
- screens drawn straight from a view structure built once per frame.

## Options considered
- **LVGL** (open source, widely used on embedded screens). It has far more widgets, animation,
  16/32-bit colour and a large community. It keeps an object tree and needs a heap pool and a
  larger draw buffer.
- **A platform toolkit** (GDI, GTK, Qt). It does not reach microcontrollers or a bare
  framebuffer, and brings large dependencies.

## Consequences
- **Measured on the same PC** ([benchmarks](../benchmarks/README.md)): a full 800 x 480 frame of
  12 tiles took 0.33 ms, against 0.97 ms for LVGL 9.2.2 with its recommended buffer and 1.73 ms
  with the same buffer size. RAM was about 20 KB against 60 to 118 KB. LVGL draws in 16-bit colour
  with full blending, so the work is not identical.
- RAM does not grow with screen size.
- **The cost:** every new widget must be written by hand. There is only a 16-colour palette, and
  no animation or images. That fits operator screens (ISA-101 asks for restraint) and would not fit
  a consumer interface.

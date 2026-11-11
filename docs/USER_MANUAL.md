# Prometheus HMI — User Manual

The operator panel for machines controlled over Modbus TCP.

---

## Contents

1. [Introduction](#1-introduction)
2. [Safety information](#2-safety-information)
3. [System requirements](#3-system-requirements)
4. [Installation](#4-installation)
5. [Starting the HMI](#5-starting-the-hmi)
6. [The screen at a glance](#6-the-screen-at-a-glance)
7. [Signing in and user roles](#7-signing-in-and-user-roles)
8. [Process screen](#8-process-screen)
9. [Readings screen](#9-readings-screen)
10. [Operating a device](#10-operating-a-device)
11. [Alarms](#11-alarms)
12. [EMERGENCY and restarting after a stop](#12-emergency-and-restarting-after-a-stop)
13. [Diagnostics screen](#13-diagnostics-screen)
14. [Telemetry screen](#14-telemetry-screen)
15. [Settings screen](#15-settings-screen)
16. [Keyboard shortcuts](#16-keyboard-shortcuts)
17. [Configuration](#17-configuration)
18. [Commissioning a plant](#18-commissioning-a-plant)
19. [Audit log](#19-audit-log)
20. [Files and folders](#20-files-and-folders)
21. [Troubleshooting](#21-troubleshooting)
22. [Glossary](#22-glossary)

---

## 1. Introduction

Prometheus HMI is a human–machine interface (HMI): the screen an operator uses to watch and run
machines or a process. It connects to PLCs, variable speed drives and energy meters over Modbus
TCP, finds them on the network by itself, and shows every live value. It handles alarms and
lets operators run each device.

| Program | File |
|---|---|
| **HMI runtime** | `industrial_hmi.exe` (Windows), `industrial_hmi` (Linux, Raspberry Pi) |

One HMI holds up to **128 devices**.

**Who this manual is for**

- **Operators** — sections 5 to 16.
- **Engineers and administrators** — all sections, in particular 17 to 21. Installing, backing up
  and upgrading are in the [Install and Operations Guide](INSTALL-AND-OPERATIONS.md).

## 2. Safety information

> **The EMERGENCY button is a software stop, not a certified emergency stop.**
> It sends stop requests to the machines over the network. It depends on this software, the PC,
> the network and each PLC program. Every machine must have its own hardwired emergency stop, as
> required by ISO 13850 and IEC 60204-1. In an emergency, press the hardwired emergency stop as
> well.

- This software is **not a safety system** and is not certified to IEC 61508, IEC 62061 or
  ISO 13849. Safety functions belong in the machine's safety circuits and safety PLC.
- Only trained persons may operate machines from the HMI. Every command is recorded with the role
  that sent it.
- Register maps for products other than the Altivar status and command words have not been
  verified against real hardware. An engineer must check each map against the device
  documentation before the HMI is used on a real machine.
- Modbus TCP has no built-in security. Keep the HMI and the controllers on a separate, protected
  control network.

## 3. System requirements

| Item | Minimum |
|---|---|
| **Windows** | Windows 10 or 11 (64-bit), including Windows 10 IoT panel PCs |
| **Linux** | any 64-bit x86 distribution; desktop (X11, or Wayland with XWayland), framebuffer panel, or headless server |
| **Raspberry Pi** | Pi 3, 4 or 5 (64-bit OS) or Pi Zero 2 / 2 / 3 (32-bit OS) |
| **Display** | 800 × 480 recommended (7-inch panel); 640 × 400 minimum |
| **Memory** | Task Manager: under 1 MB up to about 48 devices; 1.25 MB with 96 (measured on Windows) |
| **Disk** | under 1 MB for the program, plus space for the audit log |
| **Input** | touch screen, mouse or keyboard |
| **Sound** | speakers or headset for the alarm tone (optional) |
| **Network** | Ethernet or Wi-Fi to the controllers (Modbus TCP, port 502 or the device's port) |

No other software, runtime or framework is needed.

## 4. Installation

### 4.1 Windows — installer

1. Run **`Prometheus-HMI-Setup-<version>.exe`**.
2. If Windows shows *"Windows protected your PC"*, choose **More info → Run anyway** (the program
   is not digitally signed).
3. Follow the installer. No administrator rights are needed. The program is installed for the
   current user in `%LOCALAPPDATA%\Programs\Prometheus HMI`.

The installer adds **Prometheus HMI** to the desktop and the Start menu, and **Uninstall
Prometheus HMI** to the Start menu. Uninstalling keeps the audit log, the device key and the
PIN vault.

### 4.2 Windows — without installing

Copy `industrial_hmi.exe` into any folder and start it. It needs nothing else.

### 4.3 Linux PC and Raspberry Pi

```sh
tar -xzf prometheus-hmi-linux-x86_64.tar.gz      # Raspberry Pi: -raspberrypi-arm64 or -raspberrypi-armhf
cd prometheus-hmi-linux-x86_64
./install.sh
```

The installer needs no root rights. It puts the program in `~/.local/lib/prometheus-hmi`, adds the
command `prometheus-hmi`, and adds **Prometheus HMI** to the applications menu.
To uninstall: `./install.sh --remove` (settings, log, key and vault are kept).

## 5. Starting the HMI

Start **Prometheus HMI** from the desktop or menu, or run `industrial_hmi`. It reads its settings
from `hmi.cfg` (section 17).

On start-up the HMI:

1. checks that no other copy is already running on the same files (a second start brings the
   running window forward instead);
2. loads its device key and PINs;
3. scans the configured network ranges and attaches every PLC and drive it recognises.

Devices appear on the Process screen within a few seconds.

**Display modes on Linux:** on a desktop the HMI opens as a window; on a panel without a desktop
it draws directly to the screen (framebuffer); on a server, start it with `--headless`.

## 6. The screen at a glance

<p align="center"><img src="img/process.png" alt="Process screen" width="640"></p>

**Header (always visible)**

| Element | Meaning |
|---|---|
| **Prometheus HMI** and *n devices online* | number of devices currently connected |
| Moon button | switches between the dark theme and the high-contrast outdoor theme |
| **Health indicator** | the HMI's own health: **HMI OK**, **PLC offline**, **Data stale**, **Watchdog trip**, **UI stalled** or **Failover latch** |
| **Alarm indicator** | **No alarms**, *n* **active**, or *n* **unack.** (unacknowledged); its shape and colour show the highest priority |
| **Role indicator** | the signed-in role (**Viewer**, **Operator**, **Maintenance**, **Engineer**). Tap it to sign in or switch role |
| **EMERGENCY** | stops every device and locks the panel on the red EMERGENCY screen (section 12) |

**Alarm banner** — appears under the header while an alarm needs attention. It shows the most
important alarm and two numbered steps telling the operator what to do.

**Navigation bar (bottom)** — **Process**, **Readings**, **Alarms**, **Diagnostics**,
**Telemetry**, **Settings**. A red badge on **Alarms** shows the number of unacknowledged alarms.

**Colours**

| Colour | Means |
|---|---|
| Green | normal, running, online |
| Grey | stopped, idle, not available |
| Amber | warning, abnormal but not critical, bad data |
| Red | alarm, stop, offline, fault — and the EMERGENCY / Stop buttons |
| Blue | information alarms |

Colour is never the only signal: alarm priority is also shown by shape and by a word. Sites that
follow ISA-101 strictly can start the HMI with `--color-scheme high-performance`, which shows
normal states in grey instead of green.

## 7. Signing in and user roles

Without signing in, the HMI runs as **Viewer**: everything can be seen and **EMERGENCY** always
works, but nothing else can be changed.

**To sign in**

1. Tap the role indicator (**Viewer**) in the header, or **Sign in** on the Settings screen.
2. Choose the role.
3. Enter the PIN on the keypad and press **OK**.

You can also simply tap the control you need, on a faceplate or in an alarm's dialog. If your
role is too low, the HMI asks for the PIN of the role that control requires, then carries out the
action.

| Role | Can do |
|---|---|
| **Viewer** | watch all screens; EMERGENCY |
| **Operator** | start and stop devices, change setpoints and modes, acknowledge and shelve alarms, silence the horn, reset a stop |
| **Maintenance** | everything an Operator can, plus tuning parameters, taking alarms out of service, resetting the watchdog failover and resetting an EMERGENCY |
| **Engineer** | everything, plus raw register writes and watchdog tests |

**Rules**

- After **3 wrong PINs**, PIN entry is locked for **30 seconds**.
- After **5 minutes** without any input the HMI returns to **Viewer** (auto-logout).
- A new panel has no site PINs, so only Viewer can sign in until they are set (section 18).
- On a test bench, `--allow-factory-pins` allows **1111** (Operator), **2222** (Maintenance) and
  **3333** (Engineer). On a commissioned panel the factory PINs never work.

## 8. Process screen

<p align="center"><img src="img/overview_tiles.png" alt="Process screen with many devices" width="640"></p>

The Process screen shows every device. Its layout depends on how many there are:

| Devices | Layout |
|---|---|
| 1 to 4 | one large card per device, with its own gauges, tank, pump and buttons |
| 5 or more, if they fit | a grid of cards |
| more than fit as cards | **tiles**: name, status, alarm count, main value and run state |

**Tiles.** Each tile shows:
- the device name with its status dot;
- the number of its alarms, with the shape of the highest priority;
- the name of the main value and the run state (**Running**, **Stopped**, **Stop latch**,
  **Offline**);
- the main value, in colour only when it is in alarm, and a trend line of its last 60 seconds.

A tile with room to spare shows a larger 60-second chart, and a very tall one also shows bars for a
few more readings. Tile charts are scaled to the recent values so small changes are visible; the
scale is written at the top right of the chart (for example "72.0 to 77.0 %"). The charts in the
device view always use the full range of the reading.

Tap a tile to open the **device view**: that device alone, with all its graphics (gauges, tank,
pumps, the 60-second trend) and its buttons. **< All devices** goes back; **Controls >** opens the
faceplate. Tapping **Process** in the dock also goes back to all devices.

**Pages.** When even the tiles do not fit, **< Previous** and **Next >** appear at the bottom with
*Devices 1–12 of 96 – page 1 of 8*. The buttons also say how many devices on the other pages are
in alarm, for example **Next (6 in alarm) >**. The alarm banner and the Alarms screen always
cover every device, whatever page is shown.

If a device goes offline, its card or tile says so, and a communication alarm is raised. After
10 seconds offline it is removed (configurable with `--remove-after-ms`). It returns
automatically when the device comes back.

**More devices than this HMI holds.** One HMI holds up to 128 devices, or fewer if the site set
`--max-devices`. Devices beyond the limit are **not shown and not controlled**:
- the HMI raises a **Devices not shown** warning;
- it names each one in Alarms → History;
- EMERGENCY reports them as not stopped.

Devices listed with `--plc` always get a place first. Use a second HMI for the other machines.

## 9. Readings screen

<p align="center"><img src="img/readings.png" alt="Readings screen" width="640"></p>

The Readings screen lists **every reading of every device**, so any value can be found however
many devices there are.

| Part | Use |
|---|---|
| **All** / **In alarm** / **Bad data** | show every reading, only those in alarm, or only those without good data |
| **Search...** | opens a keyboard. Type part of a device name, product, address, reading name or unit. The list narrows as you type. **Done** closes the keyboard and keeps the search; **Clear** empties it |
| Device rows | a heading per device, with its status. A page that starts in the middle of a device repeats the heading, marked *continued* |
| Reading rows | name, value with unit, a **range bar** (where the value sits, with the high and high-high limits marked), and a status: *New alarm*, *Acknowledged*, *Cleared, unack.*, *Bad data*, *No data* or *Stale*. Nothing is shown when all is normal |
| Footer | *144 of 1074 readings shown*, and **< Previous** / **Next >** |

Tap a reading or a device heading to open that device's faceplate.

On a small screen with the alarm banner showing, the headings are left out to fit more rows.
Each row then names its device, for example *Boiler 1: Water temp*.

A PC keyboard types into the search box while it is open, including the letter E. The
**EMERGENCY** button at the top stays active above the keyboard.

## 10. Operating a device

<p align="center"><img src="img/faceplate.png" alt="Device faceplate" width="640"></p>

Tap a tile, a card's **Details >**, a reading, or **Open device** in an alarm's dialog, to open
the device's **faceplate**.

- **Left — Values:** every measurement and status of the device.
- **Right — Controls:** every action, with its current state under its name.
- **Close** returns to the previous screen.

**Commands**

| Control type | How it works |
|---|---|
| Buttons (**Run forward**, **Stop**, **Start pump** …) | act immediately (Stop buttons are red) |
| **Set …** (speed, level, ramp …) | opens a keypad showing the current value and the allowed range; values outside the range are refused before anything is sent |
| Mode selectors | tap the mode you want; the current mode is shown under the control name |
| **Reset fault** and similar | send a short reset pulse to the device |

Some commands ask **"Are you sure?"** first (mode changes, burner start, counter resets,
compressor stop, raw writes, stop resets). Press **Confirm** (or Enter) to go ahead, or **Cancel**.

**Blocked controls** — a start is refused while an interlock is active (for example an emergency
stop, a fault or a jam). The button shows **Blocked:** and the reason. Clear the cause at the
machine, then try again.

Every command is written to the audit log with the role, the device, the command and the value.

## 11. Alarms

<p align="center"><img src="img/alarms.png" alt="Alarm summary" width="640"></p>

### 11.1 Priorities

| Priority | Symbol | Meaning |
|---|---|---|
| **Stop** | red diamond | a machine is in a stop state (stop latch, emergency stop) |
| **Critical** | red diamond | immediate action needed |
| **Warning** | amber triangle | action needed soon |
| **Info** | blue circle | for information |

### 11.2 States

| State | Meaning | Action |
|---|---|---|
| **New** | the condition is active and nobody has acknowledged it | read it, act, acknowledge |
| **Acknowledged** | active, and an operator has acknowledged it | fix the cause |
| **Cleared** | the condition has gone, but nobody acknowledged it | acknowledge |
| **Chattering** | it came in 3 or more times within a minute | find the cause; shelve it if it hides other alarms |

An alarm leaves the list when it is both cleared and acknowledged.

### 11.3 Alarm sound

While any alarm is unacknowledged and the horn is not silenced, the HMI plays a two-tone alarm
signal on the PC's speakers. If a wired horn or stack light is connected (Linux GPIO), it is
switched as well.

- **Silence horn** stops the sound; the alarms stay unacknowledged.
- Acknowledging all alarms also stops the sound.
- A new alarm sounds again.
- During an **alarm flood** (section 11.6), Info alarms do not sound.
- During an **EMERGENCY** the panel loops its own EMERGENCY sound until the reset: a fast rising
  siren, different from the alarm sound on purpose, so nobody mistakes one for the other.
  `emergency.mp3` next to the program replaces it (Windows).
- `alarm.mp3` (included with the program) replaces the two-tone alarm sound (Windows). Other files
  can be chosen with `--emergency-sound <file>` and `--alarm-sound <file>`. Do not use the same file
  for both.

### 11.4 The Alarms screen

| Button | Does |
|---|---|
| **Active** / **Shelved** / **History** | current alarms; shelved and out-of-service alarms; the event history |
| **Ack** (on a row) | acknowledge that alarm only — the recommended way |
| A tap anywhere else on a row | opens that alarm's dialog (section 11.5) |
| **Acknowledge all** | acknowledge every shown alarm (for a known flood of alarms) |
| **Silence horn** | stop the alarm sound |
| **Reset stop** | reset a machine's stop latch (section 12) |
| **Previous** / **Next** | page through long lists |

Active alarms are sorted by priority, then unacknowledged first, then newest first. Times are
local wall-clock times. The card title shows *n active – n unacknowledged – n new in 10 min*.

### 11.5 The alarm dialog

<p align="center"><img src="img/alarm_dialog.png" alt="Alarm dialog" width="640"></p>

Tap an alarm to see:
- its name, priority, state and the time it came in;
- the value now and its limits;
- the two steps of guidance;
- a note if it is shelved, out of service or chattering.

It has four buttons:

| Button | Does | Role |
|---|---|---|
| **Acknowledge** | acknowledge this alarm | Operator |
| **Shelve for 60 min** / **Unshelve** | hide this alarm for one hour; it comes back by itself, or earlier with Unshelve | Operator |
| **Out of service** / **Return to service** | hide this alarm until Maintenance returns it (for example a broken sensor) | Maintenance |
| **Open device** | open the device's faceplate | anyone |

A shelved or out-of-service alarm is still checked and recorded, but it is not shown in the list,
counted, or sounded. Find it under **Alarms → Shelved**, with the minutes left and an **Unshelve**
or **Return** button. **Stop alarms (stop latches) can never be shelved or taken out of service.**
Every shelve, unshelve and change of service is recorded in the audit log.

### 11.6 Alarm floods

When more than **10 new alarms** come in within **10 minutes**, the HMI declares an **alarm flood**.
The Alarms screen then says **ALARM FLOOD – n new in 10 min – work from the top** in amber. Work
through the list from the top: the highest priorities are first. The flood ends when fewer than 5
new alarms came in during the last 10 minutes. The start and end are recorded in the audit log.

### 11.7 The alarm banner

The banner under the header shows the most important alarm with two steps of guidance, for
example:

> **Tank skid 1: Stop latch active**
> 1. Hardwired E-stop pressed? Release it at the machine
> 2. Acknowledge, then Reset stop on Alarms

**Acknowledge** on the banner acknowledges that alarm.

## 12. EMERGENCY and restarting after a stop

<p align="center"><img src="img/emergency.png" alt="The EMERGENCY screen" width="640"></p>

**EMERGENCY** is the red button at the top right of every screen (keyboard: **E**). Anyone can
press it, in every situation: without signing in, with a dialog open, on any screen.

When pressed, the HMI:

1. cancels every command that has not been sent yet;
2. sends each device its stop: the stop latch, every Stop/Off control and the stopping mode;
3. turns the whole panel **red** and shows, machine by machine, **Stop sent**, **Nothing to stop**
   (meters) or **NOT STOPPED** — offline, no stop control, or found on the network but not on this
   HMI. Machines NOT stopped are listed first. Stop those at the machine;
4. stays on the red screen. Nothing else can be operated. A machine that comes back online, or
   is found, while the EMERGENCY lasts is stopped as soon as it answers;
5. stops sending the HMI heartbeat, so PLC programs that watch it stop their machines as well.

When there are more machines than fit on the screen, the last line says how many more there are
and how many of them were NOT stopped.

Pressing EMERGENCY again sends the stop again. The press and the reset are both recorded in the
signed audit log with the role that was signed in.

**Leaving the EMERGENCY screen**

1. Make the area safe. Use the hardwired emergency stop at each machine as well.
2. Press **Reset EMERGENCY (Maintenance)** at the bottom of the red screen. If Maintenance is not
   signed in, the panel asks for the Maintenance PIN.
3. Confirm. The panel returns to normal. **The machines stay stopped**: the reset sends nothing to them.

<p align="center"><img src="img/stop_all.png" alt="After the reset: stop alarm" width="640"></p>

**Restarting after a stop**

1. Find and remove the cause. If a hardwired emergency stop was pressed, release it at the machine.
2. Sign in as **Operator** (or higher).
3. Acknowledge the stop alarm.
4. Press **Reset stop** on the Alarms screen and confirm.
5. Start the machines again from their faceplates. **Nothing restarts by itself.**

**HMI heartbeat** — while the HMI is running, it sends a heartbeat signal to PLCs that support it.
If the HMI stops, freezes, or its cable is pulled, the PLC program stops the machine by itself
after 5 seconds and stays stopped until an operator resets it.

## 13. Diagnostics screen

<p align="center"><img src="img/diagnostics.png" alt="Diagnostics" width="640"></p>

For engineers and maintenance staff.

| Card | Shows |
|---|---|
| **Fieldbus loop** | the communication loop rate (normally about 1000 Hz), its period, minimum/maximum and late cycles |
| **Integrity** | data checksum, checksum errors (should be 0), age of the data on screen, **Latency avg/max** (the time from a device's data arriving to it being on the screen, over the last second; target under 100 ms), retries |
| **Devices** | each device's response time, address, and successful / total requests |
| **Register map** | the first device's registers: address, name, value and raw hexadecimal value |

## 14. Telemetry screen

Shows the health of the computer running the HMI: **CPU load**, **memory in use**, and the HMI's
own memory use. Each value has warning and alarm limits; a sustained high CPU load raises an alarm.

## 15. Settings screen

<p align="center"><img src="img/settings.png" alt="Settings" width="640"></p>

**Access control** — the signed-in role, the PIN status (site PINs, or *Factory PINs – BENCH MODE*),
the role needed for each kind of action, and the auto-logout time. **Sign in** /
**Switch role / sign out** changes the role.

**System** — watchdog states, failover latch, colour scheme, audit log status, program size,
frame buffer and display refresh rate.

| Button | Does | Role |
|---|---|---|
| **Outdoor theme** / **Dark theme** | high-contrast theme for bright light, or back | any |
| **Test watchdog** | simulates a communication-loop failure to test the watchdog | Engineer |
| **Reset failover** | clears the watchdog failover latch after a fault | Maintenance |

## 16. Keyboard shortcuts

| Key | Action |
|---|---|
| **E** | EMERGENCY (always works, except while the search keyboard is open: there it types an E) |
| **1** – **6** | Process, Readings, Alarms, Diagnostics, Telemetry, Settings |
| **Tab** / **Shift+Tab**, **←** / **→** | next / previous screen |
| **A** or **Space** | acknowledge all alarms |
| **S** | silence horn |
| **X** | reset stop |
| **L** or **R** | sign in / switch role |
| **T** or **C** | switch theme |
| **Esc** | close the open dialog |
| **0**–**9**, **.**, **-**, **Backspace**, **Enter** | keypad entry in PIN and value dialogs; Enter also confirms |
| letters, digits, **Backspace**, **Enter** | typing in the Readings search keyboard; Enter closes it |

## 17. Configuration

### 17.1 The configuration file

The HMI reads `hmi.cfg` from its own folder at start-up (Linux launcher:
`~/.local/share/prometheus-hmi/hmi.cfg`). Write one or more options per line; `#` starts a comment.
Options given on the command line are added to those in the file.

```text
# Plant network: discover every PLC and drive on this subnet
--scan 192.168.10.0/24

# The energy meter does not identify itself: list it
--plc  192.168.10.40@em6400

--remove-after-ms 30000
```

Examples are in `hmi-runtime/examples/`:
- `starter.cfg`: a new site, commented;
- `lab-site.cfg`: a real network;
- `bench.cfg`: test devices on the same PC, with factory PINs.

### 17.2 Options

**Devices and network**

| Option | Meaning |
|---|---|
| `--scan <range>` | discovery range: `192.168.1.0/24`, `10.0.0.5:502-510`, `/uN` for a unit id |
| `--plc <ip[:port][/uN][@module]>` | attach a device even if it does not identify itself; `@module` forces a driver (e.g. `@em6400`, `@altivar`, `@generic`) |
| `--no-discovery` | attach only the `--plc` devices, no scanning |
| `--max-devices <n>` | attach at most *n* devices (1 to 128; default 128) |
| `--remove-after-ms <n>` | remove offline devices from the screen after *n* ms (0 = never; default 10000) |

**Files and security**

| Option | Meaning |
|---|---|
| `--config <file>` / `--no-config` | read another configuration file / none |
| `--log <path>` / `--no-log` | audit log file (default `alarms.log` next to the program) / no log |
| `--key <path>` | device key (default `hmi.key` in the protected folder, section 20) |
| `--vault <path>` | PIN vault (default `security.vault` in the protected folder) |
| `--set-pin <role>` | set the PIN of `operator`, `maintenance` or `engineer`, then exit |
| `--verify-log <path>` | check an audit log's signatures, then exit |
| `--allow-factory-pins` | **test bench only**: allow the factory PINs |

**Display and alarm output**

| Option | Meaning |
|---|---|
| `--color-scheme status` | normal states in green (default) |
| `--color-scheme high-performance` | normal states in grey (strict ISA-101) |
| `--no-sound` | no alarm tone from the speakers |
| `--emergency-sound <file>` | sound looped during an EMERGENCY (Windows; MP3 or WAV; default `emergency.mp3` next to the program, else the built-in siren) |
| `--alarm-sound <file>` | sound instead of the two-tone alarm (Windows; default `alarm.mp3` next to the program) |
| `--window` | Linux: force a desktop window |
| `--fb <device>` | Linux: force the framebuffer (e.g. `/dev/fb0`) |
| `--headless` | Linux: no display (communication, alarms and log only) |
| `--horn-gpio <chip>:<line>` | Linux: drive a GPIO line for a horn or stack light, e.g. `gpiochip0:17` |
| `--horn-fail-safe` | invert that line: on when healthy, off on alarm or if the HMI fails |

**Measuring**

| Option | Meaning |
|---|---|
| `--report <file>` | write the HMI's own measurements (loop timing, latency, alarm rate, devices) on exit |
| `--exit-after-ms <n>` | close by itself after *n* ms |

**Environment variables**

| Variable | Meaning |
|---|---|
| `HMI_SOUND_DEVICE` | Linux: sound device for the alarm tone (default `default`, e.g. `plughw:1,0`) |
| `HMI_FBDEV` | Linux: framebuffer device (default `/dev/fb0`) |

## 18. Commissioning a plant

Do this once, before the HMI is used on real machines.

1. **Check the device maps.** Compare each driver module's registers with the device or PLC
   program documentation.
2. **Write the site `hmi.cfg`** with the plant's `--scan` ranges and `--plc` entries.
   Do **not** include `--allow-factory-pins`.
3. **Set a PIN for every role.** Close the HMI, open a command prompt in the HMI folder (as the
   user account the HMI runs under) and run:

   ```bat
   industrial_hmi.exe --set-pin operator
   industrial_hmi.exe --set-pin maintenance
   industrial_hmi.exe --set-pin engineer
   ```

   It asks for the new PIN twice; the digits are hidden. PINs must have **6 to 8 digits** and
   must not be a repeated digit or a straight run (`111111`, `123456`, `987654` are refused).
   PINs are never typed on the command line.

   Once any site PIN exists the panel is **commissioned**. From then on, factory PINs and test
   mode are refused, even if `--allow-factory-pins` is given, and the attempt is logged. Roles
   that have no site PIN yet cannot sign in, so set all three.
4. **Back up the device key.** The first start creates `hmi.key` in the protected folder
   (section 20). It signs the audit log and the PIN vault. Keep an offline copy. Never copy it to
   other panels or into backups that others can read.
5. **Connect the heartbeat.** For each PLC that should stop when the HMI is lost, add the heartbeat
   logic to the PLC program. The HMI writes a changing counter every second; the PLC stops the
   machine when it has not changed for 5 seconds.
6. **Separate the network.** Put the HMI and the controllers in their own network zone.
7. **Test** EMERGENCY and its reset, every machine's stop, the alarm sound and the hardwired
   emergency stops.

**Returning a bench PC to test mode:** close the HMI, delete `security.vault` (keep `hmi.key`) and
start it again with `--allow-factory-pins`.

## 19. Audit log

The HMI writes every alarm, event, sign-in, shelve and command to `alarms.log`. Each record is
chained to the previous one and signed with the panel's device key. Records that are changed,
inserted or deleted inside the log are therefore detected. A chain cannot show records cut off
the **end** of the newest file, or a whole old file deleted: copy the log off the panel regularly
if that matters.

To check a log:

```bat
industrial_hmi.exe --verify-log alarms.log
```

The result is **VERIFIED** (the chain is intact) or **FAILED** with the first bad line. Use the
same device key that wrote the log.

## 20. Files and folders

| File | Where | Created | Contents |
|---|---|---|---|
| `hmi.cfg` | next to the program | by you | site configuration |
| `alarms.log` | next to the program | by the HMI | signed audit log of alarms, events and commands |
| `hmi.key` | protected folder | first start | the panel's device key — keep it secret and with the panel |
| `hmi.key.lock` | protected folder | while running | prevents two HMIs using the same files |
| `security.vault` | protected folder | `--set-pin` | site PINs (keyed hashes, signed) |
| `crash.log` | next to the program | after a crash | information for the developers |

**Protected folder**
- **Windows:** `%ProgramData%\Prometheus HMI`. Only SYSTEM, Administrators and the account that
  runs the HMI can open it.
- **Linux, Raspberry Pi:** `/var/lib/prometheus-hmi` when run as root, otherwise
  `~/.local/state/prometheus-hmi`. The Linux launcher keeps all files in
  `~/.local/share/prometheus-hmi`. Both are readable by the owner only.

An older installation kept `hmi.key` and `security.vault` next to the program. The first start of
this version moves them into the protected folder, and records the move in the audit log.

## 21. Troubleshooting

| Problem | Cause and solution |
|---|---|
| **No devices appear** | Check the network cable and IP addresses. Check the `--scan` range in `hmi.cfg`. Devices that do not identify themselves (energy meters, unknown devices) must be listed with `--plc`. |
| **"Devices not shown" warning** | More devices were found than this HMI holds (128, or the site's `--max-devices`). The extra ones are not on this HMI, and EMERGENCY does not reach them; their addresses are in Alarms → History. Narrow the `--scan` range, list the important devices with `--plc`, or use a second HMI. |
| **A device shows Offline** | The device does not answer: check power, cable, IP address and port. The HMI reconnects by itself. |
| **A device shows "Other device - blocked"** | After reconnecting, a different device answered at that address (another vendor or product). Nothing is read from it or sent to it. Check the device and its IP address. After 10 s offline it is removed, and a correct device is then attached with its own driver. |
| **A value cannot be found** | Use **Readings → Search...**, or the **In alarm** / **Bad data** filters. |
| **An alarm is missing from the list** | It may be shelved or out of service: look under **Alarms → Shelved**. |
| **Cannot sign in with 1111** | The panel is commissioned (a site PIN exists), or the HMI was started without test mode. Use the site PIN, or see section 18 to return a bench PC to test mode. |
| **"PIN entry locked for 30 s"** | Three wrong PINs. Wait 30 seconds. |
| **A button says Blocked:** | An interlock is active (emergency stop, fault, jam …). Clear the cause at the machine. |
| **No alarm sound** | Check the PC volume and output device. Check that `--no-sound` is not set. During an alarm flood, Info alarms are silent. On Linux, look for *"no sound device"* in the terminal and set `HMI_SOUND_DEVICE` if needed. |
| **Nothing happens when starting the HMI a second time** | It is already running; the running window is brought forward. |
| **"This display is … The HMI needs 640x400 or larger."** | The window or screen is too small. Enlarge the window or use a larger display. |
| **Linux: "desktop window unavailable"** | No usable desktop session or X11 library. Install `libx11-6`, or use `--fb` on a panel, or `--headless` on a server. |
| **Windows: "Windows protected your PC"** | The program is not digitally signed. Choose **More info → Run anyway**. |
| **Health indicator shows Data stale / Watchdog trip** | The communication loop has stopped responding. Note the time, check Diagnostics, then restart the HMI. **Reset failover** (Maintenance) clears the latch. |
| **"Device key unusable – log NOT signed"** | `hmi.key` is missing, damaged or not readable. Restore it from the panel's secure backup. |

## 22. Glossary

| Term | Meaning |
|---|---|
| **HMI** | Human–machine interface: the operator's screen for a machine or process |
| **PLC** | Programmable logic controller: the computer that runs the machine |
| **Modbus TCP** | the industrial network protocol used between the HMI and the devices |
| **Faceplate** | the detail window of one device, with its values and controls |
| **Reading** | one value from a device, such as a level, a speed or a status |
| **Interlock** | a condition that blocks a command for safety or protection |
| **Stop latch** | a PLC state that keeps a machine stopped until it is reset |
| **Acknowledge** | confirm that an alarm has been seen |
| **Shelve** | hide one alarm for a limited time; it comes back by itself |
| **Out of service** | hide one alarm until maintenance returns it |
| **Alarm flood** | more alarms than an operator can handle: here, more than 10 in 10 minutes |
| **Chattering alarm** | an alarm that keeps coming and going |
| **Bench / test mode** | training setup with test devices and factory PINs |
| **Commissioned** | a panel with site PINs, ready for real machines |
| **Heartbeat** | a changing signal from the HMI that tells the PLC the HMI is alive |
| **Audit log** | the signed record of everything that happened on the HMI |
| **ISA-101 / ISA-18.2** | international standards for HMI design and alarm management |

---

<sub>Prometheus HMI · User Manual</sub>

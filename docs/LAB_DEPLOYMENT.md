# Lab deployment: the five SRM panels

This note covers the five lab setups shown in the photos. It separates three things:
**confirmed** (from public Schneider material found during research), **assumed** (plausible,
must be checked on the bench), and **not possible** (outside what this project can do).

## The main finding: the Harmony panels cannot run this runtime

The Harmony ST6 panels (HMIST6400, HMIST6600) run Schneider's own runtime,
**EcoStruxure Operator Terminal Expert**. Research did not find any public statement that these
panels can run a third-party native executable. Treat that as **not possible** until Schneider
confirms otherwise. Our runtime cannot be installed on them.

What still works: the **same network and the same devices**. The runtime runs on a computer
that sits on the panels' Ethernet switch and talks Modbus TCP to the PLCs and drives directly.

| Option | What it needs | Notes |
|---|---|---|
| **A. Windows PC** | a PC on the switch, with the runtime | the simplest; this is what has been tested |
| **B. Harmony P6 iPC or any Windows industrial PC** | Windows, the runtime | Schneider sells the P6 with Windows 10 IoT Enterprise and states it "can run all software on Windows 10"; not yet tried on one |
| **C. Raspberry Pi** | Linux build, framebuffer or headless | built and tested for 32- and 64-bit Pi under emulation; not yet run on a Pi screen |
| **D. Harmony STW6 web panel (future)** | a web front end in the runtime | the STW6 is a browser terminal that shows any HTML5 server. If the runtime served its screens over HTTP, an STW6 on the same switch could display them with no Schneider runtime. Not built yet; it would need HTTPS and sign-in on the web side before any control is allowed from it |

Using option A/B/C, the panel's own screen stays as it is. The runtime becomes a second operator
screen that understands every device. If the lab requires the Harmony panel itself to show the
screens, that needs EcoStruxure Operator Terminal Expert on the panel, not this runtime.

## Per-panel status

| # | Panel (photo) | Devices | Protocol the runtime can use | Status |
|---|---|---|---|---|
| 1 | HMIST6600 + M251 + ATV340 | M251 PLC, ATV340 drive | Modbus TCP to both | Altivar map: status and command words **confirmed**; the M251 program map is **program-specific** (see below) |
| 2 | HMIST6600 + EAE soft PLC + TM3BCEIP + ATV630 | soft PLC, TM3 bus coupler with buttons and lamps, ATV630 | Modbus TCP to the ATV630 and, if the soft PLC serves Modbus, to it | the TM3 coupler **has no module yet** (see below); the EAE soft PLC's Modbus support is **unknown** |
| 3 | HMIST6600 as OPC UA server + ATV340 | ATV340 over Modbus TCP | Modbus TCP to the ATV340 | the runtime **does not support OPC UA**; the drive is reachable over Modbus |
| 4 | HMIST6600 as OPC UA client + M241 as OPC UA server | M241 | Modbus TCP, if the M241 program serves it | the runtime **does not support OPC UA**; the M241's Modbus server must be enabled and its %MW map defined |
| 5 | HMIST6400 + PAS600 + EM6400 meters over RS-485 | PAS600 gateway, EM6400 meters | Modbus TCP to the PAS600; it forwards to the meters | the meter module exists but its addresses are **unverified** |

## What is confirmed and what is not

**Confirmed in public Schneider material (research snippets):**
- Altivar parameter addresses: ETA 3201, RFR 3202 (output frequency), CMD 8501 (control word),
  LFR 8502 (speed reference, 0.1 Hz). Source: Schneider FAQ and ATV600 communication lists
  found in search.
- The M241 and M251 serve Modbus TCP, and their %MW variables are holding registers. The %MW
  numbers depend on the program, so they must be read from the program in EcoStruxure Machine
  Expert Basic.
- TM3BCEIP register ranges (from the Machine Expert help text, one search snippet): inputs
  3001-3499, outputs 3501-3999, system state register 932. The official page returned HTTP 403
  when fetched, so this is **unverified**.

**Assumed, to check on the bench:**
- The EM6400 register addresses. Research did not return Schneider's register list. The PAS600
  forwarding meter traffic by unit id is also assumed.
- The Altivar addresses FRH 3203, LCR 3204 (one third-party guide says 3204 is ETA, so this
  conflicts), LFT 7121, ACC 9001 and DEC 9002.
- The CiA402 bit layout of the status word, and that bit 9 means "network has control".
- The drive's command channel must be set to Modbus TCP, or it ignores the commands. This is in
  the drive's parameter list, which we have not read.

**Not possible / not done:**
- OPC UA: not supported by the runtime, and not planned in this change.
- The HMIST6600 as a runtime host: see above.
- The TM3BCEIP I/O module: not written. It needs the coupler's I/O map from the official help,
  which is blocked here.
- The M241 / M251 program maps: the runtime reads what each program defines. Give the program's
  %MW list and we can write its application module.

## Bench plan (what to check before the panel visit)

1. Put the PC (or the P6 / Pi) on the panels' switch with a static address.
2. Run `plc_simulator` with the same device types as the panel, to rehearse the screens.
3. Against the real ATV340, read ETA, RFR and LFR with the runtime's `--plc <ip>/u1@altivar`
   and compare them with the drive's display. Then send a Stop and a 5 Hz setpoint on a drive
   that is safe to move.
4. For the PAS600, read one meter's voltage and compare with a handheld meter. This settles the
   addresses and the unit-id mapping in one step.
5. Only then add the M251 / M241 program map.

Keep the rule from the project: do not drive a real machine with a register map that has not
been checked against its device.

## Before the runtime touches a real machine

1. Set every role's site PIN, as the account the HMI runs under:
   `industrial_hmi --set-pin operator` (and maintenance, engineer). Do not copy
   `--allow-factory-pins` from the bench configuration (a commissioned panel refuses it anyway).
2. Keep `hmi.key` and `security.vault` on the panel PC only, readable by the HMI account.
3. Put the PCs, panels, PLCs and drives on their own control network. Modbus TCP has no
   authentication: anyone on that network can write to the PLCs directly, with or without this HMI.
4. Check that each machine's hardwired emergency stop works on its own. STOP ALL on the HMI is a
   software stop request, not an emergency stop.
5. In each PLC program the HMI controls, add the heartbeat rungs (see the runtime README, "HMI
   heartbeat"): 5 s without a changing heartbeat sets the program's stop latch.
6. Wire an alarm buzzer or stack light: `--horn-gpio gpiochip0:<line>` on a Pi, `BOARD_PIN_HORN` on
   an ESP32 board. A PC's own beep is not loud enough on a plant floor.
7. For drives commanded over Modbus, set the drive's own communication-loss behaviour (Altivar:
   the Modbus TCP / Ethernet timeout and its fault response) so that a drive stops if the HMI or
   the network goes away. The HMI cannot stop a drive it can no longer reach.

# Site Test Checklist

Run this on the real panel (Windows panel PC, Harmony P6, Linux panel or Raspberry Pi) before the
HMI is used on a plant. Tick each line and keep the sheet with the commissioning records.
The machines must be in a safe state, with the hardwired E-stops working.

**Panel:** ____________  **HMI build** (`--report`, `build=`): ____________
**Date:** ____________  **Tested by:** ____________

## 1. Start

- [ ] The HMI starts and its window (or framebuffer screen) fills the display.
- [ ] Starting it a second time brings the first one forward (no second copy).
- [ ] The key and vault are in the protected folder, which only the HMI account can open
      (Windows: `icacls "%ProgramData%\Prometheus HMI"`; Linux: `ls -ld`).
- [ ] Factory PIN `1111` is refused; each site PIN works for its role.

## 2. Devices

- [ ] Every device appears and shows **Online**.
- [ ] Each reading matches the device's own display or a handheld meter (note any difference).
- [ ] Unplugging one device's cable raises its comm-loss alarm within 2 s; replugging recovers it.
- [ ] A different device given that address is shown as **Other device - blocked**.
- [ ] Diagnostics: loop about 1,000 Hz, CRC errors 0, **Latency avg/max under 100 ms**.

## 3. Screens and touch

- [ ] Every dock tab, button, tile and reading row responds to a single touch, **with the gloves
      used on site**.
- [ ] Title-row buttons and filter chips (36 px) can be hit reliably with gloves.
- [ ] Readings → Search finds a device by name; the filters work.
- [ ] Outdoor theme is readable in the brightest light on site.

## 4. Operating

- [ ] Each start/stop control works, and every machine stops from its Stop button.
- [ ] A setpoint outside its range is refused; one inside is applied.
- [ ] An interlocked start says **Blocked:** with the right reason.

## 5. Alarms

- [ ] A forced high value raises the alarm, the banner and the alarm sound (or wired horn).
- [ ] Ack on a row, Silence horn and Acknowledge all work.
- [ ] Shelve for 60 min hides the alarm; it is listed under **Shelved**; Unshelve brings it back.
- [ ] A stop-latch alarm cannot be shelved.

## 6. EMERGENCY

- [ ] EMERGENCY stops **every** machine; the red screen lists each one as **Stop sent**.
- [ ] A machine whose cable is pulled first is listed as **NOT STOPPED**, at the top.
- [ ] Reset EMERGENCY asks for the Maintenance PIN; after it no machine restarts.
- [ ] Each PLC with the heartbeat stops within 5 s of closing the HMI.
- [ ] The hardwired E-stop works independently of the HMI.

## 7. Records

- [ ] `industrial_hmi --verify-log alarms.log` says **VERIFIED**.
- [ ] The commands, alarms and EMERGENCY of this test are in the log with their roles.
- [ ] `hmi.key` is backed up offline.

Result: [ ] passed  [ ] passed with notes  [ ] failed — notes: ________________________

# Known Limitations

Everything that Prometheus HMI does not do, has not been checked, or does only in part.
Each item says what would close it.

---

## Not run on target hardware

| Item | To close it |
|---|---|
| A Windows panel PC, including Schneider Harmony P6 (Windows 10 IoT) | Run the release build on the panel; repeat the benchmarks there |
| A Raspberry Pi with a touch screen | Run the Linux arm64/armhf build on a Pi with its display |
| The ESP32 port (`ports/esp32/`) | It has never been compiled: install ESP-IDF and build it |
| Schneider Harmony ST6 panels | They run Schneider's own runtime and cannot run this program at all (see [LAB_DEPLOYMENT.md](LAB_DEPLOYMENT.md)) |

All performance figures come from one Windows PC ([benchmarks](benchmarks/README.md)).

## Not verified against real devices

| Item | To close it |
|---|---|
| Register maps other than the Altivar status and command words | Check each module's addresses against the device manual. The unverified addresses are listed at the top of each module |
| EM6400NG meter addresses | Check them against the meter's register map |
| A real Modicon M221 program with the tank map | Load the program into a PLC and run the HMI against it |

## Operator interface

| Item | To close it |
|---|---|
| Not reviewed with operators on a real panel; no site style guide | An ISA-101 review with the people who will use it |
| Title-row buttons (36 px) and compact reading rows (34 px) are under 7 mm on a 7" panel; untested with gloves | A glove test on the target panel |
| A PC keyboard's "E" key types into the search box while it is open, instead of pressing EMERGENCY. The on-screen EMERGENCY button still works | By design; noted in the manual |

## Alarms

| Item | To close it |
|---|---|
| No low or low-low limits | Add low limits to the driver module format |
| No on-delay or off-delay timers | Add per-alarm delays to the module format |
| Shelves and out-of-service marks are lost on restart | Keep them in a signed file |
| No master alarm database or rationalisation records | Limits and texts live in the driver modules |

## Security

| Item | To close it |
|---|---|
| One PIN per role, not per person | Per-person accounts |
| PIN lockout is not kept across a restart | Keep the failure count in the vault |
| `--set-pin` does not ask for the old PIN: anyone who can run programs as the HMI's account can set new PINs | Ask for the Engineer PIN once site PINs exist; protect the account |
| Modbus TCP has no authentication or encryption | Network segmentation (site); Modbus/TCP Security where the devices support it |
| Records cut off the end of the newest audit file are not detected | Copy the log off the panel; syslog forwarding |
| The identity check cannot detect a device that copies another device's identity | Requires device authentication, which Modbus does not have |
| Not assessed by an independent IEC 62443 assessor | An assessment |

## Scale and performance

| Item | To close it |
|---|---|
| 128 devices per HMI by default; at most 250 in a build, because slot numbers are 8-bit | A second HMI, or wider slot numbers |
| At 96 devices, 0.17 % of data-to-screen samples took over 100 ms (worst 138.6 ms) | Analyse the frame gaps (start-up, audit-log flush on the UI thread) |
| Start-up rose from 110 ms to about 240 to 300 ms | Profile the start-up |
| Each device is polled every 100 ms, whatever its type | Per-device poll rates |
| Memory in Task Manager is under 1 MB only up to about 48 devices (1.25 MB at 96, 1.22 to 1.35 MB at 120) | Fewer copies of the device data (one snapshot copy, no per-slot metadata copies) |
| At 120 devices the fieldbus loop ran 899 times a second instead of 1,000, with one stall of about 100 ms | Profile the loop with many devices attached |
| Sound files (MP3/WAV) play on Windows only; Linux plays the built-in tones and the ESP32 only drives a horn output | An embedded PCM clip for Linux / ESP32 |
| While an MP3 plays, Windows' media decoders are loaded: Task Manager memory rose to 1.27 MB for a few seconds, then 856 KB after the memory trim (3 devices) | A built-in sound instead of the MP3 |
| The tile charts and the EMERGENCY siren were built and tested on Windows only; the Linux / Raspberry Pi builds and the Windows installer were not rebuilt | Run `make test` on Linux and the release scripts |

## Not supported

- OPC UA, MQTT, or any protocol other than Modbus TCP.
- A web (HTML5) client or remote viewing.
- Trends longer than 60 s, historian storage, recipes, reports.
- Writing multiple registers at once (FC 15, 16), and FC 02 / 04.

## Not certified

ISA-101, ISA-18.2, IEC 62443-4-2 and ISO 13850 are followed, as described in
[STANDARDS.md](STANDARDS.md). None is certified. **This is not a safety system**, and EMERGENCY is
not a certified emergency stop: every machine needs its hardwired E-stop.

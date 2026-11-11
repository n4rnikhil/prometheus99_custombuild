# Install and Operations Guide

For the people who install, commission and look after a Prometheus HMI panel. How to operate the
screens is in the [User Manual](USER_MANUAL.md).

---

## 1. What you need

| | Minimum |
|---|---|
| Windows | Windows 10 or 11 (x86-64), including Windows 10 IoT panel PCs |
| Linux | x86-64 PC, or Raspberry Pi with a 64-bit or 32-bit OS |
| Screen | 800 x 480 or larger. Below 640 x 400 only a safe screen with EMERGENCY is shown |
| Memory | about 4 MB for the program with 96 devices (measured on Windows) |
| Network | Modbus TCP to the devices, normally port 502 |

## 2. Install

### Windows
1. Run `Prometheus-HMI-Setup-<version>.exe`. It needs no administrator rights and installs into
   `%LOCALAPPDATA%\Programs\Prometheus HMI`.
2. Or copy `industrial_hmi.exe` into a folder of your choice. It needs nothing else.

### Linux and Raspberry Pi
```sh
tar -xzf prometheus-hmi-<platform>.tar.gz
cd prometheus-hmi-<platform>
./install.sh                # per user; programs in ~/.local/lib/prometheus-hmi
```
Remove with `./install.sh --remove`. The settings, log, key and vault are kept.

## 3. Commission

Do this once, on the panel, as the account the HMI will run under, with the HMI closed.

1. **Set the role PINs** (6 to 8 digits; no 111111 or 123456 patterns). Each is asked twice:
   ```
   industrial_hmi --set-pin operator
   industrial_hmi --set-pin maintenance
   industrial_hmi --set-pin engineer
   ```
   From then on, factory PINs and test mode are refused, and every attempt is logged.
2. **Write `hmi.cfg`** next to the program (start from `examples/starter.cfg`):
   ```
   --scan 192.168.10.0/24:502          # where the PLCs and drives are
   --plc  192.168.10.50/u1@em6400      # devices that do not identify themselves
   --max-devices 32                    # optional site limit
   ```
3. **Start the HMI** and check on **Diagnostics** that every device is online and the latency
   is under 100 ms.
4. **Test EMERGENCY** with the machines in a safe state. Check that every machine shows
   "Stop sent", and that Maintenance can reset.
5. **Add the heartbeat** to each PLC program that should stop if the HMI is lost
   (`hmi-runtime/README.md`, HMI heartbeat).

## 4. Files

| File | Where | What it is | Back it up |
|---|---|---|---|
| `industrial_hmi(.exe)` | install folder | the program | from the release |
| `hmi.cfg` | install folder | site settings | yes |
| `alarms.log`, `alarms.log.1` | install folder | signed audit log, rotated at 1 MB | **yes, regularly, off the panel** |
| `hmi.key` | protected folder (below) | the panel's master key | **yes, offline and secure** |
| `security.vault` | protected folder | the role PINs (keyed hashes, signed) | yes |
| `hmi.key.lock` | protected folder | single-instance lock | no |

**Protected folder:**
- Windows: `%ProgramData%\Prometheus HMI`. Only SYSTEM, Administrators and the account that
  created it can open it.
- Linux: `/var/lib/prometheus-hmi` when run as root, otherwise `~/.local/state/prometheus-hmi`
  (mode 0700). The Linux launcher (`prometheus-hmi`) keeps everything in
  `~/.local/share/prometheus-hmi` instead (mode 0700).

An older installation kept `hmi.key` and `security.vault` next to the program. The first start of
this version moves them into the protected folder and records it in the log.

`--key`, `--vault` and `--log` put the files elsewhere.

## 5. Run

| Situation | How |
|---|---|
| Panel PC, operator desk | start `industrial_hmi`; it opens full screen or as a window |
| Linux panel without a desktop | it draws to the framebuffer directly (`--fb /dev/fb0`) |
| Server, no screen | `--headless` |
| Start with Windows | put a shortcut in the Startup folder of the HMI account, or use Task Scheduler "At log on" |
| Start with Linux | a systemd user service running `prometheus-hmi` |

Only one HMI runs on the same files. A second start brings the running window forward and exits
with code 4.

## 6. Check it is healthy

| Where | What to look at |
|---|---|
| Header | **HMI OK**. Anything else names the problem |
| Diagnostics | fieldbus loop at about 1,000 Hz; CRC errors 0; latency avg/max under 100 ms |
| `--report <file>` | the same figures in a text file, written on exit |
| Audit log | `industrial_hmi --verify-log alarms.log` (exit code 0 = intact) |

## 7. Upgrade

1. Close the HMI.
2. Copy `alarms.log*` off the panel.
3. Run the new installer, or replace `industrial_hmi.exe`.
4. Start it. The key, vault and settings are kept. `--verify-log` still passes across the upgrade,
   because the key is the same.

## 8. Recover

| Problem | What to do |
|---|---|
| Forgotten PIN | Someone with access to the HMI's account: close the HMI and run `--set-pin <role>` again (the old PIN is not asked for, so protect that account) |
| `hmi.key` lost | Restore it from the backup. Without it, old logs cannot be verified and the vault is unreadable: set all PINs again |
| "Credential vault damaged" | Restore `security.vault`, or set the PINs again |
| A device shows "Other device - blocked" | A different device answers at that address. Check the device and its IP address. Once it has been offline for 10 s, it is removed, and discovery attaches whatever answers there now with its own driver |
| Bench panel back to test mode | Close the HMI and delete `security.vault` (keep `hmi.key`) |

## 9. Uninstall

Windows: Settings → Apps → Prometheus HMI → Uninstall. Linux: `./install.sh --remove`.
The audit log, key and vault are records of the site, so they are kept. Delete them yourself
once you no longer need them.

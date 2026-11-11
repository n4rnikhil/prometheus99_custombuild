# Security Policy

What Prometheus HMI protects, how, and what a site must do itself. It follows the component
requirements of IEC 62443-4-2. **It is not certified.** Risks and threats are in the
[Threat model](THREAT-MODEL.md).

---

## 1. Reporting a vulnerability

Report it privately to the maintainer through the repository's GitHub security advisory page
("Report a vulnerability"). Do not open a public issue. Please include:
- the version (`build=` line of a `--report` file, or the exe's file properties);
- the steps to reproduce it;
- the impact you expect.

Only the latest release receives fixes.

## 2. What the HMI does

### Access control
| Control | Detail |
|---|---|
| Roles | Viewer, Operator, Maintenance, Engineer. Each command needs a minimum role (`src/core/security.c`, `s_required_role`) |
| Least privilege | The panel starts as Viewer, and returns to Viewer after 5 minutes without input |
| PIN on demand | An action that needs a higher role asks for that role's PIN, then carries on |
| Lockout | 3 wrong PINs lock sign-in for 30 seconds. Every failure is logged |
| EMERGENCY | The only command that needs no role. It can never be blocked by a dialog, a lock or a sign-in |

### Credentials
| Control | Detail |
|---|---|
| Site PINs | 6 to 8 digits; repeated digits (111111) and straight runs (123456, 987654) are refused |
| Storage | Salted, keyed hashes, 2,000 HMAC-SHA256 rounds, in `security.vault`. The vault itself is signed with HMAC-SHA256 |
| Entry | `--set-pin <role>` asks twice on the console. PINs are never accepted on the command line, so they do not reach shell history or process lists |
| Factory PINs | 1111 / 2222 / 3333 work only with `--allow-factory-pins`, and never once any site PIN exists. A refused attempt is logged |

### Keys
| Control | Detail |
|---|---|
| Device key | One 32-byte key per panel (`hmi.key`), from the operating system's random source on first start |
| Location | `%ProgramData%\Prometheus HMI` on Windows: a protected DACL allowing SYSTEM, Administrators and the HMI's account only. On Linux, `/var/lib/prometheus-hmi` (as root) or `~/.local/state/prometheus-hmi`, mode 0700 |
| Separation | Purpose keys are derived with HKDF-SHA256 (audit log, credentials). The master key is wiped from memory after use |
| Squatting | A protected folder that already exists is used only if it is a real folder owned by the HMI account, SYSTEM or Administrators; it is then protected again on every start. Otherwise the secrets stay next to the program and the journal says "Protected folder unsafe" |
| Old installations | A key or vault found next to the program is moved into the protected folder once, and the move is logged |

### Audit trail
| Control | Detail |
|---|---|
| Content | Every alarm transition, operator command, sign-in, refusal, shelve, EMERGENCY and reset |
| Integrity | Each record is chained to the previous one with HMAC-SHA256 under the audit-log key |
| Check | `industrial_hmi --verify-log alarms.log` reports the first broken or forged record |
| Loss | The in-memory journal holds 512 records between writes. An overrun would write a "RECORDS LOST" record. Records are flushed to the OS at once, and forced to the disk at least every 250 ms (at once for stop and critical events) |

### Devices on the network
| Control | Detail |
|---|---|
| Identity check | After every connect, a device must report the same vendor and product (FC43) as when it was attached, before anything is read or written. Otherwise it is blocked and the comm-loss alarm is raised |
| Strict parsing | Modbus frames are checked for length, protocol id, transaction id, unit and function. `tests/test_modbus_faults.c` sends malformed frames, wrong ids and exceptions over real sockets |
| Input limits | Setpoints are checked against the module's range before they are sent |
| Single instance | A lock file next to the key: a second HMI on the same files exits |

### Build
- C99, no heap, warnings are errors (`-Wall -Wextra -Wshadow -Werror`).
- Windows hardening: stack protector, ASLR (`--dynamicbase`, `--high-entropy-va`), DEP (`--nxcompat`).
- Release scripts publish SHA-256 checksums.

## 3. What the site must do

1. **Segment the network.** Modbus TCP has no authentication and no encryption. Anyone on the
   control network can read and write a PLC directly, without the HMI. Put the HMI and PLCs in
   their own zone behind a firewall (IEC 62443-3-3, zones and conduits).
2. **Set the site PINs** before first use, and keep them personal to the shift role.
3. **Back up `hmi.key`** offline. Without it, the audit log cannot be verified and the PIN vault
   is unreadable.
4. **Copy the audit log off the panel** regularly. A copy elsewhere is the only protection against
   the newest records being cut off the end of the file (see limitations).
5. **Run the HMI under its own account**, not an administrator account.
6. **Keep the hardwired E-stop.** EMERGENCY is a software stop.

## 4. Known security limitations

- No per-person accounts: roles share a PIN.
- PIN lockout is not kept across a restart.
- Modbus TCP itself is unprotected. The HMI cannot stop another computer from writing to a PLC.
- Records cut off the end of the newest audit file cannot be detected without an outside copy.
- No log forwarding (syslog) yet.
- The identity check trusts what a device reports about itself. A device that copies another
  device's identity is not detected.

The full list is in [Known limitations](KNOWN-LIMITATIONS.md).

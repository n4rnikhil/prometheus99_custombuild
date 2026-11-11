# ADR-0006: Keep the device key and PIN vault in a protected system folder

**Status:** accepted (Build-19)

## Context
`hmi.key` signs the audit log and the PIN vault. Earlier builds kept it next to the program, with
restricted file rights. A program folder is often copied, shared or backed up with the
application, which spreads the key.

## Decision
- **Default location:** `%ProgramData%\Prometheus HMI` on Windows, created with a protected DACL
  (SYSTEM, Administrators and the HMI account only, inherited rights removed). On Linux,
  `/var/lib/prometheus-hmi` as root, otherwise `~/.local/state/prometheus-hmi`, mode 0700
  (`hal_secure_dir()`).
- The folder is created only when the default location is actually used.
- A folder that already exists is used only if it is a real folder owned by the HMI account (or
  SYSTEM / Administrators on Windows), and is protected again on every start. On Windows any
  user can create folders in `%ProgramData%`, so one made by another account first is refused.
- A key or vault found next to the program is moved there once (copied, protected, then
  removed), and the move is logged, so a commissioned panel keeps its key and audit chain.
- `--key` and `--vault` still choose other places.

## Consequences
- Copying the program folder no longer copies the secrets.
- Back up the protected folder separately (see the Install and Operations Guide).
- Administrators can still read the key. That is outside what the HMI can control.

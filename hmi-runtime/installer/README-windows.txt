Prometheus HMI for Windows

FIRST STEPS
  1. Set the role PINs (6 to 8 digits, asked twice). In this folder, in a command prompt:
       industrial_hmi --set-pin operator
       industrial_hmi --set-pin maintenance
       industrial_hmi --set-pin engineer
  2. Open hmi.cfg in this folder and list your devices (--scan and --plc lines).
  3. Start "Prometheus HMI" from the desktop or the Start menu.

  Until the PINs are set only the Viewer role can sign in, so nothing can be operated.
  On a test bench, examples\bench.cfg allows the factory PINs (Operator 1111):
       industrial_hmi --config examples\bench.cfg

NOTES
  - The EMERGENCY button is a software stop over the network, not a certified emergency stop:
    every machine still needs its hardwired E-stop.
  - Alarm sound: a two-tone signal on the PC's speakers while an alarm is unacknowledged.
    Acknowledge or Silence horn stops it. --no-sound in hmi.cfg turns it off.
  - Opening it again while it runs brings the running window forward (one HMI at a time).
  - alarms.log is written in this folder. The device key (hmi.key) and the PIN vault
    (security.vault) are kept in %ProgramData%\Prometheus HMI, which only this account,
    SYSTEM and administrators can open. Uninstalling keeps all three.

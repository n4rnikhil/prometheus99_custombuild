Prometheus HMI for Windows

START
  Desktop or Start menu: "Prometheus HMI" opens the PLC simulator (a drive, a tank, a meter)
  and then the HMI. Sign in: tap "Viewer", choose Operator, PIN 1111.

START MENU
  Prometheus HMI (bench with simulator)   simulator + HMI together
  Prometheus HMI                          the HMI alone (reads hmi.cfg in this folder)
  PLC Simulator                           the simulator alone

NOTES
  - Bench use only: hmi.cfg allows the factory PINs. On a real plant set site PINs with
    industrial_hmi --set-pin <role> and remove --allow-factory-pins from hmi.cfg.
  - The EMERGENCY button is a software stop, not a certified emergency stop: every machine
    still needs its hardwired E-stop.
  - Alarm sound: a two-tone signal on the PC's speakers while an alarm is unacknowledged.
    Acknowledge or Silence horn stops it. --no-sound in hmi.cfg turns it off.
  - Opening it again while it runs brings the running window forward (one HMI at a time).
  - alarms.log, hmi.key and security.vault are created in this folder and kept on uninstall.

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
  - STOP ALL is a software stop, not an emergency stop.
  - alarms.log, hmi.key and security.vault are created in this folder and kept on uninstall.

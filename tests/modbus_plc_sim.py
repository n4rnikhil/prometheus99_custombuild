"""
modbus_plc_sim.py - Modbus TCP device simulator for plug-and-play testing
(Python standard library only).

  python tests/modbus_plc_sim.py --profile tank  --port 1502     # Modicon M340 tank skid PLC
  python tests/modbus_plc_sim.py --profile drive --port 1503     # Altivar ATV320 drive
  python tests/modbus_plc_sim.py --profile meter --port 1504     # PM5560 power meter (NOT a PLC)
  python tests/modbus_plc_sim.py --profile anon  --port 1505     # Modbus device without FC43

Every profile answers FC43 / MEI 0x0E "Read Device Identification" with its
vendor / product code (except "anon"), so the HMI's discovery can tell a PLC
from other Modbus devices. Supports FC01, FC03, FC05, FC06, FC43.

  --outage-at S --outage-for S   drop all connections and refuse new ones for a while
  --run-for S                    exit after S seconds (0 = run until Ctrl+C)

Data maps (0-based PDU addresses):
  tank : HR0 level (0.1 %), HR1 rpm, HR2 valve % (w), HR3 pressure kPa, HR4 temp C
         coil0 pump A (w), coil1 coolant pump B (w), coil2 E-stop latch (w)
  drive: HR0 output freq (0.1 Hz), HR1 current (0.1 A), HR2 speed setpoint (0.1 Hz, w), HR3 temp C
         coil0 run (w), coil1 fault
"""
import argparse
import socket
import struct
import threading
import time

IDENTITY = {
    "tank":  ("Schneider Electric", "BMX P34 2020", "V3.20"),
    "drive": ("Schneider Electric", "ATV320U15N4C", "V2.6"),
    "meter": ("Schneider Electric", "PM5560", "V2.7.4"),
    "anon":  None,
}

LOCK = threading.Lock()
STATS = {"requests": 0, "writes": 0, "exceptions": 0, "connections": 0, "fc43": 0}
OUTAGE = threading.Event()
HR = [0] * 16
COILS = [0] * 8
PROFILE = "tank"


def setup(profile):
    global PROFILE
    PROFILE = profile
    if profile == "tank":
        HR[:5] = [720, 1450, 65, 130, 56]
        COILS[:3] = [1, 0, 0]
    elif profile == "drive":
        HR[:4] = [500, 120, 500, 41]
        COILS[:2] = [1, 0]
    elif profile == "meter":
        HR[:4] = [2301, 2298, 2305, 4999]     # voltages, frequency
    else:
        HR[:8] = [11, 22, 33, 44, 55, 66, 77, 88]


def process():
    level = 72000
    while True:
        time.sleep(0.1)
        with LOCK:
            if PROFILE == "tank":
                if COILS[2]:
                    COILS[0] = 0
                    COILS[1] = 0
                rpm = HR[1]
                target = 1450 if COILS[0] else 0
                rpm += max(-150, min(60, target - rpm))
                HR[1] = rpm
                level = max(0, min(100000, level + 60 * rpm // 1450 - HR[2] * 9 // 10))
                HR[0] = level // 100
                HR[3] = 20 + rpm * 95 // 1450 + (100 - HR[2]) * 40 // 100
                HR[4] = 30 + rpm * 29 // 1450 - (14 if COILS[1] else 0)
            elif PROFILE == "drive":
                target = HR[2] if COILS[0] and not COILS[1] else 0
                HR[0] += max(-20, min(20, target - HR[0]))          # ramp 2 Hz per 100 ms
                HR[1] = 20 + HR[0] * 3 // 10 if HR[0] else 0         # current follows speed
                HR[3] = 30 + HR[0] // 25


def exception(tid, unit, fc, code):
    STATS["exceptions"] += 1
    return struct.pack(">HHHBBB", tid, 0, 3, unit, fc | 0x80, code)


def device_id(tid, unit):
    ident = IDENTITY[PROFILE]
    if ident is None:
        return exception(tid, unit, 0x2B, 1)                  # FC43 not supported
    STATS["fc43"] += 1
    objs = b""
    for i, text in enumerate(ident):
        raw = text.encode("ascii")
        objs += bytes([i, len(raw)]) + raw
    pdu = bytes([0x2B, 0x0E, 0x01, 0x01, 0x00, 0x00, len(ident)]) + objs
    return struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu


def handle(adu):
    tid, _proto, _length, unit = struct.unpack(">HHHB", adu[:7])
    pdu = adu[7:]
    fc = pdu[0]
    STATS["requests"] += 1
    if fc == 0x2B and len(pdu) >= 2 and pdu[1] == 0x0E:
        return device_id(tid, unit)
    with LOCK:
        if fc in (1, 3):
            addr, qty = struct.unpack(">HH", pdu[1:5])
            table = COILS if fc == 1 else HR
            if qty == 0 or addr + qty > len(table):
                return exception(tid, unit, fc, 2)
            if fc == 3:
                body = struct.pack(">B", qty * 2) + b"".join(struct.pack(">H", v & 0xFFFF) for v in HR[addr:addr + qty])
            else:
                bits = 0
                for i in range(qty):
                    bits |= (1 if COILS[addr + i] else 0) << i
                n = (qty + 7) // 8
                body = struct.pack(">B", n) + bits.to_bytes(n, "little")
            pdu_out = bytes([fc]) + body
        elif fc == 5:
            addr, val = struct.unpack(">HH", pdu[1:5])
            writable = {"tank": (0, 1, 2), "drive": (0,)}.get(PROFILE, ())
            if addr not in writable or val not in (0, 0xFF00):
                return exception(tid, unit, fc, 2 if addr not in writable else 3)
            COILS[addr] = 1 if val == 0xFF00 else 0
            if PROFILE == "tank" and addr in (0, 1) and COILS[2]:
                COILS[addr] = 0                                   # interlock: E-stop latched
            STATS["writes"] += 1
            pdu_out = pdu[:5]
        elif fc == 6:
            addr, val = struct.unpack(">HH", pdu[1:5])
            if addr != 2 or PROFILE not in ("tank", "drive"):
                return exception(tid, unit, fc, 2)
            HR[2] = max(0, min(100 if PROFILE == "tank" else 600, val))
            STATS["writes"] += 1
            pdu_out = pdu[:5]
        else:
            return exception(tid, unit, fc, 1)
    return struct.pack(">HHHB", tid, 0, len(pdu_out) + 1, unit) + pdu_out


def client(conn):
    STATS["connections"] += 1
    buf = b""
    conn.settimeout(0.2)
    try:
        while not OUTAGE.is_set():
            try:
                data = conn.recv(1024)
            except socket.timeout:
                continue
            if not data:
                break
            buf += data
            while len(buf) >= 7:
                length = struct.unpack(">H", buf[4:6])[0]
                if len(buf) < 6 + length:
                    break
                adu, buf = buf[:6 + length], buf[6 + length:]
                conn.sendall(handle(adu))
    except OSError:
        pass
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1502)
    ap.add_argument("--profile", choices=sorted(IDENTITY), default="tank")
    ap.add_argument("--outage-at", type=float, default=0)
    ap.add_argument("--outage-for", type=float, default=0)
    ap.add_argument("--run-for", type=float, default=0, help="exit after N seconds (0 = forever)")
    a = ap.parse_args()
    setup(a.profile)

    threading.Thread(target=process, daemon=True).start()
    t0 = time.time()

    def server():
        while True:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            s.bind(("127.0.0.1", a.port))
            s.listen(4)
            s.settimeout(0.2)
            while not OUTAGE.is_set():
                try:
                    c, _ = s.accept()
                except socket.timeout:
                    continue
                threading.Thread(target=client, args=(c,), daemon=True).start()
            s.close()
            while OUTAGE.is_set():
                time.sleep(0.05)

    threading.Thread(target=server, daemon=True).start()
    ident = IDENTITY[a.profile]
    print(f"{a.profile} simulator on 127.0.0.1:{a.port}  identity={ident}", flush=True)
    outage_done = False
    try:
        while True:
            time.sleep(0.05)
            el = time.time() - t0
            if a.outage_at and not outage_done and el >= a.outage_at:
                print(f"[{el:5.1f}s] outage start", flush=True)
                OUTAGE.set()
                time.sleep(a.outage_for)
                OUTAGE.clear()
                outage_done = True
                print(f"[{time.time() - t0:5.1f}s] outage end", flush=True)
            if a.run_for and el >= a.run_for:
                break
    except KeyboardInterrupt:
        pass
    print("stats:", STATS, "HR:", HR[:8], "coils:", COILS[:3], flush=True)


if __name__ == "__main__":
    main()

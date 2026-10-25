"""
modbus_plc_sim.py - a tiny Modbus TCP server that behaves like the Modicon
M340/M580 process the HMI expects (Python standard library only).

  python tests/modbus_plc_sim.py [--port 1502] [--outage-at S --outage-for S]

Data map (0-based PDU addresses, same as include/drivers/driver_modbus.h):
  HR 0 tank level (0.1 %)  HR 1 pump rpm  HR 2 valve %  HR 3 pressure kPa  HR 4 temp C
  coil 0 pump A   coil 1 coolant pump B   coil 2 E-stop latch
Supports FC01, FC03, FC05, FC06; anything else -> exception 01, bad address -> 02.
--outage-at/--outage-for drop every connection and refuse new ones for a while,
to test the HMI's timeout / reconnect path.
"""
import argparse
import socket
import struct
import threading
import time

HR = [720, 1450, 65, 130, 56, 0, 0, 0, 0, 0]
COILS = [1, 0, 0, 0, 0, 0, 0, 0]
LOCK = threading.Lock()
STATS = {"requests": 0, "writes": 0, "exceptions": 0, "connections": 0}
OUTAGE = threading.Event()


def process():
    level = 72000
    while True:
        time.sleep(0.1)
        with LOCK:
            if COILS[2]:
                COILS[0] = 0
                COILS[1] = 0
            rpm = HR[1]
            target = 1450 if COILS[0] else 0
            rpm += max(-150, min(60, target - rpm))
            HR[1] = rpm
            inflow = 60 * rpm // 1450
            outflow = HR[2] * 9 // 10
            level = max(0, min(100000, level + inflow - outflow))
            HR[0] = level // 100
            HR[3] = 20 + rpm * 95 // 1450 + (100 - HR[2]) * 40 // 100
            HR[4] = 30 + rpm * 29 // 1450 - (14 if COILS[1] else 0)


def exception(tid, unit, fc, code):
    STATS["exceptions"] += 1
    return struct.pack(">HHHBBB", tid, 0, 3, unit, fc | 0x80, code)


def handle(adu):
    tid, proto, length, unit = struct.unpack(">HHHB", adu[:7])
    pdu = adu[7:]
    fc = pdu[0]
    STATS["requests"] += 1
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
                nbytes = (qty + 7) // 8
                body = struct.pack(">B", nbytes) + bits.to_bytes(nbytes, "little")
            pdu_out = bytes([fc]) + body
        elif fc == 5:
            addr, val = struct.unpack(">HH", pdu[1:5])
            if addr >= len(COILS) or val not in (0, 0xFF00):
                return exception(tid, unit, fc, 2 if addr >= len(COILS) else 3)
            COILS[addr] = 1 if val == 0xFF00 else 0
            if addr in (0, 1) and COILS[2]:
                COILS[addr] = 0          # interlock: nothing starts while the E-stop is latched
            STATS["writes"] += 1
            pdu_out = pdu[:5]
        elif fc == 6:
            addr, val = struct.unpack(">HH", pdu[1:5])
            if addr != 2:
                return exception(tid, unit, fc, 2)   # only the valve command is writable
            HR[2] = max(0, min(100, val))
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
    ap.add_argument("--outage-at", type=float, default=0)
    ap.add_argument("--outage-for", type=float, default=0)
    ap.add_argument("--run-for", type=float, default=0, help="exit after N seconds (0 = forever)")
    a = ap.parse_args()

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
    print(f"Modbus PLC simulator on 127.0.0.1:{a.port}", flush=True)
    outage_done = False
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
    print("stats:", STATS, "HR:", HR, "coils:", COILS, flush=True)


if __name__ == "__main__":
    main()

"""Write a box's USB log to a file, with the PC's clock time on each line. Opening the port does not reset the box.
Usage (ESP-IDF python):  python tools/boxlog.py <port, such as COM5> box.log [minutes]
Reconnects automatically when the box restarts (the USB port disappears for a moment).
"""
import sys
import time

import serial

port, path = sys.argv[1], sys.argv[2]
minutes = float(sys.argv[3]) if len(sys.argv) > 3 else 60
end = time.time() + minutes * 60
buf = b""
with open(path, "a", encoding="utf-8", errors="replace", buffering=1) as out:
    out.write("%s ---- log started on %s ----\n" % (time.strftime("%H:%M:%S"), port))
    while time.time() < end:
        try:
            ser = serial.Serial()
            ser.port, ser.baudrate, ser.timeout = port, 115200, 0.5
            ser.dtr = False          # Keeps the reset and boot lines released when the port opens.
            ser.rts = False
            ser.open()
        except (serial.SerialException, OSError):
            time.sleep(0.5)
            continue
        try:
            while time.time() < end:
                data = ser.read(4096)
                if not data:
                    continue
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    out.write("%s %s\n" % (time.strftime("%H:%M:%S"), line.decode("utf-8", "replace").rstrip("\r")))
        except (serial.SerialException, OSError):
            out.write("%s ---- port closed (box restarting?) ----\n" % time.strftime("%H:%M:%S"))
            time.sleep(0.5)
        finally:
            try:
                ser.close()
            except Exception:
                pass

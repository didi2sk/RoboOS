#!/usr/bin/env python3
"""Run the kernel in QEMU raspi3b, send commands over UART and take an HDMI screenshot."""
import socket, subprocess, sys, time, os

kernel = sys.argv[1]
extra = os.environ.get("QEMU_EXTRA", "").split()
if os.environ.get("QEMU_SD"): extra += ["-drive", "file=%s,if=sd,format=raw" % os.environ["QEMU_SD"]]
cmds = sys.argv[2:] or ["help", "info", "uptime"]
ser_path, mon_path = "/tmp/rpi_ser.sock", "/tmp/rpi_mon.sock"
for p in (ser_path, mon_path):
    if os.path.exists(p): os.unlink(p)

q = subprocess.Popen(["qemu-system-aarch64", "-M", os.environ.get("QEMU_MACHINE", "raspi3b"), "-kernel", kernel,
                      "-display", "none", *extra,
                      "-serial", f"unix:{ser_path},server=on,wait=off",
                      "-monitor", f"unix:{mon_path},server=on,wait=off"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(1)
ser = socket.socket(socket.AF_UNIX); ser.connect(ser_path); ser.settimeout(0.3)
mon = socket.socket(socket.AF_UNIX); mon.connect(mon_path); mon.settimeout(0.3)

out = b""
def drain(t):
    global out
    end = time.time() + t
    while time.time() < end:
        try:
            d = ser.recv(65536)
            if d: out += d
        except socket.timeout:
            pass

drain(float(os.environ.get('BOOT_WAIT', '4')))
for c in cmds:
    if c.startswith("~"):            # ~seconds: just wait (and collect output)
        drain(float(c[1:]))
        continue
    if c.startswith("!"):            # QEMU monitor command (e.g. !sendkey a)
        for m in c[1:].split(";"):
            mon.send(m.strip().encode() + b"\n"); time.sleep(0.15)
        drain(0.5)
        continue
    for ch in c.encode() + b"\r":
        ser.send(bytes([ch])); time.sleep(0.005)
    drain(1.5)

mon.send(b"screendump /tmp/rpi_screen.ppm\n"); time.sleep(1)
q.kill()
sys.stdout.write(out.decode("utf-8", "replace"))
print("\n--- qemu ---\n" + q.stdout.read().decode(errors="replace")[-2000:])

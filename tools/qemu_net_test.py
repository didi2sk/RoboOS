#!/usr/bin/env python3
"""QEMU raspi3b + emulated USB network adapter (CDC-ECM) on QEMU user networking.
usage: tools/qemu_net_test.py kernel8.img sd.img "cmd1" "cmd2" "@get3" ...
  - each argument is typed over UART (2 s each); "@getN" fetches the board's web page N times
    from the host (port 80 of the board is forwarded to 127.0.0.1:18080; run "http on" first)
  - QEMU DHCP gives 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3 (DNS needs a resolver on the host)"""
import socket, subprocess, sys, time, os
kernel, sdimg = sys.argv[1], sys.argv[2]
ser_path = os.environ.get("SER", "/tmp/rpi_ser_net.sock")
if os.path.exists(ser_path): os.unlink(ser_path)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", kernel, "-display", "none",
                      "-drive", "file=%s,if=sd,format=raw" % sdimg,
                      "-device", "usb-kbd", "-device", "usb-net,netdev=n0",
                      "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:18080-:80",
                      "-serial", f"unix:{ser_path},server=on,wait=off", "-monitor", "none"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(1)
ser = socket.socket(socket.AF_UNIX); ser.connect(ser_path); ser.settimeout(0.2)
out = b""
def drain(t):
    global out
    end = time.time() + t
    while time.time() < end:
        try:
            d = ser.recv(65536)
            if d: out += d
        except socket.timeout: pass
def cmd(c, wait=2.0):
    for ch in c.encode() + b"\r":
        ser.send(bytes([ch])); time.sleep(0.005)
    drain(wait)
drain(7)
for c in sys.argv[3:]:
    if c.startswith("@get"):
        n = int(c[4:] or 1)
        for i in range(n):
            try:
                s = socket.create_connection(("127.0.0.1", 18080), timeout=5)
                s.sendall(b"GET / HTTP/1.0\r\nHost: x\r\n\r\n")
                body = b""
                s.settimeout(5)
                while True:
                    d = s.recv(4096)
                    if not d: break
                    body += d
                print("=== HTTP GET %d: %d bytes ===" % (i + 1, len(body)))
                if i == n - 1: print(body.decode(errors="replace"))
            except Exception as e:
                print("=== HTTP GET failed:", e)
            drain(0.5)
    else:
        cmd(c)
q.kill()
import re
print(re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", out).decode(errors="replace").replace("\r", ""))

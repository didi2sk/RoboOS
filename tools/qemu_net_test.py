#!/usr/bin/env python3
"""QEMU raspi3b + emulated USB network adapter (CDC-ECM) on QEMU user networking.
usage: tools/qemu_net_test.py kernel8.img sd.img "cmd1" "cmd2" "@get3" ...
  - each argument is typed over UART (2 s each); "@getN" fetches the board's web page N times
    from the host (port 80 of the board is forwarded to 127.0.0.1:18080; run "http on" first)
  - "@tcp:PORT:text" connects from the host to the board's TCP PORT (23 and 80 are forwarded),
    sends text ("|" = new line), prints what comes back within 1.5 s
  - "@udp:PORT:text" sends a datagram from the host to the board's UDP PORT (5000 is forwarded)
    and prints the answer
  - the host also runs a TCP server on 10.0.2.2:18000 (answers "host got: <line>") and a UDP
    listener on 10.0.2.2:15001 (prints what arrives), for testing programs that connect out
  - QEMU DHCP gives 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3 (DNS needs a resolver on the host)"""
import socket, subprocess, sys, time, os
kernel, sdimg = sys.argv[1], sys.argv[2]
ser_path = os.environ.get("SER", "/tmp/rpi_ser_net.sock")
if os.path.exists(ser_path): os.unlink(ser_path)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", kernel, "-display", "none",
                      "-drive", "file=%s,if=sd,format=raw" % sdimg,
                      "-device", "usb-kbd", "-device", "usb-net,netdev=n0",
                      "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:18080-:80,hostfwd=tcp:127.0.0.1:18023-:23,hostfwd=udp:127.0.0.1:15000-:5000",
                      "-serial", f"unix:{ser_path},server=on,wait=off", "-monitor", "none"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
import threading
def tcp_server():
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 18000)); srv.listen(4)
    while True:
        c, _ = srv.accept()
        def serve(c):
            buf = b""
            try:
                while True:
                    d = c.recv(4096)
                    if not d: break
                    buf += d
                    while b"\n" in buf:
                        l, buf = buf.split(b"\n", 1)
                        print("[host tcp] got:", l.decode(errors="replace").strip(), flush=True)
                        c.sendall(b"host got: " + l.strip() + b"\r\n")
            except OSError: pass
            c.close()
        threading.Thread(target=serve, args=(c,), daemon=True).start()
def udp_listener():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("127.0.0.1", 15001))
    while True:
        d, a = u.recvfrom(2048)
        print("[host udp] from %s:%d: %s" % (a[0], a[1], d.decode(errors="replace").strip()), flush=True)
threading.Thread(target=tcp_server, daemon=True).start()
threading.Thread(target=udp_listener, daemon=True).start()
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
    if c.startswith("@tcp:") or c.startswith("@udp:"):
        _, port, text = c.split(":", 2)
        data = text.replace("|", "\n").encode()
        try:
            if c.startswith("@tcp:"):
                s = socket.create_connection(("127.0.0.1", 18000 + int(port) if int(port) < 1000 else int(port)), timeout=5)
                s.settimeout(0.3)
                got = b""
                for part in data.split(b"\n"):
                    s.sendall(part + b"\n"); time.sleep(0.3)
                end = time.time() + 1.5
                while time.time() < end:
                    try:
                        d = s.recv(4096)
                        if not d: break
                        got += d
                    except socket.timeout: pass
                s.close()
            else:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
                s.sendto(data, ("127.0.0.1", 10000 + int(port)))
                got, _ = s.recvfrom(2048)
            print("=== %s answer: %r" % (c[:4], got.decode(errors="replace")))
        except Exception as e:
            print("=== %s failed: %s" % (c[:4], e))
        drain(0.5)
        continue
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

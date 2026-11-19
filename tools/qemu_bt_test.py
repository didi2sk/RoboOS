#!/usr/bin/env python3
"""
QEMU test of 'bt scan' (src/bt.c) without a Bluetooth chip.

QEMU raspi3b has the PL011 on its first serial port and the mini UART on the second.
At boot the console is on the PL011: this script types 'bt scan 3' there, then - once
RoboOS has moved the console to the mini UART - plays the BT controller on the same
socket: answers every HCI command with Command Complete (Read Local Name = BCM43430A1,
the firmware records from the .hcd file, ...) and, while the scan is on, sends LE
advertising reports (a named device, an Apple device, a Xiaomi thermometer with the
pvvx firmware). The console output is read from the mini UART and checked.
usage: tools/qemu_bt_test.py kernel8.img sd.img
"""
import os, socket, struct, subprocess, sys, time

kernel, sdimg = sys.argv[1], sys.argv[2]
pl, mini = "/tmp/rpi_bt_pl011.sock", "/tmp/rpi_bt_mini.sock"
for p in (pl, mini):
    if os.path.exists(p): os.unlink(p)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", kernel, "-display", "none",
                      "-drive", f"file={sdimg},if=sd,format=raw",
                      "-serial", f"unix:{pl},server=on,wait=off", "-serial", f"unix:{mini},server=on,wait=off",
                      "-monitor", "none"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
for _ in range(50):
    if os.path.exists(pl) and os.path.exists(mini): break
    time.sleep(0.1)
a = socket.socket(socket.AF_UNIX); a.connect(pl); a.settimeout(0.05)
b = socket.socket(socket.AF_UNIX); b.connect(mini); b.settimeout(0.05)

console = b""
def read(s):
    try: return s.recv(65536)
    except socket.timeout: return b""

t0 = time.time()
while time.time() - t0 < 5: console += read(a)          # boot on the PL011
a.sendall(b"bt scan 3\r")
t0 = time.time()
while b"mini UART" not in console and time.time() - t0 < 5: console += read(a)
console += read(a)

# ---- the fake controller on the PL011 ----
def event(code, params): a.sendall(bytes([4, code, len(params)]) + params)
def complete(op, status=0, ret=b""): event(0x0E, bytes([1]) + struct.pack("<H", op) + bytes([status]) + ret)

def adv(addr, addr_type, data, rssi):
    rep = bytes([0x00, addr_type]) + bytes(reversed(addr)) + bytes([len(data)]) + data + struct.pack("b", rssi)
    event(0x3E, bytes([0x02, 1]) + rep)

def ad(t, v): return bytes([len(v) + 1, t]) + v

thermo_mac = bytes.fromhex("a4c138112233")
pvvx = struct.pack("<H", 0x181A) + bytes(reversed(thermo_mac)) + struct.pack("<hHHBBB", 2153, 4512, 2950, 87, 7, 0)
reports = [
    (bytes.fromhex("c0ffee000001"), 1, ad(0x01, b"\x06") + ad(0x09, b"RoboOS-test"), -48),
    (bytes.fromhex("7a1122334455"), 1, ad(0xFF, b"\x4c\x00\x10\x05\x01\x18\x00\x00\x00"), -71),
    (thermo_mac, 0, ad(0x16, pvvx) + ad(0x09, b"ATC_112233"), -63),
]

buf, scanning, sent, ops, records = b"", False, 0, [], 0
t0 = time.time()
while time.time() - t0 < 25:
    buf += read(a)
    console += read(b)
    while len(buf) >= 4 and buf[0] == 1 and len(buf) >= 4 + buf[3]:
        op, n = struct.unpack("<H", buf[1:3])[0], buf[3]
        p, buf = buf[4:4 + n], buf[4 + n:]
        ops.append(op)
        if op == 0x0C14: complete(op, 0, b"BCM43430A1".ljust(248, b"\0"))
        elif op == 0x1009: complete(op, 0, bytes(reversed(bytes.fromhex("b827eb112233"))))
        elif op == 0x200C:
            scanning = p[0] == 1
            complete(op)
        else:
            if op in (0xFC4C, 0xFC4E): records += 1
            complete(op)
    if buf and buf[0] != 1: buf = buf[1:]
    if scanning and sent < 6:
        r = reports[sent % 3]; adv(*r); sent += 1
    if b"device(s)" in console: break
time.sleep(0.5); console += read(b)
b.sendall(b"bt\r"); time.sleep(0.5); console += read(b)
b.sendall(b"bt off\r"); time.sleep(1.0); console += read(b) + read(a)
a.sendall(b"version\r"); time.sleep(0.8); back = read(a)
q.kill()

text = console.decode("latin1")
print(text[text.find("bt scan"):])
print("--- after 'bt off', on the PL011 again:", back.decode("latin1").strip()[:120])
checks = [
    ("firmware records sent (Write RAM + Launch RAM)", records > 50),
    ("HCI reset, local name, BD_ADDR, event mask, scan", all(o in ops for o in (0x0C03, 0x0C14, 0xFC2E, 0x1009, 0x0C01, 0x200B, 0x200C))),
    ("named device listed", "RoboOS-test" in text),
    ("Apple maker decoded", "Apple" in text),
    ("Xiaomi pvvx thermometer decoded 21.5 C 45.1 %", "21.5 C 45.1 %" in text),
    ("console was on the mini UART", "Bluetooth on" in text),
    ("'bt off' gives the console back to the PL011", b"version" in back or b"robo@" in back),
]
ok = True
for name, c in checks:
    print(("OK   " if c else "FAIL ") + name); ok &= c
sys.exit(0 if ok else 1)

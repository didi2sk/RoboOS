#!/usr/bin/env python3
"""
QEMU test of Bluetooth (src/bt.c) without a Bluetooth chip.

QEMU raspi3b has the PL011 on its first serial port and the mini UART on the second. At boot the
console is on the PL011: this script types the first commands there; once RoboOS has moved the
console to the mini UART it plays the HCI controller on the PL011 socket:
  - answers every HCI command (Read Local Name = BCM43430A1, the .hcd records, LE buffer size ...)
  - sends LE advertising reports while scanning (a named device, Apple, a Xiaomi pvvx thermometer)
  - plays a phone: LE Connection Complete, then ATT over ACL - MTU, service discovery, the Nordic UART
    Service, CCCD on, "status" / "gpio 17 1" / "sensors" written to RX - and reads the notifications
  - returns ACL credits (Number Of Completed Packets) and disconnects at the end
The console (mini UART) gets 'bt', 'bt scan 2', 'bt sensors', 'gpio'. With mosquitto on the host
(port 1883) and the board on QEMU user networking, the thermometer must arrive as MQTT 'ble/<mac>'.
usage: tools/qemu_bt_test.py kernel8.img sd.img
"""
import os, shutil, socket, struct, subprocess, sys, time

kernel, sdimg = sys.argv[1], sys.argv[2]
pl, mini = "/tmp/rpi_bt_pl011.sock", "/tmp/rpi_bt_mini.sock"
for p in (pl, mini):
    if os.path.exists(p): os.unlink(p)
mq = None
if shutil.which("mosquitto_sub"):
    mq = subprocess.Popen(["mosquitto_sub", "-h", "127.0.0.1", "-t", "roboos/#", "-t", "homeassistant/#", "-v"],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", kernel, "-display", "none",
                      "-drive", f"file={sdimg},if=sd,format=raw",
                      "-device", "usb-net,netdev=n0", "-netdev", "user,id=n0",
                      "-serial", f"unix:{pl},server=on,wait=off", "-serial", f"unix:{mini},server=on,wait=off",
                      "-monitor", "none"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
for _ in range(50):
    if os.path.exists(pl) and os.path.exists(mini): break
    time.sleep(0.1)
a = socket.socket(socket.AF_UNIX); a.connect(pl); a.settimeout(0.02)
b = socket.socket(socket.AF_UNIX); b.connect(mini); b.settimeout(0.02)

def read(s):
    try: return s.recv(65536)
    except socket.timeout: return b""

console = b""
t0 = time.time()
while time.time() - t0 < 6: console += read(a)            # boot on the PL011 (DHCP too)
a.sendall(b"mqtt broker 10.0.2.2\r")
t0 = time.time()
while time.time() - t0 < 3: console += read(a)
a.sendall(b"bt on\r")
t0 = time.time()
while b"mini UART" not in console and time.time() - t0 < 5: console += read(a)

# ---------------- the fake controller ----------------
def event(code, params): a.sendall(bytes([4, code, len(params)]) + params)
def complete(op, status=0, ret=b""): event(0x0E, bytes([1]) + struct.pack("<H", op) + bytes([status]) + ret)
def ad(t, v): return bytes([len(v) + 1, t]) + v
def adv(addr, addr_type, data, rssi):
    rep = bytes([0x00, addr_type]) + bytes(reversed(addr)) + bytes([len(data)]) + data + struct.pack("b", rssi)
    event(0x3E, bytes([0x02, 1]) + rep)

thermo = bytes.fromhex("a4c138112233")
pvvx = struct.pack("<H", 0x181A) + bytes(reversed(thermo)) + struct.pack("<hHHBBB", 2153, 4512, 2950, 87, 7, 0)
reports = [(bytes.fromhex("c0ffee000001"), 1, ad(0x01, b"\x06") + ad(0x09, b"RoboOS-test"), -48),
           (bytes.fromhex("7a1122334455"), 1, ad(0xFF, b"\x4c\x00\x10\x05\x01\x18\x00\x00\x00"), -71),
           (thermo, 0, ad(0x16, pvvx) + ad(0x09, b"ATC_112233"), -63)]

HANDLE = 0x0040
def acl(cid, payload):
    l2 = struct.pack("<HH", len(payload), cid) + payload
    a.sendall(bytes([2]) + struct.pack("<HH", HANDLE, len(l2)) + l2)

ops, records, adv_cmds, scanning, sent_adv = [], 0, 0, False, 0
att_in = []                       # ATT PDUs from the board
notif = b""
buf = b""

def pump(seconds, until=None):
    global buf, scanning, sent_adv, records, adv_cmds, console, notif
    end = time.time() + seconds
    while time.time() < end:
        buf += read(a)
        console += read(b)
        while buf:
            if buf[0] == 1:
                if len(buf) < 4 or len(buf) < 4 + buf[3]: break
                op, n = struct.unpack("<H", buf[1:3])[0], buf[3]
                p, buf = buf[4:4 + n], buf[4 + n:]
                ops.append(op)
                if op == 0x0C14: complete(op, 0, b"BCM43430A1".ljust(248, b"\0"))
                elif op == 0x1009: complete(op, 0, bytes(reversed(bytes.fromhex("b827eb112233"))))
                elif op == 0x2002: complete(op, 0, struct.pack("<HB", 27, 8))
                elif op == 0x200C: scanning = p[0] == 1; complete(op)
                else:
                    if op in (0xFC4C, 0xFC4E): records += 1
                    if op == 0x200A: adv_cmds += 1
                    complete(op)
            elif buf[0] == 2:
                if len(buf) < 5: break
                h, n = struct.unpack("<HH", buf[1:5])
                if len(buf) < 5 + n: break
                d, buf = buf[5:5 + n], buf[5 + n:]
                l2, cid = struct.unpack("<HH", d[:4])
                pdu = d[4:4 + l2]
                event(0x13, bytes([1]) + struct.pack("<HH", HANDLE, 1))   # the packet went out
                if cid == 4:
                    att_in.append(pdu)
                    if pdu[0] == 0x1B: notif += pdu[3:]
            else:
                buf = buf[1:]
        if scanning and sent_adv < 9:
            adv(*reports[sent_adv % 3]); sent_adv += 1
        if until and until(): return True
    return False

pump(8, lambda: b"visible as" in console)

def att_req(pdu, expect):
    n = len(att_in)
    acl(4, pdu)
    pump(2, lambda: len(att_in) > n and att_in[-1][0] in expect)
    return att_in[-1] if len(att_in) > n else b""

# console: scan + sensors
b.sendall(b"bt scan 2\r"); pump(4, lambda: b"device(s)" in console)
b.sendall(b"bt sensors\r"); pump(2)

# ---------------- the phone ----------------
cc = bytes([0x01, 0x00]) + struct.pack("<H", HANDLE) + bytes([0x01, 0x01]) + bytes(reversed(bytes.fromhex("5a1122334466"))) + struct.pack("<HHHB", 24, 0, 400, 0)
event(0x3E, cc)
pump(1)
mtu = att_req(b"\x02\xf7\x00", (0x03,))
groups = att_req(b"\x10\x01\x00\xff\xff\x00\x28", (0x11, 0x01))
groups2 = att_req(b"\x10\x04\x00\xff\xff\x00\x28", (0x11, 0x01))
chars = att_req(b"\x08\x04\x00\x09\x00\x03\x28", (0x09, 0x01))
info = att_req(b"\x04\x09\x00\x09\x00", (0x05, 0x01))
name = att_req(b"\x0a\x03\x00", (0x0b, 0x01))
cccd = att_req(b"\x12\x09\x00\x01\x00", (0x13, 0x01))
acl(4, b"\x52\x06\x00" + b"status\n"); pump(2, lambda: b"RoboOS" in notif)
acl(4, b"\x52\x06\x00" + b"gpio 17 1"); pump(2, lambda: b"GPIO17 = 1" in notif)
acl(4, b"\x52\x06\x00" + b"sensors\n"); pump(2, lambda: b"ATC_112233" in notif)
acl(4, b"\x52\x06\x00" + b"bogus\n"); pump(1)
event(0x05, bytes([0]) + struct.pack("<H", HANDLE) + bytes([0x13]))     # the phone goes away
adv_before = adv_cmds
pump(2)
b.sendall(b"gpio\r"); pump(1.5)
b.sendall(b"bt\r"); pump(1)
pump(8)                                                   # MQTT publishes the thermometer
q.kill()
mq_out = b""
if mq:
    mq.kill(); mq_out = mq.stdout.read()

text = console.decode("latin1")
print(text[text.find("bt on"):][-2600:])
print("--- notifications to the phone ---\n" + notif.decode("latin1"))
if mq: print("--- MQTT ---\n" + "\n".join(l for l in mq_out.decode("latin1").splitlines() if "ble" in l)[:900])
checks = [
    ("firmware records + HCI set-up", records > 50 and all(o in ops for o in (0x0C03, 0x0C14, 0x2002, 0x2006, 0x2008, 0x2009, 0x200B))),
    ("console: scan lists the thermometer", "ATC_112233" in text and "21.5 C 45.1 %" in text),
    ("console: bt sensors JSON", '"temp":21.53' in text),
    ("ATT MTU 23", mtu[:1] == b"\x03" and mtu[1:3] == b"\x17\x00"),
    ("GAP service found", groups[:1] == b"\x11" and groups[2:8] == b"\x01\x00\x03\x00\x00\x18"),
    ("Nordic UART service found", groups2[:1] == b"\x11" and groups2[1] == 20 and groups2[-2:] == bytes.fromhex("406e")),
    ("characteristics found", chars[:1] == b"\x09"),
    ("CCCD found", info[:1] == b"\x05" and info[2:6] == b"\x09\x00\x02\x29"),
    ("device name readable", name[:1] == b"\x0b" and b"rpi3" in name),
    ("CCCD write answered", cccd[:1] == b"\x13"),
    ("phone: status", b"RoboOS" in notif and b"0.4" in notif),
    ("phone: gpio 17 1", b"GPIO17 = 1" in notif and "GPIO17 out  1" in text.replace("GPIO17 out 1", "GPIO17 out  1")),
    ("phone: sensors incl. the thermometer", b"ATC_112233 21.5 C" in notif),
    ("phone: unknown command answered", b"unknown command" in notif),
    ("advertising again after the phone left", adv_cmds > adv_before),
    ("bt status", "visible as" in text),
]
if mq: checks.append(("MQTT ble/<mac> + Home Assistant", b"ble/a4c138112233" in mq_out and b"homeassistant/sensor/ble_a4c138112233/temp/config" in mq_out))
ok = True
for n_, c in checks:
    print(("OK   " if c else "FAIL ") + n_); ok &= bool(c)
sys.exit(0 if ok else 1)

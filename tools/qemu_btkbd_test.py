#!/usr/bin/env python3
"""
QEMU test of the classic Bluetooth keyboard (src/bt.c HID host) without a Bluetooth chip.

The PL011 (QEMU serial 0) carries the console until 'bt on'; then this script plays the HCI controller
with a classic keyboard behind it, while the console runs on the mini UART (serial 1):
  - placeholder BD_ADDR 43:45:c0:00:1f:ac -> the board must write its own (vendor 0xFC01)
  - 'bt pair': inquiry finds the keyboard, Create Connection, Authentication Requested, Link Key Request
    (no key yet), IO Capability (the board: display only), User Passkey Notification 123456 (shown),
    Link Key Notification (saved to /BTKEYS.TXT), Authentication Complete, encryption
  - L2CAP: the board opens PSM 0x11 + 0x13, configuration both ways, SET_PROTOCOL boot
  - the keyboard types "echo kb1" + Enter -> the shell runs it
  - disconnect; the keyboard calls again (Connection Request): accepted, Link Key Reply carries the
    stored key, the keyboard opens the HID channels itself and types "echo kb2"
usage: tools/qemu_btkbd_test.py kernel8.img sd.img
"""
import os, socket, struct, subprocess, sys, time

kernel, sdimg = sys.argv[1], sys.argv[2]
pl, mini = "/tmp/rpi_btk_pl011.sock", "/tmp/rpi_btk_mini.sock"
for p in (pl, mini):
    if os.path.exists(p): os.unlink(p)
q = subprocess.Popen(["qemu-system-aarch64", "-M", "raspi3b", "-kernel", kernel, "-display", "none",
                      "-drive", f"file={sdimg},if=sd,format=raw",
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
while time.time() - t0 < 5: console += read(a)
a.sendall(b"bt on\r")
t0 = time.time()
while b"mini UART" not in console and time.time() - t0 < 5: console += read(a)

def event(code, params): a.sendall(bytes([4, code, len(params)]) + params)
def complete(op, status=0, ret=b""): event(0x0E, bytes([1]) + struct.pack("<H", op) + bytes([status]) + ret)
def cstatus(op, status=0): event(0x0F, bytes([status, 1]) + struct.pack("<H", op))
def ad(t, v): return bytes([len(v) + 1, t]) + v

KB = bytes(reversed(bytes.fromhex("54466b000010")))      # HCI order
KEY = bytes(range(0x10, 0x20))
bdaddr = bytes(reversed(bytes.fromhex("4345c0001fac")))
ops, cmds = [], {}
H = 0x0011
kb_cid = {}                                            # board's local cid per psm (as seen in its frames)
board_rcid = {}                                        # our cid per psm
hid_ready = []
sig_from_board = []
buf = b""

def acl(cid, payload, h=None):
    h = H if h is None else h
    l2 = struct.pack("<HH", len(payload), cid) + payload
    a.sendall(bytes([2]) + struct.pack("<HH", h | 0x2000, len(l2)) + l2)

def sig(code, ident, data): acl(1, bytes([code, ident]) + struct.pack("<H", len(data)) + data)

def board_sig(d):
    global sid
    i = 0
    while i + 4 <= len(d):
        code, ident, l = d[i], d[i + 1], struct.unpack("<H", d[i + 2:i + 4])[0]
        p = d[i + 4:i + 4 + l]
        sig_from_board.append((code, p))
        if code == 0x02:                              # board opens a channel: accept with our cid
            psm, scid = struct.unpack("<HH", p[:4])
            ours = 0x70 + len(kb_cid)
            kb_cid[psm] = scid; board_rcid[psm] = ours
            sig(0x03, ident, struct.pack("<HHHH", ours, scid, 0, 0))
            sig(0x04, 0x60 + ours, struct.pack("<HH", scid, 0))     # our Configuration Request
        elif code == 0x04:                            # board's Configuration Request -> success
            dcid = struct.unpack("<H", p[:2])[0]
            psm = [k for k, v in board_rcid.items() if v == dcid]
            if psm: sig(0x05, ident, struct.pack("<HHH", kb_cid[psm[0]], 0, 0))
        elif code == 0x03:                            # answer to OUR connection request (reconnect case)
            dcid, scid, res = struct.unpack("<HHH", p[:6])
            for psm, v in board_rcid.items():
                if v == scid: kb_cid[psm] = dcid
        i += 4 + l

def pump(seconds, until=None):
    global buf, console
    end = time.time() + seconds
    while time.time() < end:
        buf += read(a)
        console += read(b)
        while buf:
            if buf[0] == 1:
                if len(buf) < 4 or len(buf) < 4 + buf[3]: break
                op, n = struct.unpack("<H", buf[1:3])[0], buf[3]
                p, buf = buf[4:4 + n], buf[4 + n:]
                ops.append(op); cmds.setdefault(op, []).append(p)
                on_cmd(op, p)
            elif buf[0] == 2:
                if len(buf) < 5: break
                h, n = struct.unpack("<HH", buf[1:5])
                if len(buf) < 5 + n: break
                d, buf = buf[5:5 + n], buf[5 + n:]
                event(0x13, bytes([1]) + struct.pack("<HH", h & 0xFFF, 1))
                l2, cid = struct.unpack("<HH", d[:4])
                pl_ = d[4:4 + l2]
                if cid == 1: board_sig(pl_)
                elif pl_ == b"\x70":                  # SET_PROTOCOL boot on the control channel
                    hid_ready.append(cid)
                    acl(kb_cid[0x11] if False else board_cid_for(0x11), b"\x00")   # HANDSHAKE success
            else:
                buf = buf[1:]
        if until and until(): return True
    return False

def board_cid_for(psm): return kb_cid[psm]

def on_cmd(op, p):
    global bdaddr
    if op == 0x0C14: complete(op, 0, b"BCM43430A1".ljust(248, b"\0"))
    elif op == 0x1009: complete(op, 0, bdaddr)
    elif op == 0xFC01: bdaddr = p[:6]; complete(op)
    elif op == 0x1005: complete(op, 0, struct.pack("<HBHH", 1021, 64, 8, 0))
    elif op == 0x2002: complete(op, 0, struct.pack("<HB", 27, 8))
    elif op == 0x0401:                                # inquiry: the keyboard answers
        cstatus(op)
        eir = (ad(0x09, b"BT3.0 Keyboard") + bytes(240))[:240]
        event(0x2F, bytes([1]) + KB + bytes([1, 0]) + bytes([0x40, 0x05, 0x00]) + struct.pack("<H", 0) + struct.pack("b", -60) + eir)
    elif op == 0x0405: cstatus(op); event(0x03, bytes([0]) + struct.pack("<H", H) + p[:6] + bytes([1, 0]))
    elif op == 0x0411:                                # Authentication Requested
        cstatus(op); event(0x17, KB)                  # Link Key Request
    elif op == 0x040C:                                # no key: Secure Simple Pairing
        complete(op, 0, KB); event(0x31, KB)          # IO Capability Request
    elif op == 0x040B:                                # stored key: authenticated
        complete(op, 0, KB); event(0x06, bytes([0]) + struct.pack("<H", H))
    elif op == 0x042B:
        complete(op, 0, KB)
        event(0x32, KB + bytes([0x02, 0, 0x05]))      # the keyboard: keyboard only
        event(0x3B, KB + struct.pack("<I", 123456))   # User Passkey Notification
        delays.append((time.time() + 0.5, "paired"))
    elif op == 0x0413: cstatus(op); event(0x08, bytes([0]) + struct.pack("<H", H) + bytes([1]))
    elif op == 0x0409:                                # Accept Connection Request
        cstatus(op); event(0x03, bytes([0]) + struct.pack("<H", H) + KB + bytes([1, 0]))
    elif op in (0x0401, 0x0C03) or True: complete(op)

delays = []
def run_delays():
    for t, what in list(delays):
        if time.time() >= t:
            delays.remove((t, what))
            if what == "paired":
                event(0x36, bytes([0]) + KB)
                event(0x18, KB + KEY + bytes([5]))    # Link Key Notification
                event(0x06, bytes([0]) + struct.pack("<H", H))

def pump2(seconds, until=None):
    end = time.time() + seconds
    while time.time() < end:
        if pump(0.05, until): return True
        run_delays()
    return False

KEYS = {"e": 0x08, "c": 0x06, "h": 0x0B, "o": 0x12, " ": 0x2C, "k": 0x0E, "b": 0x05, "1": 0x1E, "2": 0x1F, "\r": 0x28}
def type_text(t):
    for ch in t:
        acl(board_cid_for(0x13), b"\xa1" + bytes([0, 0, KEYS[ch], 0, 0, 0, 0, 0])); pump2(0.04)
        acl(board_cid_for(0x13), b"\xa1" + bytes(8)); pump2(0.04)

pump2(6, lambda: b"visible as" in console)
b.sendall(b"bt pair\r")
pump2(25, lambda: b"keyboard ready" in console)
pump2(3, lambda: console.rstrip().endswith(b"$") or console.rstrip().endswith(b"$\x1b[0m"))   # the shell prompt is back
pump2(0.3)
type_text("echo kb1\r")
pump2(2, lambda: b"\nkb1" in console.replace(b"\r", b""))
b.sendall(b"cat /BTKEYS.TXT\r"); pump2(2)
b.sendall(b"bt\r"); pump2(1.5)
# the keyboard goes away and comes back by itself
event(0x05, bytes([0]) + struct.pack("<H", H) + bytes([0x13]))
pump2(1.5)
H = 0x0012
kb_cid.clear(); board_rcid.clear()
event(0x04, KB + bytes([0x40, 0x05, 0x00]) + bytes([1]))      # Connection Request
pump2(3, lambda: 0x040B in ops)
# the keyboard opens HID control + interrupt itself
def kb_open(psm, ours):
    board_rcid[psm] = ours
    sig(0x02, ours, struct.pack("<HH", psm, ours))
    pump2(1, lambda: psm in kb_cid)
for _ in range(20):
    if 0x0413 in ops: break
    pump2(0.1)
pump2(0.3)
# record the board's cids from its Connection Responses
orig = board_sig
def board_sig2(d):
    i = 0
    while i + 4 <= len(d):
        code, ident, l = d[i], d[i + 1], struct.unpack("<H", d[i + 2:i + 4])[0]
        p = d[i + 4:i + 4 + l]
        sig_from_board.append((code, p))
        if code == 0x03:
            dcid, scid = struct.unpack("<HH", p[:4])
            for psm, v in board_rcid.items():
                if v == scid: kb_cid[psm] = dcid
        elif code == 0x04:
            dcid = struct.unpack("<H", p[:2])[0]
            psm = [k for k, v in board_rcid.items() if v == dcid]
            if psm: sig(0x05, ident, struct.pack("<HHH", kb_cid[psm[0]], 0, 0))
        i += 4 + l
board_sig = board_sig2
for psm, ours in ((0x11, 0x90), (0x13, 0x91)):
    kb_open(psm, ours)
    sig(0x04, 0x30 + ours, struct.pack("<HH", kb_cid[psm], 0))   # our Configuration Request
    pump2(0.5)
pump2(3, lambda: console.count(b"keyboard ready") >= 2)
type_text("echo kb2\r")
pump2(2, lambda: b"\nkb2" in console.replace(b"\r", b""))
q.kill()

text = console.decode("latin1").replace("\r", "")
print(text[text.find("bt pair"):][-2500:])
reply = cmds.get(0x040B, [b""])[0]
caps = cmds.get(0x042B, [b""])[0]
checks = [
    ("placeholder address replaced (0xFC01, b8:27:eb)", 0xFC01 in ops and cmds[0xFC01][0][3:] == bytes([0xEB, 0x27, 0xB8])),
    ("SSP on, page scan on", 0x0C56 in ops and cmds.get(0x0C1A, [b""])[0] == b"\x02"),
    ("inquiry found the keyboard, connection to it", 0x0405 in ops and cmds[0x0405][0][:6] == KB),
    ("authentication requested", 0x0411 in ops),
    ("no key at first: negative reply", 0x040C in ops),
    ("IO capability: display only", caps[6:7] == b"\x00"),
    ("passkey shown", "type  123456  and press Enter" in text),
    ("encryption switched on", 0x0413 in ops),
    ("HID channels 0x11 + 0x13 + SET_PROTOCOL boot", len(hid_ready) >= 1),
    ("keyboard ready", "keyboard ready: 54:46:6b:00:00:10" in text),
    ("typed on the BT keyboard: echo kb1", "\nkb1\n" in text),
    ("link key saved to /BTKEYS.TXT", "54:46:6b:00:00:10 101112131415161718191a1b1c1d1e1f" in text),
    ("reconnect accepted", 0x0409 in ops),
    ("stored key given back", reply == KB + KEY),
    ("typed after the reconnect: echo kb2", "\nkb2\n" in text),
]
ok = True
for n_, c in checks:
    print(("OK   " if c else "FAIL ") + n_); ok &= bool(c)
sys.exit(0 if ok else 1)

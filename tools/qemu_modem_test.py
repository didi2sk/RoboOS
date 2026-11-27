#!/usr/bin/env python3
"""
QEMU test of the GSM modem driver (src/modem.c, 0.144) without a modem.

QEMU raspi3b: the PL011 is serial 0 (the console - QEMU does not model the pin mux, so the console keeps working
after the modem takes GPIO14/15), the mini UART is serial 1 = the modem. This script plays a SIM800L there:
answers the AT commands, takes AT+CMGS texts (> prompt, Ctrl+Z), dials, and sends an incoming SMS (+CMTI ->
AT+CMGR) with "DISARM 1234" from a listed number and "ARM 1234" from a stranger.
usage: tools/qemu_modem_test.py kernel8.img sd.img
"""
import os, socket, subprocess, sys, time

kernel, sdimg = sys.argv[1], sys.argv[2]
pl, mini = "/tmp/rpi_modem_pl011.sock", "/tmp/rpi_modem_mini.sock"
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

console = b""
mbuf = b""
sms_out = []          # (number, text)
calls = []
stored = {}           # index -> (from, text)
in_text = None        # number while the board types an SMS text
log = []

def mreply(s): b.sendall(s.encode())

def modem_line(l):
    global in_text
    log.append(l)
    if l == "AT" or l in ("ATE0", "AT+CMEE=2", "AT+CMGF=1", 'AT+CSCS="GSM"', "AT+CNMI=2,1,0,0,0", "AT+CLIP=1",
                         "AT+COPS=3,0", "AT+CMGD=1,4", "ATH", "AT&W"):
        mreply("\r\nOK\r\n")
    elif l == "ATI": mreply("\r\nSIM800 R14.18\r\n\r\nOK\r\n")
    elif l == "AT+CPIN?": mreply("\r\n+CPIN: READY\r\n\r\nOK\r\n")
    elif l == "AT+CSQ": mreply("\r\n+CSQ: 20,0\r\n\r\nOK\r\n")
    elif l == "AT+CREG?": mreply("\r\n+CREG: 0,1\r\n\r\nOK\r\n")
    elif l == "AT+COPS?": mreply('\r\n+COPS: 0,0,"Orange SK"\r\n\r\nOK\r\n')
    elif l.startswith('AT+CMGS="'):
        in_text = l[9:-1]
        mreply("\r\n> ")
    elif l.startswith("ATD"):
        calls.append(l[3:].rstrip(";"))
        mreply("\r\nOK\r\n")
    elif l.startswith("AT+CMGR="):
        i = int(l[8:])
        f, t = stored[i]
        mreply(f'\r\n+CMGR: "REC UNREAD","{f}","","26/11/27,10:00:00+04"\r\n{t}\r\n\r\nOK\r\n')
    elif l.startswith("AT+CMGD="):
        mreply("\r\nOK\r\n")
    else:
        mreply("\r\nERROR\r\n")

def pump(sec):
    global console, mbuf, in_text
    end = time.time() + sec
    while time.time() < end:
        try:
            d = a.recv(65536)
            console += d
        except socket.timeout: pass
        try:
            d = b.recv(65536)
        except socket.timeout: d = b""
        for ch in d:
            c = bytes([ch])
            if in_text is not None:
                if c == b"\x1a":
                    sms_out.append((in_text, mbuf.decode(errors="replace")))
                    mbuf = b""; in_text = None
                    mreply("\r\n+CMGS: 7\r\n\r\nOK\r\n")
                else: mbuf += c
                continue
            if c == b"\r":
                l = mbuf.decode(errors="replace").strip(); mbuf = b""
                if l: modem_line(l)
            elif c != b"\n": mbuf += c

def cmd(s, wait=2):
    a.sendall(s.encode() + b"\r")
    pump(wait)

pump(6)
cmd("alarm off"); cmd("modem off"); cmd("alarm on")
cmd("modem number add +421900111222")
cmd("modem on", 6)
cmd("modem", 2)
cmd("alarm arm 1234", 3)
stored[1] = ("+421900111222", "DISARM 1234")
mreply('\r\n+CMTI: "SM",1\r\n'); pump(5)
stored[2] = ("+421999888777", "ARM 1234")
mreply('\r\n+CMTI: "SM",2\r\n'); pump(4)
cmd("alarm panic", 6)
cmd("alarm disarm 1234", 4)
cmd("modem at AT+CSQ", 3)
cmd("modem", 2)
cmd("modem off", 2)
cmd("alarm off", 2)
q.kill()

txt = console.decode(errors="replace")
checks = [
    ("modem probed + set up", "AT+CNMI=2,1,0,0,0" in log),
    ("status shows the network", "Orange SK" in txt and "-73 dBm" in txt),
    ("SMS on arm", any("armed by keyboard" in t for n, t in sms_out)),
    ("SMS DISARM from a listed number ran", "DISARMED by SMS +421900111222" in txt or "disarmed by SMS" in " ".join(t for n, t in sms_out)),
    ("reply SMS to the sender", any(n == "+421900111222" and "Now:" in t for n, t in sms_out)),
    ("stranger ignored", "not in the list" in txt and not any(n == "+421999888777" for n, t in sms_out)),
    ("SMS on alarm", any("ALARM" in t for n, t in sms_out)),
    ("call on alarm", "+421900111222" in calls),
    ("raw AT", "+CSQ: 20,0" in txt),
]
for n, t in sms_out: print("SMS ->", n, ":", t)
print("calls:", calls)
bad = 0
for name, ok in checks:
    print(("OK   " if ok else "FAIL ") + name)
    bad += not ok
print(txt[-2500:])
sys.exit(1 if bad else 0)

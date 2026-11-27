#!/usr/bin/env python3
"""
Test receiver for SIA DC-09 (RoboOS pco.c, 0.146): TCP + UDP, checks CRC-16/ARC, the length field and the layout
(the regexes follow pysiaalarm), decrypts AES-CBC (IV 0), answers ACK, and on request NAK (with its own time) or DUH.
Every message goes to the log file as one JSON line.

usage: tools/dc09_receiver.py <tcp port> <udp port> <log file> [key hex|text:...] [nak_first] [duh=<code>]
"""
import json, re, socket, sys, threading, time
from datetime import datetime, timezone
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

tport, uport, logf = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
key = None
opts = sys.argv[4:]
for o in opts:
    if o.startswith("text:"): key = o[5:].encode()
    elif re.fullmatch(r"[0-9A-Fa-f]{32}|[0-9A-Fa-f]{48}|[0-9A-Fa-f]{64}", o): key = bytes.fromhex(o)
nak_first = "nak_first" in opts
duh = [o[4:] for o in opts if o.startswith("duh=")]
naked = set()
lock = threading.Lock()

MAIN = re.compile(r'(?P<crc>[A-Fa-f0-9]{4})(?P<length>[A-Fa-f0-9]{4})"(?P<enc>[*])?(?P<id>SIA-DCS|ADM-CID|NULL)"'
                  r'(?P<seq>[0-9]{4})(?P<rcv>R[A-Fa-f0-9]{1,6})?(?P<line>L[A-Fa-f0-9]{1,6})#(?P<acct>[A-Fa-f0-9]{3,16})\[(?P<rest>.*)$')
ADM = re.compile(r'^#(?P<acct>[A-F0-9]{3,16})\|(?P<q>\d)(?P<code>\d{3}) (?P<part>\d{2}) (?P<zone>\d{3})$')
SIA = re.compile(r'^#(?P<acct>[A-F0-9]{3,16})\|Nri(?P<ri>\d+)/(?P<code>[A-Z]{2})(?P<zone>\d{3})$')
TS = re.compile(r'^_(\d{2}:\d{2}:\d{2},\d{2}-\d{2}-\d{4})$')

def crc(s):
    c = 0
    for b in s.encode():
        t = b
        for _ in range(8):
            t ^= c & 1; c >>= 1
            if t & 1: c ^= 0xA001
            t >>= 1
    return "%04X" % c

def aes(data, dec):
    c = Cipher(algorithms.AES(key), modes.CBC(bytes(16)))
    x = c.decryptor() if dec else c.encryptor()
    return x.update(data) + x.finalize()

def stamp():
    return datetime.now(timezone.utc).strftime("_%H:%M:%S,%m-%d-%Y")

def frame(res):
    return ("\n" + crc(res) + "%04X" % len(res) + res + "\r").encode()

def handle(raw, proto):
    rec = {"proto": proto, "raw": raw}
    s = raw.strip("\n\r")
    m = MAIN.match(s)
    if not m:
        rec["error"] = "layout"; log(rec); return None
    body = s[8:]
    rec.update(crc_ok=crc(body) == m["crc"].upper(), len_ok=int(m["length"], 16) == len(body),
               id=m["id"], enc=bool(m["enc"]), seq=m["seq"], rcv=m["rcv"], line=m["line"], acct=m["acct"])
    rest = m["rest"]
    if m["enc"]:
        if not key: rec["error"] = "encrypted, no key"; log(rec); return None
        try:
            pt = aes(bytes.fromhex(rest), True).decode("ascii")
        except Exception as e:
            rec["error"] = "decrypt " + str(e); log(rec); return None
        rec["plain"] = pt
        if "|" in pt: pad, after = pt.split("|", 1)
        elif m["id"] == "NULL" and "]" in pt: pad, after = pt.split("]", 1); after = "]" + after
        else: rec["error"] = "no pad separator"; log(rec); return None
        rec["pad_ok"] = all(ch not in "|[]" for ch in pad) and 1 <= len(pad) <= 16
        data, _, ts = after.partition("]")
        data = "#" + data[1:] if data.startswith("#") else data
    else:
        data, _, ts = rest.partition("]")
    rec["ts"] = ts
    rec["ts_ok"] = ts == "" or bool(TS.match(ts))
    if ts:
        t = datetime.strptime(ts[1:], "%H:%M:%S,%m-%d-%Y").replace(tzinfo=timezone.utc)
        rec["skew_s"] = round((t - datetime.now(timezone.utc)).total_seconds())
    if m["id"] == "NULL":
        rec["data_ok"] = data == ""
    else:
        d = (ADM if m["id"] == "ADM-CID" else SIA).match(data)
        rec["data_ok"] = bool(d)
        if d: rec["event"] = d.groupdict()
    rec["data"] = data
    # the answer
    seqkey = (m["seq"], m["acct"])
    if nak_first and m["id"] != "NULL" and seqkey not in naked:
        naked.add(seqkey)
        typ = "NAK"
    elif any(c in data for c in duh):
        typ = "DUH"
    else:
        typ = "ACK"
    rec["answer"] = typ
    if typ == "NAK":
        res = f'"NAK"0000R0L0A0[]{stamp()}'
    elif m["enc"] and typ != "DUH":
        pt = "]" + stamp()
        pt = pt.zfill(len(pt) + 16 - len(pt) % 16)
        res = f'"*{typ}"{m["seq"]}{m["rcv"] or ""}{m["line"]}#{m["acct"]}[' + aes(pt.encode(), False).hex().upper()
    else:
        res = f'"{typ}"{m["seq"]}{m["rcv"] or ""}{m["line"]}#{m["acct"]}[]'
    log(rec)
    return frame(res)

def log(rec):
    with lock:
        with open(logf, "a") as f: f.write(json.dumps(rec) + "\n")

def tcp_client(c):
    buf = b""
    while True:
        try: d = c.recv(4096)
        except OSError: break
        if not d: break
        buf += d
        while b"\r" in buf:
            one, buf = buf.split(b"\r", 1)
            a = handle(one.decode(errors="replace"), "tcp")
            if a: c.sendall(a)
    c.close()

def tcp_server():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", tport)); s.listen(4)
    while True:
        c, _ = s.accept()
        threading.Thread(target=tcp_client, args=(c,), daemon=True).start()

def udp_server():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("0.0.0.0", uport))
    while True:
        d, a = u.recvfrom(2048)
        r = handle(d.decode(errors="replace"), "udp")
        if r: u.sendto(r, a)

threading.Thread(target=tcp_server, daemon=True).start()
threading.Thread(target=udp_server, daemon=True).start()
while True: time.sleep(3600)

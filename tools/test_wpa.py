#!/usr/bin/env python3
"""Plays a WPA2-PSK access point (Python hashlib / hmac / cryptography, independent of BearSSL)
against tools/test_wpa.c + src/wpa.c: 4-way handshake, group rekey, wrong password, replay.
usage: tools/test_wpa.py <path to the test_wpa binary>"""
import hashlib, hmac, os, struct, subprocess, sys
from cryptography.hazmat.primitives.keywrap import aes_key_wrap

PASS, SSID = "tajneheslo123", "iot"
AA, SPA = bytes.fromhex("0cea14403cb6"), bytes.fromhex("b827ebb44cdf")

def prf(k, label, data, n):
    out, i = b"", 0
    while len(out) < n:
        out += hmac.new(k, label.encode() + b"\0" + data + bytes([i]), hashlib.sha1).digest(); i += 1
    return out[:n]

def ptk_of(pmk, anonce, snonce):
    return prf(pmk, "Pairwise key expansion", min(AA, SPA) + max(AA, SPA) + min(anonce, snonce) + max(anonce, snonce), 48)

def eapol_key(ki, replay, nonce=b"\0" * 32, rsc=b"\0" * 8, data=b"", kck=None):
    body = bytes([2]) + struct.pack(">HH", ki, 16) + struct.pack(">Q", replay) + nonce + b"\0" * 16 + rsc + b"\0" * 8 + b"\0" * 16 + struct.pack(">H", len(data)) + data
    e = bytes([2, 3]) + struct.pack(">H", len(body)) + body
    if kck:
        mic = hmac.new(kck, e, hashlib.sha1).digest()[:16]
        e = e[:4 + 77] + mic + e[4 + 93:]
    return SPA + AA + b"\x88\x8e" + e

def check_mic(frame, kck):
    e = frame[14:]
    z = e[:4 + 77] + b"\0" * 16 + e[4 + 93:]
    return hmac.compare_digest(hmac.new(kck, z, hashlib.sha1).digest()[:16], e[4 + 77:4 + 93])

p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def cmd(s):
    p.stdin.write(s + "\n"); p.stdin.flush(); return p.stdout.readline().split()

ok = True
def expect(cond, what):
    global ok
    print(("OK   " if cond else "FAIL ") + what); ok &= bool(cond)

def handshake(ap_pass, gtk_len=16):
    pmk = hashlib.pbkdf2_hmac("sha1", ap_pass.encode(), SSID.encode(), 4096, 32)
    assert cmd(f"start {PASS} {SSID} {SPA.hex()}") == ["ok"]
    anonce = os.urandom(32)
    r = cmd("rx " + eapol_key(0x008A, 1, anonce).hex())                     # msg 1: pairwise, ack
    expect(r[0] == "used", "message 1 accepted")
    m2 = bytes.fromhex(r[1])
    expect(m2[:6] == AA and m2[6:12] == SPA and m2[12:14] == b"\x88\x8e", "message 2 addressed to the AP")
    snonce = m2[14 + 4 + 13:14 + 4 + 45]
    ptk = ptk_of(pmk, anonce, snonce)
    kck, kek, tk = ptk[:16], ptk[16:32], ptk[32:]
    mic_ok = check_mic(m2, kck)
    ki2 = struct.unpack(">H", m2[19:21])[0]
    ie = m2[14 + 4 + 95:]
    if ap_pass == PASS:
        expect(mic_ok, "message 2 MIC verifies with the AP's own PTK")
        expect(ki2 == 0x010A, "message 2 key info 0x010a")
        expect(ie[:1] == b"\x30", "message 2 carries the RSN element")
    gtk = os.urandom(gtk_len)
    kde = b"\xdd" + bytes([6 + gtk_len]) + b"\x00\x0f\xac\x01" + bytes([1, 0]) + gtk
    kd = b"\x30\x14" + bytes(20) + kde                                     # AP RSN element + GTK KDE
    kd += b"\xdd" + bytes((8 - (len(kd) + 1) % 8) % 8) if len(kd) % 8 else b""
    while len(kd) % 8: kd += b"\0"
    if len(kd) < 16: kd += bytes(16 - len(kd))
    wrapped = aes_key_wrap(kek, kd)
    m3 = eapol_key(0x13CA, 2, anonce, rsc=bytes([5, 0, 0, 0, 0, 0, 0, 0]), data=wrapped, kck=kck)   # install, ack, mic, secure, enc
    r = cmd("rx " + m3.hex())
    return r, kck, kek, tk, gtk, anonce

# 1. right password
r, kck, kek, tk, gtk, anonce = handshake(PASS)
expect(r[0] == "used", "message 3 accepted")
if r[0] == "used":
    m4 = bytes.fromhex(r[1])
    expect(check_mic(m4, kck), "message 4 MIC verifies")
    expect(struct.unpack(">H", m4[19:21])[0] == 0x030A, "message 4 key info 0x030a")
    expect(r[2] == "ptk=1" and r[3] == "gtk=1", "keys ready to install")
    expect(r[4] == "1" and r[5] == gtk.hex(), "GTK unwrapped, key index 1")
    expect(r[6] == tk.hex(), "TK equals the AP's TK")
# 2. replayed message 3 is dropped
m3 = eapol_key(0x13CA, 2, anonce, data=aes_key_wrap(kek, bytes(24)), kck=kck)
r2 = cmd("rx " + m3.hex())
expect(r2[0] == "drop" and "replay" in " ".join(r2), "replayed message 3 dropped")
# 3. group rekey (TKIP group key, 32 bytes)
g2 = os.urandom(32)
kd = b"\xdd" + bytes([6 + 32]) + b"\x00\x0f\xac\x01" + bytes([2, 0]) + g2
while len(kd) % 8: kd += b"\xdd" if len(kd) == 40 else b"\0"
r = cmd("rx " + eapol_key(0x1382, 3, data=aes_key_wrap(kek, kd), kck=kck).hex())   # enc, mic, secure, ack, group
expect(r[0] == "used" and r[3] == "gtk=1" and r[4] == "2" and r[5] == g2.hex(), "group rekey: new GTK, index 2")
if r[0] == "used":
    gm2 = bytes.fromhex(r[1])
    expect(check_mic(gm2, kck) and struct.unpack(">H", gm2[19:21])[0] == 0x0302, "group message 2 MIC + key info 0x0302")
# 4. tampered message: MIC must fail
bad = bytearray(eapol_key(0x1382, 4, data=aes_key_wrap(kek, kd), kck=kck)); bad[-1] ^= 1
r = cmd("rx " + bytes(bad).hex())
expect(r[0] == "drop" and "MIC" in " ".join(r), "tampered frame dropped (MIC)")
# 5. wrong password on the AP side: message 3 MIC fails
r, *_ = handshake("ineheslo99")
expect(r[0] == "drop" and "MIC" in " ".join(r), "different password: message 3 dropped (MIC wrong)")
p.stdin.close(); p.wait()
print("ALL OK" if ok else "FAILURES"); sys.exit(0 if ok else 1)

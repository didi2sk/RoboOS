#!/usr/bin/env python3
"""Print the Raspberry Pi camera receiver (Unicam CSI1) registers while a camera runs under Linux,
in the same format as RoboOS 'cam dump' - to compare a working setup with RoboOS.

Pi 3 / Zero 2 W (BCM2837). Run as root while the camera streams, e.g.:
    raspistill -t 0 &          (legacy camera stack)   or   rpicam-hello -t 0 &   (libcamera)
    sudo python3 camdump.py
Only READS registers (through /dev/mem, mapped read-write only so that 32-bit loads work)."""
import ctypes, mmap, os, struct, sys, time

def periph_base():
    try:
        r = open('/proc/device-tree/soc/ranges', 'rb').read()
        b = struct.unpack('>I', r[4:8])[0]
        return b if b else struct.unpack('>I', r[8:12])[0]
    except OSError:
        return 0x3F000000

base = periph_base()
try:
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
except PermissionError:
    sys.exit('run it with sudo')
m = mmap.mmap(fd, 0x803000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)

def rd(off):
    # one 32-bit load (a Python slice copies 16 bytes at a time, which returns the first register 4x)
    return ctypes.c_uint32.from_buffer(m, off).value

UNI = 0x801000
offs = [0x000,0x004,0x008,0x00C,0x010,0x014,0x018,0x01C,0x020,0x024,0x028,0x02C,0x030,0x034,0x038,
        0x100,0x104,0x108,0x10C,0x110,0x114,0x118,0x11C,0x120,0x124,0x128,0x12C,0x130,0x134,
        0x200,0x300,0x400]
print('peripherals at 0x%x' % base)
line = []
for i, o in enumerate(offs):
    line.append('%03x=%08x' % (o, rd(UNI + o)))
    if i % 6 == 5:
        print('  '.join(line)); line = []
if line:
    print('  '.join(line))
print('CM_CAM1CTL=%08x CM_CAM1DIV=%08x gate=%08x' % (rd(0x101048), rd(0x10104C), rd(0x802004)))
wp = []
for _ in range(5):
    wp.append(rd(UNI + 0x11C)); time.sleep(0.013)
print('write pointer moving:', 'yes' if len(set(wp)) > 1 else 'no', ' '.join('%08x' % w for w in wp))

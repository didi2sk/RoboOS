#!/usr/bin/env python3
"""Decodes the pictures written by tools/test_jpeg.c with PIL and compares them with the originals."""
import sys, numpy as np
from PIL import Image
d = sys.argv[1] if len(sys.argv) > 1 else '/tmp'
limits = {"jt_grad": 32, "jt_grad_q30": 26, "jt_small": 28, "jt_noise": 40, "jt_black": 60}
ok = True
for name, lim in limits.items():
    im = Image.open(f'{d}/{name}.jpg'); im.load()
    w, h = im.size
    a = np.asarray(im.convert('RGB'), dtype=np.float64)
    b = np.fromfile(f'{d}/{name}.raw', dtype=np.uint8).reshape(h, w, 3).astype(np.float64)
    if name == 'jt_noise':      # 4:2:0 noise cannot survive - check luma only
        a = a @ [0.299, 0.587, 0.114]; b = b @ [0.299, 0.587, 0.114]
    mse = ((a - b) ** 2).mean()
    psnr = 99 if mse == 0 else 10 * np.log10(255 * 255 / mse)
    good = psnr >= lim
    ok &= good
    print(f'{name}: {w}x{h} PSNR {psnr:.1f} dB (need {lim}) {"OK" if good else "FAIL"}')
print('ALL OK' if ok else 'FAILED')
sys.exit(0 if ok else 1)

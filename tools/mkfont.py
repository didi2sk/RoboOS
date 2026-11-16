# Render DejaVu Sans Mono into an 8x16 bitmap font (ASCII 32..126) as a C array.
import sys
from PIL import Image, ImageDraw, ImageFont
W,H=8,16
f=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",13)
out=["/* 8x16 font rendered from DejaVu Sans Mono (Bitstream Vera license) */",
     "const unsigned char font8x16[95][16] = {"]
for c in range(32,127):
    im=Image.new("L",(W,H),0); d=ImageDraw.Draw(im)
    d.text((0,-1),chr(c),font=f,fill=255)
    rows=[]
    for y in range(H):
        b=0
        for x in range(W):
            if im.getpixel((x,y))>100: b|=0x80>>x
        rows.append("0x%02x"%b)
    out.append("  {"+",".join(rows)+"}, /* %r */"%chr(c))
out.append("};")
open(sys.argv[1],"w").write("\n".join(out)+"\n")

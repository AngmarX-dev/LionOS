#!/usr/bin/env python3
"""Convert LionOS-Wallpaper.png into a compact RGB565 framebuffer asset."""
import struct,sys,zlib
from pathlib import Path
SRC=Path("LionOS-Wallpaper.png")
OUT=Path(sys.argv[1]) if len(sys.argv)>1 else Path("build/lion_wallpaper.h")
W,H=960,540
SIG=b"\x89PNG\r\n\x1a\n"
def paeth(a,b,c):
    p=a+b-c; pa=abs(p-a);pb=abs(p-b);pc=abs(p-c)
    return a if pa<=pb and pa<=pc else b if pb<=pc else c
def read_png(path):
    d=path.read_bytes(); assert d.startswith(SIG)
    pos=8; ih=None; ids=bytearray(); pal=None; trns=None
    while pos<len(d):
        n=struct.unpack(">I",d[pos:pos+4])[0]; t=d[pos+4:pos+8]; q=d[pos+8:pos+8+n]; pos+=12+n
        if t==b'IHDR': ih=struct.unpack(">IIBBBBB",q)
        elif t==b'PLTE': pal=[tuple(q[i:i+3]) for i in range(0,len(q),3)]
        elif t==b'tRNS': trns=q
        elif t==b'IDAT': ids+=q
        elif t==b'IEND': break
    if not ih: raise ValueError("missing IHDR")
    sw,sh,depth,ct,comp,filt,inter=ih
    if depth!=8 or inter!=0 or comp!=0 or filt!=0 or ct not in (2,3,6): raise ValueError("unsupported PNG")
    ch={2:3,3:1,6:4}[ct]; raw=zlib.decompress(ids); stride=sw*ch; rows=[]; prev=bytearray(stride); pos=0
    for _ in range(sh):
        ft=raw[pos];pos+=1; cur=bytearray(raw[pos:pos+stride]);pos+=stride
        for i in range(stride):
            l=cur[i-ch] if i>=ch else 0; u=prev[i]; ul=prev[i-ch] if i>=ch else 0
            if ft==1: cur[i]=(cur[i]+l)&255
            elif ft==2: cur[i]=(cur[i]+u)&255
            elif ft==3: cur[i]=(cur[i]+((l+u)//2))&255
            elif ft==4: cur[i]=(cur[i]+paeth(l,u,ul))&255
            elif ft!=0: raise ValueError("unsupported PNG filter")
        rows.append(cur);prev=cur
    px=[]
    for row in rows:
        out=[]
        if ct==6:
            out=[tuple(row[i:i+4]) for i in range(0,len(row),4)]
        elif ct==2:
            out=[(*row[i:i+3],255) for i in range(0,len(row),3)]
        else:
            for idx in row:
                rgb=pal[idx]; a=trns[idx] if trns and idx<len(trns) else 255; out.append((*rgb,a))
        px.append(out)
    return sw,sh,px
sw,sh,pix=read_png(SRC)
# cover-crop into the fixed asset while preserving the wallpaper composition.
scale=max(W/sw,H/sh); crop_w=W/scale; crop_h=H/scale; ox=(sw-crop_w)/2; oy=(sh-crop_h)/2
vals=[]
for y in range(H):
    sy=min(sh-1,max(0,int(oy+(y+0.5)/scale)))
    for x in range(W):
        sx=min(sw-1,max(0,int(ox+(x+0.5)/scale)))
        r,g,b,a=pix[sy][sx]
        vals.append(((r>>3)<<11)|((g>>2)<<5)|(b>>3))
OUT.parent.mkdir(parents=True,exist_ok=True)
with OUT.open("w") as f:
    f.write("#ifndef LIONOS_GENERATED_WALLPAPER_H\n#define LIONOS_GENERATED_WALLPAPER_H\n#include <stdint.h>\n")
    f.write(f"#define LION_WALLPAPER_W {W}u\n#define LION_WALLPAPER_H {H}u\n")
    f.write("static const uint16_t lion_wallpaper_rgb565[] = {\n")
    for i in range(0,len(vals),16): f.write("    "+", ".join(f"0x{v:04X}u" for v in vals[i:i+16])+",\n")
    f.write("};\n#endif\n")

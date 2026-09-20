#!/usr/bin/env python3
"""Convert selected PNG icons into a tiny C header for the freestanding GUI.

Supports the PNG formats used by LionOS icons: 8-bit RGB, RGBA and palette
images, non-interlaced, with PNG scanline filters 0..4.
"""
import struct
import sys
import zlib
from pathlib import Path

SIZE = 48
ICONS = {
    "lionos": "Icons/lionos-icon.png",
    "terminal": "Icons/Terminal-icon.png",
    "browser": "Icons/Browser-icon.png",
    "desktop": "Icons/Desktop-icon.png",
    "documents": "Icons/DocumentsFolder-icon.png",
    "tools": "Icons/Tools-icon.png",
}

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter(raw, width, height, bpp, stride):
    rows = []
    pos = 0
    prev = bytearray(stride)
    for _ in range(height):
        ft = raw[pos]
        pos += 1
        cur = bytearray(raw[pos:pos + stride])
        pos += stride
        for i in range(stride):
            left = cur[i - bpp] if i >= bpp else 0
            up = prev[i]
            ul = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                cur[i] = (cur[i] + left) & 255
            elif ft == 2:
                cur[i] = (cur[i] + up) & 255
            elif ft == 3:
                cur[i] = (cur[i] + ((left + up) >> 1)) & 255
            elif ft == 4:
                cur[i] = (cur[i] + paeth(left, up, ul)) & 255
            elif ft != 0:
                raise ValueError(f"unsupported PNG filter {ft}")
        rows.append(bytes(cur))
        prev = cur
    return rows


def read_png(path):
    data = Path(path).read_bytes()
    if not data.startswith(PNG_SIG):
        raise ValueError(f"{path}: not a PNG")
    pos = len(PNG_SIG)
    ihdr = None
    idat = bytearray()
    palette = None
    trns = None
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif typ == b"PLTE":
            palette = [tuple(chunk[i:i + 3]) for i in range(0, len(chunk), 3)]
        elif typ == b"tRNS":
            trns = bytes(chunk)
        elif typ == b"IDAT":
            idat.extend(chunk)
        elif typ == b"IEND":
            break
    if not ihdr:
        raise ValueError(f"{path}: missing IHDR")
    width, height, depth, ctype, comp, filt, interlace = ihdr
    if depth != 8 or comp != 0 or filt != 0 or interlace != 0:
        raise ValueError(f"{path}: only non-interlaced 8-bit PNGs are supported")
    channels = {2: 3, 3: 1, 6: 4}.get(ctype)
    if channels is None:
        raise ValueError(f"{path}: unsupported PNG color type {ctype}")
    raw = zlib.decompress(bytes(idat))
    rows = unfilter(raw, width, height, channels, width * channels)
    pixels = []
    for row in rows:
        out = []
        if ctype == 6:
            for i in range(0, len(row), 4):
                out.append((row[i], row[i + 1], row[i + 2], row[i + 3]))
        elif ctype == 2:
            transparent = None
            if trns and len(trns) >= 6:
                transparent = (trns[1], trns[3], trns[5])
            for i in range(0, len(row), 3):
                rgb = tuple(row[i:i + 3])
                out.append((*rgb, 0 if transparent == rgb else 255))
        else:
            for index in row:
                rgb = palette[index]
                alpha = trns[index] if trns and index < len(trns) else 255
                out.append((*rgb, alpha))
        pixels.append(out)
    return width, height, pixels


def resize_rgba(width, height, pixels):
    out = []
    for y in range(SIZE):
        sy = min(height - 1, (y * height) // SIZE)
        for x in range(SIZE):
            sx = min(width - 1, (x * width) // SIZE)
            r, g, b, a = pixels[sy][sx]
            out.append((r << 16) | (g << 8) | b | (a << 24))
    return out


def emit_array(name, values):
    lines = [f"static const uint32_t lion_icon_{name}[{SIZE * SIZE}] = {{"]
    for i in range(0, len(values), 8):
        lines.append("    " + ", ".join(f"0x{v:08X}u" for v in values[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/lion_icons.h")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    chunks = [
        "#ifndef LIONOS_GENERATED_ICONS_H",
        "#define LIONOS_GENERATED_ICONS_H",
        "#include <stdint.h>",
        f"#define LION_ICON_SIZE {SIZE}u",
        "",
    ]
    for name, path in ICONS.items():
        values = resize_rgba(*read_png(path))
        chunks.append(emit_array(name, values))
        chunks.append("")
    chunks.append("#endif")
    out_path.write_text("\n".join(chunks) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

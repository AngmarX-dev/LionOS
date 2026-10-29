#!/usr/bin/env python3
"""Convert a PNG into a compact RGB565 framebuffer image, with cover-cropping."""
import struct
import sys
import zlib
from pathlib import Path

DEFAULT_SRC = Path("assets/wallpapers/lionos-wallpaper-default.png")
DEFAULT_OUT = Path("build/lion_wallpaper.h")
W, H = 960, 540
PNG_SIG = b"\x89PNG\r\n\x1a\n"


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pc and pa <= pb:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path):
    data = path.read_bytes()
    if not data.startswith(PNG_SIG):
        raise ValueError(f"{path}: not a PNG")
    pos = len(PNG_SIG)
    ihdr = None
    idat = bytearray()
    palette = None
    while pos < len(data):
        if pos + 8 > len(data):
            raise ValueError(f"{path}: truncated PNG chunk")
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + n]
        if len(chunk) != n:
            raise ValueError(f"{path}: truncated PNG data")
        pos += 12 + n
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif typ == b"PLTE":
            palette = [tuple(chunk[i:i + 3]) for i in range(0, len(chunk), 3)]
        elif typ == b"IDAT":
            idat.extend(chunk)
        elif typ == b"IEND":
            break
    if not ihdr:
        raise ValueError(f"{path}: missing IHDR")
    sw, sh, depth, ctype, comp, filt, interlace = ihdr
    if depth != 8 or comp != 0 or filt != 0 or interlace != 0 or ctype not in (2, 3, 6):
        raise ValueError(f"{path}: expected non-interlaced 8-bit RGB, RGBA, or palette PNG")
    if ctype == 3 and not palette:
        raise ValueError(f"{path}: indexed PNG is missing PLTE")
    channels = {2: 3, 3: 1, 6: 4}[ctype]
    raw = zlib.decompress(bytes(idat))
    return sw, sh, ctype, channels, palette, raw


def decode_row(raw, pos, channels, stride, previous):
    if pos + stride + 1 > len(raw):
        raise ValueError("truncated PNG scanlines")
    filter_type = raw[pos]
    pos += 1
    current = bytearray(raw[pos:pos + stride])
    pos += stride
    for i in range(stride):
        left = current[i - channels] if i >= channels else 0
        up = previous[i]
        up_left = previous[i - channels] if i >= channels else 0
        if filter_type == 1:
            current[i] = (current[i] + left) & 255
        elif filter_type == 2:
            current[i] = (current[i] + up) & 255
        elif filter_type == 3:
            current[i] = (current[i] + ((left + up) >> 1)) & 255
        elif filter_type == 4:
            current[i] = (current[i] + paeth(left, up, up_left)) & 255
        elif filter_type != 0:
            raise ValueError(f"unsupported PNG filter {filter_type}")
    return pos, current


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def convert(src, out_path, prefix, array_name):
    sw, sh, ctype, channels, palette, raw = read_png(src)
    stride = sw * channels
    scale = max(W / sw, H / sh)
    crop_w, crop_h = W / scale, H / scale
    offset_x, offset_y = (sw - crop_w) / 2.0, (sh - crop_h) / 2.0
    source_x = [min(sw - 1, max(0, int(offset_x + (x + 0.5) / scale))) for x in range(W)]
    destination_rows = {}
    for y in range(H):
        sy = min(sh - 1, max(0, int(offset_y + (y + 0.5) / scale)))
        destination_rows.setdefault(sy, []).append(y)

    values = [0] * (W * H)
    pos = 0
    previous = bytearray(stride)
    if ctype == 2:
        def pixel_rgb(row, sx):
            i = sx * 3
            return row[i], row[i + 1], row[i + 2]
    elif ctype == 6:
        def pixel_rgb(row, sx):
            i = sx * 4
            return row[i], row[i + 1], row[i + 2]
    else:
        def pixel_rgb(row, sx):
            return palette[row[sx]]

    for sy in range(sh):
        pos, row = decode_row(raw, pos, channels, stride, previous)
        dest_ys = destination_rows.get(sy)
        if dest_ys:
            sampled = [rgb565(*pixel_rgb(row, sx)) for sx in source_x]
            for dy in dest_ys:
                base = dy * W
                values[base:base + W] = sampled
        previous = row

    guard = "LIONOS_GENERATED_" + prefix + "_H"
    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "#include <stdint.h>",
        f"#define {prefix}_W {W}u",
        f"#define {prefix}_H {H}u",
        f"static const uint16_t {array_name}[{W * H}] = {{",
    ]
    for i in range(0, len(values), 16):
        lines.append("    " + ", ".join(f"0x{v:04X}u" for v in values[i:i + 16]) + ",")
    lines.extend(["};", "#endif", ""])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT
    prefix = sys.argv[3] if len(sys.argv) > 3 else "LION_WALLPAPER"
    array_name = sys.argv[4] if len(sys.argv) > 4 else "lion_wallpaper_rgb565"
    convert(src, out_path, prefix, array_name)


if __name__ == "__main__":
    main()

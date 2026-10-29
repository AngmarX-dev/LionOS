#!/usr/bin/env python3
"""Convert PNG assets to a high-quality RGB565 image with centered cover-cropping."""
import struct
import sys
import zlib
from pathlib import Path

DEFAULT_SRC = Path("assets/wallpapers/lionos-wallpaper-default.png")
DEFAULT_OUT = Path("build/lion_wallpaper.h")
W, H = 1280, 720
PNG_SIG = b"\x89PNG\r\n\x1a\n"
BG_RGB = (10, 13, 22)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pc and pa <= pb:
        return a
    if pb <= pc and pb <= pc:
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
    transparency = None
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
        elif typ == b"tRNS":
            transparency = bytes(chunk)
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
    return sw, sh, ctype, channels, palette, transparency, raw


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


def source_axis(source_size, output_size, offset, scale):
    result = []
    for i in range(output_size):
        coord = max(0.0, min(source_size - 1.0, offset + (i + 0.5) / scale - 0.5))
        lo = int(coord)
        result.append((lo, min(source_size - 1, lo + 1), coord - lo))
    return result


def convert(src, out_path, prefix, array_name):
    sw, sh, ctype, channels, palette, transparency, raw = read_png(src)
    stride = sw * channels
    scale = max(W / sw, H / sh)
    offset_x = (sw - W / scale) / 2.0
    offset_y = (sh - H / scale) / 2.0

    # Retain decoded scanlines as bytes to keep memory use bounded for large PNGs.
    rows = []
    pos = 0
    previous = bytearray(stride)
    for _ in range(sh):
        pos, row = decode_row(raw, pos, channels, stride, previous)
        rows.append(bytes(row))
        previous = row
    del raw

    def pixel_rgba(row, sx):
        if ctype == 2:
            i = sx * 3
            r, g, b = row[i], row[i + 1], row[i + 2]
            a = 255
            if transparency and len(transparency) >= 6:
                if (r, g, b) == (transparency[1], transparency[3], transparency[5]):
                    a = 0
            return r, g, b, a
        if ctype == 6:
            i = sx * 4
            return row[i], row[i + 1], row[i + 2], row[i + 3]
        idx = row[sx]
        r, g, b = palette[idx]
        a = transparency[idx] if transparency and idx < len(transparency) else 255
        return r, g, b, a

    xs = source_axis(sw, W, offset_x, scale)
    ys = source_axis(sh, H, offset_y, scale)
    values = [0] * (W * H)
    for y, (y0, y1, fy) in enumerate(ys):
        row0, row1 = rows[y0], rows[y1]
        wy0, wy1 = 1.0 - fy, fy
        base = y * W
        for x, (x0, x1, fx) in enumerate(xs):
            wx0, wx1 = 1.0 - fx, fx
            samples = (
                (pixel_rgba(row0, x0), wx0 * wy0),
                (pixel_rgba(row0, x1), wx1 * wy0),
                (pixel_rgba(row1, x0), wx0 * wy1),
                (pixel_rgba(row1, x1), wx1 * wy1),
            )
            alpha = sum(px[3] * weight for px, weight in samples)
            if alpha > 0.0:
                r = sum(px[0] * px[3] * weight for px, weight in samples) / alpha
                g = sum(px[1] * px[3] * weight for px, weight in samples) / alpha
                b = sum(px[2] * px[3] * weight for px, weight in samples) / alpha
                inverse = 255.0 - alpha
                r = int((r * alpha + BG_RGB[0] * inverse) / 255.0 + 0.5)
                g = int((g * alpha + BG_RGB[1] * inverse) / 255.0 + 0.5)
                b = int((b * alpha + BG_RGB[2] * inverse) / 255.0 + 0.5)
            else:
                r, g, b = BG_RGB
            values[base + x] = rgb565(r, g, b)

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
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT
    prefix = sys.argv[3] if len(sys.argv) > 3 else "LION_WALLPAPER"
    array_name = sys.argv[4] if len(sys.argv) > 4 else "lion_wallpaper_rgb565"
    convert(src, out_path, prefix, array_name)


if __name__ == "__main__":
    main()

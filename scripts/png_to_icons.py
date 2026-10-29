#!/usr/bin/env python3
"""Convert selected PNG icons into a tiny C header for the freestanding GUI.

Supports the PNG formats used by LionOS icons: 8-bit RGB, RGBA and palette
images, non-interlaced, with PNG scanline filters 0..4.
"""
import struct
import sys
import zlib
from pathlib import Path

SIZE = 64
ICONS = {
    "lionos": "assets/branding/lionos-emblem.png",
    "logo": "assets/branding/lionos-logo.png",
    "wordmark": "assets/branding/lionos-wordmark.png",
    "terminal": "assets/icons/applications/terminal.png",
    "browser": "assets/icons/system/network.png",
    "desktop": "assets/branding/lionos-emblem.png",
    "documents": "assets/icons/applications/file-manager.png",
    "tools": "assets/icons/applications/settings.png",
    "computer": "assets/icons/desktop/computer.png",
    "home": "assets/icons/desktop/folder.png",
    "trash": "assets/icons/desktop/trash.png",
    "notepad": "assets/icons/applications/text-editor.png",
    "image_viewer": "assets/icons/applications/image-viewer.png",
    "file": "assets/icons/desktop/file.png",
    "wifi": "assets/icons/system/wifi.png",
    "volume": "assets/icons/system/volume.png",
    "power": "assets/icons/system/power.png",
    "help": "assets/icons/system/help.png",
    "security": "assets/icons/system/security.png",
    "update": "assets/icons/system/update.png",
    "battery": "assets/icons/system/battery.png",
    "file_audio": "assets/icons/filetypes/file-audio.png",
    "file_code": "assets/icons/filetypes/file-code.png",
    "file_image": "assets/icons/filetypes/file-image.png",
    "file_pdf": "assets/icons/filetypes/file-pdf.png",
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
    # Fit the art without stretching and filter it instead of nearest-neighbour sampling.
    scale = min(SIZE / width, SIZE / height)
    draw_w = max(1, min(SIZE, int(width * scale + 0.5)))
    draw_h = max(1, min(SIZE, int(height * scale + 0.5)))
    offset_x = (SIZE - draw_w) // 2
    offset_y = (SIZE - draw_h) // 2
    out = [0] * (SIZE * SIZE)

    def pack(samples):
        total_w = sum(weight for _, weight in samples)
        alpha_sum = sum(px[3] * weight for px, weight in samples)
        if total_w <= 0.0:
            return 0
        alpha = max(0, min(255, int(alpha_sum / total_w + 0.5)))
        if alpha_sum <= 0.0:
            return alpha << 24
        r = int(sum(px[0] * px[3] * weight for px, weight in samples) / alpha_sum + 0.5)
        g = int(sum(px[1] * px[3] * weight for px, weight in samples) / alpha_sum + 0.5)
        b = int(sum(px[2] * px[3] * weight for px, weight in samples) / alpha_sum + 0.5)
        return (alpha << 24) | (r << 16) | (g << 8) | b

    if scale >= 1.0:
        # Bilinear interpolation for small source images.
        for y in range(draw_h):
            sy = max(0.0, min(height - 1.0, (y + 0.5) * height / draw_h - 0.5))
            y0, y1 = int(sy), min(height - 1, int(sy) + 1)
            fy = sy - int(sy)
            for x in range(draw_w):
                sx = max(0.0, min(width - 1.0, (x + 0.5) * width / draw_w - 0.5))
                x0, x1 = int(sx), min(width - 1, int(sx) + 1)
                fx = sx - int(sx)
                samples = [
                    (pixels[y0][x0], (1.0 - fx) * (1.0 - fy)),
                    (pixels[y0][x1], fx * (1.0 - fy)),
                    (pixels[y1][x0], (1.0 - fx) * fy),
                    (pixels[y1][x1], fx * fy),
                ]
                out[(offset_y + y) * SIZE + offset_x + x] = pack(samples)
    else:
        # Area averaging removes high-frequency detail that would alias at icon size.
        for y in range(draw_h):
            top, bottom = y * height / draw_h, (y + 1) * height / draw_h
            sy0, sy1 = int(top), min(height, int(bottom + 0.999999))
            for x in range(draw_w):
                left, right = x * width / draw_w, (x + 1) * width / draw_w
                sx0, sx1 = int(left), min(width, int(right + 0.999999))
                samples = []
                for sy in range(sy0, sy1):
                    wy = max(0.0, min(bottom, sy + 1.0) - max(top, float(sy)))
                    for sx in range(sx0, sx1):
                        wx = max(0.0, min(right, sx + 1.0) - max(left, float(sx)))
                        weight = wx * wy
                        if weight:
                            samples.append((pixels[sy][sx], weight))
                out[(offset_y + y) * SIZE + offset_x + x] = pack(samples)
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

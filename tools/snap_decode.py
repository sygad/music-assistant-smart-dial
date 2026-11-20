"""Turn SNAP blocks from a knob log into PNG files.

Usage: python3 tools/snap_decode.py <log file> <output folder>
Each SNAP block holds one row of byte-swapped RGB565 per base64 line.
"""
import base64
import struct
import sys
import zlib
from pathlib import Path


def png(path, w, h, rgb_rows):
    raw = b"".join(b"\x00" + row for row in rgb_rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rgb565_swapped_to_rgb(row):
    out = bytearray()
    for i in range(0, len(row), 2):
        v = (row[i] << 8) | row[i + 1]  # big-endian on the wire to the panel
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return bytes(out)


def main():
    lines = Path(sys.argv[1]).read_text(errors="replace").splitlines()
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    i = 0
    while i < len(lines):
        if lines[i].startswith("SNAP "):
            _, name, w, h = lines[i].split()
            w, h = int(w), int(h)
            # other tasks' log lines can land inside a block: keep only full image rows
            row_len = (w * 2 + 2) // 3 * 4
            rows, j = [], i + 1
            while j < len(lines) and len(rows) < h and lines[j] != "SNAPEND":
                if len(lines[j]) == row_len:
                    rows.append(rgb565_swapped_to_rgb(base64.b64decode(lines[j])))
                j += 1
            if len(rows) == h:
                png(out / f"{name}.png", w, h, rows)
                print(out / f"{name}.png")
            else:
                print(f"{name}: incomplete ({len(rows)} of {h} rows), skipped")
            i = j + 1
        else:
            i += 1


if __name__ == "__main__":
    main()

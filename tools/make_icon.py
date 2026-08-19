"""Generate the Splittr app icon: the ember dot-matrix flame from the
matrix-bar loading animation, purple on the bench-dark background, inside a
Big Sur style rounded square. Pure stdlib (zlib+struct) PNG writer."""
import zlib, struct, math, sys

SIZE = 1024
R = 232                      # corner radius (Big Sur-ish)
BG = (0x14, 0x11, 0x18)
MARGIN = 128

# matrix-bar geometry: flat cells
CELL_W, CELL_H, GAP = 34, 15, 11

def hash01(a, b, salt=0):
    h = (a * 73856093) ^ (b * 19349663) ^ (salt * 83492791)
    h ^= h >> 13
    h = (h * 0x5bd1e995) & 0xFFFFFFFF
    h ^= h >> 15
    return (h & 0xFF) / 255.0

STOPS = [(0.00, (58, 47, 85)),
         (0.35, (107, 79, 168)),
         (0.65, (171, 124, 242)),
         (0.85, (192, 154, 246)),
         (1.00, (237, 228, 255))]

def ramp(t):
    t = max(0.0, min(1.0, t))
    for (t0, c0), (t1, c1) in zip(STOPS, STOPS[1:]):
        if t <= t1:
            f = (t - t0) / (t1 - t0) if t1 > t0 else 0
            return tuple(int(a + (b - a) * f) for a, b in zip(c0, c1))
    return STOPS[-1][1]

def flame_intensity(u, v, c, r):
    """u in [-1,1] across, v in [0,1] from bottom -> 0..1"""
    if v >= 1.0:
        return 0.0
    body = (1.0 - v) ** 1.15 * math.exp(-(u * u) * (1.4 + 4.0 * v))
    ember = 0.5 + 0.5 * hash01(c, r)
    lick = 0.85 + 0.3 * math.sin(u * 9.0 + hash01(c, 7) * 6.28) * v
    t = body * ember * lick
    # sparse floating sparks above the body
    if t < 0.04 and v > 0.35 and hash01(c, r, 3) > 0.975:
        t = 0.25 + 0.45 * hash01(c, r, 5)
    return max(0.0, min(1.0, t))

# --- build cell field ---
inner = SIZE - 2 * MARGIN
cols = inner // (CELL_W + GAP)
rows = inner // (CELL_H + GAP)
x0 = (SIZE - cols * (CELL_W + GAP) + GAP) // 2
y0 = (SIZE - rows * (CELL_H + GAP) + GAP) // 2

cells = []
for r in range(rows):
    for c in range(cols):
        u = 2.0 * (c + 0.5) / cols - 1.0
        v = 1.0 - (r + 0.5) / rows      # v: 0 at bottom
        t = flame_intensity(u, v, c, r)
        base = 0.10  # faint base grid, like the unlit matrix field
        alpha = max(base if 0.25 < abs(u) < 1.0 or True else 0, min(1.0, t * 1.7))
        alpha = max(0.10, min(1.0, t * 2.3))
        cells.append((x0 + c * (CELL_W + GAP), y0 + r * (CELL_H + GAP), ramp(t), alpha))

def inside_round_rect(x, y):
    if x < 0 or y < 0 or x >= SIZE or y >= SIZE:
        return False
    cx = R if x < R else (SIZE - 1 - R if x > SIZE - 1 - R else None)
    cy = R if y < R else (SIZE - 1 - R if y > SIZE - 1 - R else None)
    if cx is None or cy is None:
        return True
    return (x - cx) ** 2 + (y - cy) ** 2 <= R * R

# --- rasterise ---
rowsdata = bytearray()
cell_rows = {}
for (cx, cy, col, a) in cells:
    for yy in range(cy, cy + CELL_H):
        cell_rows.setdefault(yy, []).append((cx, col, a))

for y in range(SIZE):
    rowsdata.append(0)  # filter none
    row = bytearray()
    spans = cell_rows.get(y, [])
    for x in range(SIZE):
        if not inside_round_rect(x, y):
            row += b"\x00\x00\x00\x00"
            continue
        r0, g0, b0 = BG
        for (cx, col, a) in spans:
            if cx <= x < cx + CELL_W:
                r0 = int(BG[0] + (col[0] - BG[0]) * a)
                g0 = int(BG[1] + (col[1] - BG[1]) * a)
                b0 = int(BG[2] + (col[2] - BG[2]) * a)
                break
        row += bytes((r0, g0, b0, 255))
    rowsdata += row

def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rowsdata), 9))
png += chunk(b"IEND", b"")

with open(sys.argv[1], "wb") as f:
    f.write(png)
print("wrote", sys.argv[1], cols, "x", rows, "cells")

"""ref_hb_fb.py - referencia (numpy) dos 60 quadros do hb_fb e conferencia do PPM do oraculo.

Uso: python ref_hb_fb.py [arquivo.ppm]

Recalcula o mesmo padrao do hb_fb.c (barras, degrade, quadrado; so depende do numero do quadro) em
uint32 RGBA8888 little-endian, a soma ponderada Σ px[i] * (i + 1) mod 2^64 de cada quadro e o FNV-1a 64
do ultimo quadro. Com um PPM, confere pixel a pixel contra o RGB do ultimo quadro.
"""
import sys

import numpy as np

W, H, BAR_H, SQ, FRAMES = 1280, 720, 120, 64, 60


def rgba(r, g, b):
    return (np.uint32(r) & 0xFF) | ((np.uint32(g) & 0xFF) << 8) | ((np.uint32(b) & 0xFF) << 16) | np.uint32(0xFF000000)


BARS = np.array([rgba(*c) for c in [(0xFF, 0xFF, 0xFF), (0xFF, 0xFF, 0), (0, 0xFF, 0xFF), (0, 0xFF, 0),
                                    (0xFF, 0, 0xFF), (0xFF, 0, 0), (0, 0, 0xFF), (0x10, 0x10, 0x10)]], dtype=np.uint32)
Y, X = np.mgrid[0:H, 0:W].astype(np.uint32)
IDX = (Y.astype(np.uint64) * W + X + 1).ravel()


def frame(f):
    sx, sy = (f * 20) % (W - SQ), 300 + (f * 7) % 200
    grad = rgba((X * 255 // (W - 1) + f * 4) & 0xFF,
                (np.where(Y >= BAR_H, Y - BAR_H, 0) * 255 // (H - BAR_H - 1) + f * 2) & 0xFF,
                ((X ^ Y) + f * 3) & 0xFF)
    px = np.where(Y < BAR_H, BARS[((X + f * 8) // 160) & 7], grad)
    sq = (X >= sx) & (X < sx + SQ) & (Y >= sy) & (Y < sy + SQ)
    return np.where(sq, rgba(0xFF, 0xFF, 0xFF), px).astype(np.uint32)


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    total = np.uint64(0)
    with np.errstate(over="ignore"):
        for f in range(1, FRAMES + 1):
            last = frame(f)
            total += np.sum(last.ravel().astype(np.uint64) * IDX, dtype=np.uint64)
    raw = last.astype("<u4").tobytes()
    print(f"Soma ponderada (todos os frames): 0x{int(total):016x}")
    print(f"FNV-1a do ultimo frame: 0x{fnv1a64(raw):016x}")
    if len(sys.argv) > 1:
        data = open(sys.argv[1], "rb").read()
        header = f"P6\n{W} {H}\n255\n".encode()
        rgb = np.frombuffer(raw, dtype=np.uint8).reshape(H, W, 4)[:, :, :3].tobytes()
        print(f"PPM {sys.argv[1]}: {'igual ao ultimo frame' if data == header + rgb else 'DIFERENTE'} ({len(data)} bytes)")


if __name__ == "__main__":
    main()

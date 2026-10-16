"""gen_romfs.py - gera o RomFS de teste do hb_assets e imprime o oraculo de integridade.

Uso: python gen_romfs.py [diretorio]   (padrao: romfs/ ao lado deste script)

  romfs/data/bloco.bin  256 KiB, byte i = ((i * 167 + 13) ^ (i >> 9)) & 0xFF
  romfs/texto/info.txt  uma linha de texto

O mesmo diretorio vira a secao RomFS do NRO (elf2nro --romfsdir=romfs) e o romfs:/ do oraculo pelo shim.
CRC-32 (IEEE, zlib) e FNV-1a 64 sao calculados aqui, independente do C.
"""
import os
import sys
import zlib

BLOCK_SIZE = 256 * 1024
INFO = b"o.b.d.c hb_assets: RomFS de teste\n"


def block():
    return bytes(((i * 167 + 13) ^ (i >> 9)) & 0xFF for i in range(BLOCK_SIZE))


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "romfs")
    data = block()
    os.makedirs(os.path.join(root, "data"), exist_ok=True)
    os.makedirs(os.path.join(root, "texto"), exist_ok=True)
    with open(os.path.join(root, "data", "bloco.bin"), "wb") as f:
        f.write(data)
    with open(os.path.join(root, "texto", "info.txt"), "wb") as f:
        f.write(INFO)
    print(f"[gen_romfs] {root}")
    print(f"Bloco: {len(data)} bytes")
    print(f"CRC-32: 0x{zlib.crc32(data):08x}")
    print(f"FNV-1a: 0x{fnv1a64(data):016x}")
    print(f"Bytes em 0x10000: {data[0x10000:0x10004].hex()}")


if __name__ == "__main__":
    main()

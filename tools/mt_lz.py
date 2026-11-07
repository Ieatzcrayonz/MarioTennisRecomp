"""Decompressor for the LZ format used by Mario Tennis (NM8E) boot loader (func_80300080).

Format: 4-byte header, then groups of [flag byte][8 items]. Flag bits are MSB-first:
0 = literal byte, 1 = back-reference of 2 bytes: b0=LLLL... actually (b0 hi nibble | b1) = distance,
b0 low nibble = n. n==0 -> length = next byte + 0x11 (3-byte ref); n==1 -> length 2; else length n+1.
Distance 0 terminates. A flag byte of 0 means 8 literals.
"""
import sys

def decompress(src: bytes, start: int = 0) -> tuple[bytes, int]:
    i = start + 4
    out = bytearray()
    while True:
        flags = src[i]; i += 1
        if flags == 0:
            out += src[i:i+8]; i += 8
            continue
        for bit in range(8):
            if flags & (0x80 >> bit):
                b0, b1 = src[i], src[i+1]
                dist = ((b0 & 0xF0) << 4) | b1
                if dist == 0:
                    return bytes(out), i + 2
                n = b0 & 0xF
                if n == 0:
                    length = src[i+2] + 0x11; i += 3
                else:
                    length = 2 if n == 1 else n + 1; i += 2
                p = len(out) - dist
                for k in range(length):
                    out.append(out[p + k])
            else:
                out.append(src[i]); i += 1

if __name__ == "__main__":
    rom, off, dst = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    data = open(rom, "rb").read()
    out, end = decompress(data, off)
    open(dst, "wb").write(out)
    print(f"in 0x{off:X}-0x{end:X} ({end-off:#x} bytes) -> {len(out):#x} bytes")

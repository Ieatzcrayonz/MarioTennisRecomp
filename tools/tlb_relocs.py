"""Relocations for code that runs through the TLB.

A TLB module (e.g. Mario Tennis entry 209, linked at 0x50000000) addresses its own code and data
with absolute lui/lo pairs. The recompiled code cannot follow the TLB, so each such pair is given
an R_MIPS_HI16 / R_MIPS_LO16 relocation: at runtime the section is "loaded" at the RAM buffer that
backs the TLB window, and RELOC_HI16/LO16 rebase those references onto the buffer.
"""

# Opcodes whose 16-bit immediate is the low half of an address (addiu, ori, loads, stores, FPU).
LO_OPS = {0x09, 0x0D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,
          0x2E, 0x31, 0x35, 0x37, 0x39, 0x3D, 0x3F}
# Opcodes that write rt (register clobbered), besides lui.
WRITES_RT = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
             0x26, 0x27, 0x37}


def tlb_relocs(W, vram, window, funcs):
    """Return sorted (vram, target_vram, type) relocations for references into [vram, vram+window).

    W: section words; funcs: [(addr, size)]. Pairing is tracked per function, in program order:
    a lui of the window's high half arms its register, each following low-half use through that
    register emits a LO16 (the first one also fixes the lui's HI16 target), and any other write to
    the register disarms it.
    """
    relocs = set()
    unpaired = 0
    for a, sz in funcs:
        hi = {}  # reg -> [lui word index, high value, first paired target]
        for i in range((a - vram) // 4, (a - vram + sz) // 4):
            w = W[i]
            op = w >> 26
            rs = (w >> 21) & 31
            rt = (w >> 16) & 31
            imm = w & 0xFFFF

            if op == 0x0F:  # lui
                val = imm << 16
                if hi.get(rt) is not None and hi[rt][2] is None:
                    unpaired += 1
                if vram - 0x8000 <= val < vram + window + 0x8000:
                    hi[rt] = [i, val, None]
                else:
                    hi.pop(rt, None)
                continue

            if op in LO_OPS and rs in hi:
                h = hi[rs]
                lo = imm if op == 0x0D else (imm - 0x10000 if imm & 0x8000 else imm)
                target = (h[1] + lo) & 0xFFFFFFFF
                if vram <= target < vram + window:
                    relocs.add((vram + 4 * i, target, "R_MIPS_LO16"))
                    if h[2] is None:
                        h[2] = target
                        relocs.add((vram + 4 * h[0], target, "R_MIPS_HI16"))

            # `addu rd, base, index` (indexed table access): rd inherits the armed high half, so the
            # following load through rd still pairs with the original lui.
            if op == 0 and (w & 0x3F) in (0x20, 0x21):
                rd = (w >> 11) & 31
                src = hi.get(rs) if rs in hi else hi.get(rt)
                if src is not None and not (rs in hi and rt in hi) and rd != 0:
                    hi[rd] = src
                    continue

            # Clobbers: R-type writes rd; I-type ALU/loads write rt. Stores do not.
            dst = (w >> 11) & 31 if op == 0 else (rt if op in WRITES_RT else None)
            if dst is not None and dst in hi and dst != 0:
                if hi[dst][2] is None:
                    unpaired += 1
                del hi[dst]
        unpaired += sum(1 for h in hi.values() if h[2] is None)
    return sorted(relocs), unpaired

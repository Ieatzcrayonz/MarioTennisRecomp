"""Build the N64Recomp inputs for Mario Tennis (USA) [NM8E].

Outputs (in the project root):
  build_assets/mt.us.recomp.z64   original ROM + every code segment decompressed and appended
                                 at a virtual ROM offset (N64Recomp reads code bytes from here)
  syms/mt.us.syms.toml           sections + function list for N64Recomp's symbols-file mode
  syms/overlay_map.json          real ROM segment -> virtual ROM offset, for the runtime hook
  mt.us.toml                     N64Recomp config

Usage: python tools/gen_symbols.py "<path to Mario Tennis (USA).z64>"
"""
import hashlib, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from mt_lz import decompress
from find_funcs import find_funcs, words
from libultra_names import LIBULTRA

EXPECTED_SHA1 = "999047f07cec931ffbdcc7b33b8502ef602807ee"
VROM_BASE = 0x01000000  # appended data starts here (ROM is 16 MiB)

# Segment-table index -> load address. Confidence notes in NOTES.md; "verify" ones get checked
# at runtime by the loader hook before being trusted.
OVERLAYS = [
    # (segment index, ROM offset, vram, verified)
    (0,   0x00156C0, 0x800C8000, True),
    (19,  0x0958B70, 0x802EC000, True),
    (31,  0x0963AE0, 0x800CA000, True),
    (35,  0x096AA30, 0x80500000, True),
    (71,  0x09CD770, 0x800E8000, True),
    (86,  0x0A1F530, 0x800E2000, True),
    (285, 0x0BAF500, 0x802D8000, True),
    (454, 0x0C67B90, 0x802D8000, True),   # shares slot with seg285
    (462, 0x0C6C320, 0x800E2000, True),
    (703, 0x0D19000, 0x80228000, True),
    (78,  0x0A01F60, 0x80100000, True),   # main calls 0x80100000; j targets agree
]

# Extra entry points inside other functions (hand-written asm that jumps into another routine's
# body). Emitted as overlapping functions so the jump becomes a tail call.
#   0x80033244: bzero body after `t1 = 0`; memset (0x80033204) jumps here with a fill pattern.
EXTRA_MAIN_FUNCS = [(0x80033244, 0x800332DC)]

# Code+data modules (likely court/character files) that load at 0x80200000 and start with a
# 3-entry jump-vector table (+0x0/+0x8/+0x10, matching overlay calls). Code extent is found by
# following the vectors, since the data after it can decode as valid instructions.
MODULE_VRAM = 0x80200000
MODULES = [12, 125, 275, 379, 475, 477, 764, 814, 832, 881, 882, 976, 981]

# Code modules that run through the TLB. Entry 209 (file 0xD1) is linked at 0x50000000; seg86 and
# seg462 map TLB entry 31 (2x16 KiB) there onto a RAM buffer, load it and call its vectors. It is
# recompiled as a relocatable section: every HI16/LO16 reference into its own window is relocated
# to the buffer, while its functions are also registered at 0x5000xxxx for the vector pointers.
TLB_MODULES = [(209, 0x50000000, 0x8000)]   # (file-table entry, vram, mapped window size)

# Code modules (< 8 KiB, entry stub `j 0x10000124`, or `j 0x10000130` for 108/473/1042 -- used by the
# title-screen demo) linked at 0x10000000 and executed
# through seg454's sliding 2x4 KiB TLB window, which maps whichever loaded copy is active.
TLB_WINDOW_VRAM = 0x10000000
TLB_WINDOW_MODULES = [17, 33, 99, 100, 101, 102, 103, 104, 105, 106, 108, 147, 163, 178, 200, 473, 774, 965, 989, 1042]

MAIN_TEXT = [(0x80031000, 0x8003DA00), (0x80040E60, 0x80049A78)]  # RSP code/ucode sits between
BOOT_TEXT = (0x80300000, 0x80300344)


def main(rom_path):
    rom = open(rom_path, "rb").read()
    sha1 = hashlib.sha1(rom).hexdigest()
    if sha1 != EXPECTED_SHA1:
        sys.exit(f"ROM SHA-1 {sha1} does not match Mario Tennis (USA) {EXPECTED_SHA1}")

    image = bytearray(rom)
    sections = []
    overlay_map = []

    def append(data):
        while len(image) % 0x10: image.append(0)
        off = len(image); image.extend(data); return off

    # The boot stub (ROM 0x1000 -> 0x80300000) is not recompiled: the runtime's init callback
    # performs its RAM clear + LZ decompression natively and then calls main at 0x80031000.

    # Main: LZ block at 0x1350 -> 0x80031000. N64Recomp requires the entrypoint section at ROM
    # 0x1000, so the decompressed main is written over the image there (the image is only a
    # code source for the recompiler; the runtime always reads the original ROM).
    main_bin, _ = decompress(rom, 0x1350)
    main_off = 0x1000
    image[main_off:main_off + len(main_bin)] = main_bin
    W = words(main_bin)
    # jal targets are only trusted from real code, never from data that happens to decode as jal.
    main_text_words = [w for lo, hi in MAIN_TEXT for w in W[(lo - 0x80031000) // 4:(hi - 0x80031000) // 4]]
    # Overlays call into main too; their (trimmed) text supplies extra main entry points.
    from find_funcs import text_end
    ovl_text_words = []
    for _, rom_off, vram, _v in OVERLAYS:
        d, _ = decompress(rom, rom_off); ow = words(d); ovl_text_words += ow[:text_end(ow, vram) // 4]
    mf = []
    for lo, hi in MAIN_TEXT:
        _, f = find_funcs(main_bin[lo - 0x80031000:hi - 0x80031000], lo, [main_text_words, ovl_text_words], te=hi - lo, keep=LIBULTRA)
        mf += f
    mf += [(a, e - a) for a, e in EXTRA_MAIN_FUNCS]
    sections.append(dict(name="main", rom=main_off, vram=0x80031000, size=len(main_bin), funcs=mf))
    overlay_map.append(dict(segment="main", real_rom=0x1350, vrom=main_off, vram=0x80031000,
                            size=len(main_bin), verified=True))

    for idx, rom_off, vram, verified in OVERLAYS:
        data, _ = decompress(rom, rom_off)
        off = append(data)
        _, f = find_funcs(data, vram, [main_text_words])
        sections.append(dict(name=f"seg{idx}", rom=off, vram=vram, size=len(data), funcs=f))
        overlay_map.append(dict(segment=idx, real_rom=rom_off, vrom=off, vram=vram,
                                size=len(data), verified=verified))

    from find_funcs import reachable_text_end
    import struct
    for idx in MODULES:
        rom_off = struct.unpack(">I", rom[0x116C0 + 8*idx:0x116C0 + 8*idx + 4])[0]
        data, _ = decompress(rom, rom_off); dw = words(data)
        vec = [((w & 0x3FFFFFF) << 2) | 0x80000000 for w in dw[:6] if w >> 26 == 2]
        te = reachable_text_end(data, MODULE_VRAM, vec)
        off = append(data)
        _, f = find_funcs(data, MODULE_VRAM, te=te)
        sections.append(dict(name=f"mod{idx}", rom=off, vram=MODULE_VRAM, size=len(data), funcs=f))
        overlay_map.append(dict(segment=idx, real_rom=rom_off, vrom=off, vram=MODULE_VRAM,
                                size=len(data), verified=True))

    from tlb_relocs import tlb_relocs
    for idx, vram, window in TLB_MODULES:
        rom_off = struct.unpack(">I", rom[0x116C0 + 8*idx:0x116C0 + 8*idx + 4])[0]
        data, _ = decompress(rom, rom_off); dw = words(data)
        vec = [((w & 0x3FFFFFF) << 2) | (vram & 0xF0000000) for w in dw[:4] if w >> 26 == 2]
        te = reachable_text_end(data, vram, vec)
        off = append(data)
        _, f = find_funcs(data, vram, te=te)
        relocs, unpaired = tlb_relocs(dw, vram, window, f)
        if unpaired:
            print(f"  warning: tlb{idx}: {unpaired} lui into the TLB window without a low half")
        sections.append(dict(name=f"tlb{idx}", rom=off, vram=vram, size=len(data), funcs=f, relocs=relocs))
        overlay_map.append(dict(segment=idx, real_rom=rom_off, vrom=off, vram=vram,
                                size=len(data), verified=True, tlb=True))

    # Small code modules linked at 0x10000000 and run through a 2x4 KiB TLB window (several are
    # loaded side by side; the window selects one). No relocations: their data references stay
    # virtual and the software TLB resolves them to whichever copy is mapped.
    for idx in TLB_WINDOW_MODULES:
        rom_off = struct.unpack(">I", rom[0x116C0 + 8*idx:0x116C0 + 8*idx + 4])[0]
        data, _ = decompress(rom, rom_off); dw = words(data)
        vram = TLB_WINDOW_VRAM
        vec = [((w & 0x3FFFFFF) << 2) | (vram & 0xF0000000) for w in dw[:4] if w >> 26 == 2] + [vram + 0x10]
        te = reachable_text_end(data, vram, vec)
        off = append(data)
        _, f = find_funcs(data, vram, te=te)
        sections.append(dict(name=f"win{idx}", rom=off, vram=vram, size=len(data), funcs=f))
        overlay_map.append(dict(segment=idx, real_rom=rom_off, vrom=off, vram=vram,
                                size=len(data), verified=True, tlb=True))

    os.makedirs(os.path.join(ROOT, "build_assets"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "syms"), exist_ok=True)
    open(os.path.join(ROOT, "build_assets", "mt.us.recomp.z64"), "wb").write(image)
    json.dump(overlay_map, open(os.path.join(ROOT, "syms", "overlay_map.json"), "w"), indent=1)
    # C++ table for the runtime's loader hook: real (compressed) ROM offset -> recomp section.
    with open(os.path.join(ROOT, "src", "game", "mt_overlay_table.inc"), "w") as f:
        f.write("// Generated by tools/gen_symbols.py -- do not edit by hand.\n")
        f.write("// { real ROM offset of LZ segment, virtual ROM offset of section, vram, size }\n")
        for e in overlay_map:
            if e["segment"] == "main": continue
            f.write(f'{{ 0x{e["real_rom"]:08X}, 0x{e["vrom"]:08X}, 0x{e["vram"]:08X}, 0x{e["size"]:08X} }}, // segment {e["segment"]}\n')

    # Functions that fall through into the next start (multiple entry points, e.g. memset ->
    # memset16) are extended to overlap the following function(s) so each one ends in a return.
    def ends_cleanly(img, base, sz):
        ws = [int.from_bytes(img[base + o: base + o + 4], "big") for o in range(0, sz, 4)]
        while ws and ws[-1] == 0: ws.pop()   # padding (and possibly a nop delay slot)
        def jump(w): return w == 0x03E00008 or (w & 0xFC1FFFFF) == 0x00000008 or w >> 26 == 2 or w >> 16 == 0x1000
        return any(jump(w) for w in ws[-2:])
    for s in sections:
        fl = sorted(s["funcs"])
        ext = []
        for k, (a, sz) in enumerate(fl):
            base = s["rom"] + a - s["vram"]; j = k
            while (not ends_cleanly(image, base, sz) and j + 1 < len(fl)
                   and fl[j + 1][0] <= a + sz + 0x10):          # only into adjacent code
                j += 1; sz = fl[j][0] + fl[j][1] - a
            ext.append((a, sz))
        s["funcs"] = ext

    # Function names: libultra names in main only; overlays with shared vram get a section tag.
    shared = {}
    for s in sections:
        shared.setdefault(s["vram"], []).append(s["name"])
    lines = ["# Generated by tools/gen_symbols.py -- do not edit by hand.\n"]
    total = 0; named = 0
    for s in sections:
        tag = s["name"] if s["name"] not in ("main", "boot") else ""
        lines.append(f'[[section]]\nname = "{s["name"]}"\nrom = 0x{s["rom"]:08X}\n'
                     f'vram = 0x{s["vram"]:08X}\nsize = 0x{s["size"]:X}\n\nfunctions = [\n')
        for a, sz in s["funcs"]:
            if s["name"] == "main" and a in LIBULTRA:
                name = LIBULTRA[a]; named += 1
            elif s["name"] == "boot" and a == 0x80300000:
                name = "boot_entrypoint"
            else:
                name = f"func_{tag + '_' if tag else ''}{a:08X}"
            lines.append(f'    {{ name = "{name}", vram = 0x{a:08X}, size = 0x{sz:X} }},\n')
            total += 1
        lines.append("]\n\n")
        if s.get("relocs"):
            lines.append("relocs = [\n")
            for v, t, ty in s["relocs"]:
                lines.append(f'    {{ vram = 0x{v:08X}, target_vram = 0x{t:08X}, type = "{ty}" }},\n')
            lines.append("]\n\n")
    open(os.path.join(ROOT, "syms", "mt.us.syms.toml"), "w").writelines(lines)

    missing = sorted(set(LIBULTRA) - {a for a, _ in sections[0]["funcs"]})
    if missing:
        sys.exit("libultra names point at non-function addresses: " + ", ".join(f"{a:08X}" for a in missing))

    print(f"recomp image: {len(image):#x} bytes; sections: {len(sections)}; "
          f"functions: {total} ({named} libultra names)")
    for s in sections:
        print(f"  {s['name']:7} vram {s['vram']:08X} vrom {s['rom']:08X} size {s['size']:#8x} funcs {len(s['funcs'])}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "..", "Mario Tennis (USA).z64"))

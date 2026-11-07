"""Pre-check the symbols file against N64Recomp's rule that branches stay inside their function
and `j` targets are either inside the function or a function start. Prints every violation."""
import re, struct, sys
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
from find_funcs import _branch_target
img = open("build_assets/mt.us.recomp.z64", "rb").read()
secs = []; cur = None
for line in open("syms/mt.us.syms.toml"):
    m = re.match(r'name = "(\w+)"', line)
    if m and not line.startswith(" "): cur = dict(name=m.group(1), funcs=[]); secs.append(cur)
    for k in ("rom", "vram"):
        m = re.match(rf"{k} = 0x(\w+)", line)
        if m and cur is not None: cur[k] = int(m.group(1), 16)
    m = re.search(r'name = "(\w+)", vram = 0x(\w+), size = 0x(\w+)', line)
    if m: cur["funcs"].append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
bad = 0
for s in secs:
    starts = {v for _, v, _ in s["funcs"]}
    for name, v, sz in s["funcs"]:
        base = s["rom"] + v - s["vram"]
        for k in range(sz // 4):
            w = struct.unpack(">I", img[base + 4*k: base + 4*k + 4])[0]
            t = _branch_target(w, k)
            if t is not None and not (0 <= t < sz // 4):
                tv = v + 4*t; bad += 1
                print(f"{s['name']:7} {name} @{v+4*k:08X}: branch -> {tv:08X}{' (func start)' if tv in starts else ''}")
            if w >> 26 == 2:
                tv = ((w & 0x3FFFFFF) << 2) | ((v + 4*k) & 0xF0000000)
                if not (v <= tv < v + sz) and tv not in starts:
                    bad += 1; print(f"{s['name']:7} {name} @{v+4*k:08X}: j -> {tv:08X} (not a func start)")
# every jal must land on a function start somewhere (main, own section, or any overlay)
all_starts = {v for s in secs for _, v, _ in s["funcs"]}
for s in secs:
    for name, v, sz in s["funcs"]:
        base = s["rom"] + v - s["vram"]
        for k in range(sz // 4):
            w = struct.unpack(">I", img[base + 4*k: base + 4*k + 4])[0]
            if w >> 26 == 3:
                tv = ((w & 0x3FFFFFF) << 2) | ((v + 4*k) & 0xF0000000)
                if tv not in all_starts:
                    bad += 1; print(f"{s['name']:7} {name} @{v+4*k:08X}: jal -> {tv:08X} (no function)")
print(bad, "violations")

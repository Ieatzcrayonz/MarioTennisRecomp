"""Write mt.trace.toml: mt.us.toml plus an entry hook on every main/seg function that logs the first
call of each function (mt_trace in src/game/mt_libultra.cpp). Used to find where boot stalls.
Usage: python tools/make_trace_config.py [section-name-prefix ...]   (default: main)"""
import re, sys
prefixes = sys.argv[1:] or ["main"]
base = open("mt.us.toml").read()
syms = open("syms/mt.us.syms.toml").read()
ignored = set(re.findall(r'"(func_\w+)"', base.split("ignored = [")[1].split("]")[0]))
hooked = set(re.findall(r'func = "(\w+)"\s*\nbefore_vram = 0x(\w+)', base))
out = [base, "\n# ---- generated trace hooks ----\n"]
n = 0
for sec in re.split(r"\[\[section\]\]", syms)[1:]:
    name = re.search(r'name = "(\w+)"', sec).group(1)
    if not any(name.startswith(p) for p in prefixes): continue
    for fn, v in re.findall(r'name = "(\w+)", vram = 0x(\w+), size', sec):
        if fn in ignored or not fn.startswith("func_") or (fn, v) in hooked: continue
        hook_name = "recomp_entrypoint" if v == "80031000" else fn  # N64Recomp renames the entry
        out.append(f'[[patches.hook]]\nfunc = "{hook_name}"\nbefore_vram = 0x{v}\n'
                   f'text = "    {{ void mt_trace(uint32_t); mt_trace(0x{v}u); }}"\n\n')
        n += 1
open("mt.trace.toml", "w").write("".join(out))
print(f"mt.trace.toml: {n} trace hooks")

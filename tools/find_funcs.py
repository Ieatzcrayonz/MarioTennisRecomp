"""Function boundary finder for Mario Tennis segments (no symbols available).

Starts: section start; in-section jal targets (from any section); first non-nop after each
`jr $ra` + delay slot; and after an unconditional j/b + delay slot when the next instruction
opens a stack frame (addiu sp,sp,-N) and no branch in the preceding function targets it.
Text ends at the last `jr $ra` before a run of invalid instructions."""
import struct, rabbitizer

def words(b): return [struct.unpack(">I", b[i:i+4])[0] for i in range(0, len(b) - 3, 4)]

def text_end(W, va, run=12):
    last_end = 0; bad = 0
    for i, w in enumerate(W):
        ok = rabbitizer.Instruction(w, va + 4*i).isValid()
        if w == 0x03E00008: last_end = i + 2
        bad = 0 if ok else bad + 1
        if bad >= run and last_end: return last_end * 4
    return last_end * 4

def _is_prologue(w): return (w >> 16) == 0x27BD and (w & 0x8000)

def _branch_target(w, i):
    op = w >> 26
    if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 16, 17, 18, 19)):
        off = w & 0xFFFF
        if off & 0x8000: off -= 0x10000
        return i + 1 + off
    if op == 0x11 and ((w >> 21) & 0x1F) == 8:  # bc1x
        off = w & 0xFFFF
        if off & 0x8000: off -= 0x10000
        return i + 1 + off
    return None

def _local_target(w, i, va):
    """Branch or `j` target as a word index relative to the section start (None otherwise)."""
    t = _branch_target(w, i)
    if t is not None: return t
    if w >> 26 == 2:
        return ((((w & 0x3FFFFFF) << 2) | (va & 0xF0000000)) - va) // 4
    return None

def find_funcs(b, va, extra_jal_sources=(), te=None, keep=()):
    W = words(b)
    if te is None: te = text_end(W, va)
    n = te // 4
    starts = {0}
    for i in range(n):
        if W[i] == 0x03E00008:
            j = i + 2
            while j < n and W[j] == 0: j += 1
            if j < n: starts.add(j)
    for src in [W[:n], *extra_jal_sources]:
        for w in src:
            if w >> 26 == 3:
                t = ((w & 0x3FFFFFF) << 2) | 0x80000000
                if va <= t < va + te: starts.add((t - va) // 4)
    # tail-call splits: after `j X` (X outside the current function) + delay slot, when no branch
    # in the current function reaches past it and the function has no jump table (`jr` non-ra).
    cur_targets = set(); fstart = 0; has_jtbl = False
    for i in range(n):
        if i in starts: cur_targets = set(); fstart = i; has_jtbl = False
        w = W[i]
        t = _branch_target(w, i)
        if t is not None: cur_targets.add(t)
        if (w & 0xFC1FFFFF) == 0x00000008 and ((w >> 21) & 31) != 31: has_jtbl = True
        tail = False
        if w >> 26 == 2:
            tgt = (((w & 0x3FFFFFF) << 2) | ((va + 4*i) & 0xF0000000)) - va
            tail = not (fstart*4 <= tgt <= 4*(i+1))
        elif w >> 16 == 0x1000:
            tgt = _branch_target(w, i); tail = tgt is not None and tgt < fstart
        if tail and not has_jtbl and i + 2 < n and not any(x >= i + 2 for x in cur_targets):
            j = i + 2
            while j < n and W[j] == 0: j += 1
            if j < n: starts.add(j)
    # Merge pass: a start that code before it branches into (or whose code branches back before
    # it) is a label inside the previous function, e.g. a tail placed after `jr $ra`. Starts that
    # are jal targets or explicitly kept (known library entry points) are never merged.
    jal_targets = set()
    # Only the section's own calls protect a start: calls from other sections into a shared
    # overlay slot may refer to a different overlay loaded there.
    for src in [W[:n]] if extra_jal_sources and va != 0x80031000 else [W[:n], *extra_jal_sources]:
        for w in src:
            if w >> 26 == 3:
                t = ((w & 0x3FFFFFF) << 2) | 0x80000000
                if va <= t < va + te: jal_targets.add((t - va) // 4)
    protected = jal_targets | {(a - va) // 4 for a in keep if va <= a < va + te} | {0}
    changed = True
    while changed:
        changed = False
        order = sorted(starts)
        for k in range(1, len(order)):
            s = order[k]
            if s in protected: continue
            prev, nxt = order[k - 1], (order[k + 1] if k + 1 < len(order) else n)
            into = any((t := _local_target(W[i], i, va)) is not None and s <= t < nxt for i in range(prev, s))
            back = any((t := _local_target(W[i], i, va)) is not None and prev <= t < s for i in range(s, nxt))
            if into or back:
                starts.discard(s); changed = True; break
    starts = sorted(starts)
    funcs = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else n
        while e - 2 > s and W[e - 1] == 0 and W[e - 2] == 0: e -= 1  # drop alignment padding, keep delay slot
        funcs.append((va + 4*s, 4*(e - s)))
    return te, funcs

def reachable_text_end(b, va, entries):
    """Code extent of a code+data module: follow functions from entry vectors (j targets at the
    start) and their in-module jal/j targets; text ends at the furthest function end."""
    W = words(b); n = len(W)
    todo = [(e - va) // 4 for e in entries if va <= e < va + 4*n]; seen = set(); end = 0
    while todo:
        s = todo.pop()
        if s in seen or not (0 <= s < n): continue
        seen.add(s); i = s; far = s
        while i < n:
            w = W[i]
            t = _branch_target(w, i)
            if t is not None and t > far: far = t
            if w >> 26 in (2, 3):
                tv = ((w & 0x3FFFFFF) << 2) | (va & 0xF0000000)
                if va <= tv < va + 4*n:
                    if w >> 26 == 2 and s*4 <= tv - va and (tv - va)//4 > far and (tv-va)//4 < i + 512: far = (tv - va)//4
                    else: todo.append((tv - va) // 4)
            if (w == 0x03E00008 or (w >> 26 == 2 and not (s <= ((((w & 0x3FFFFFF) << 2) | (va & 0xF0000000)) - va)//4 <= far))) and i >= far:
                i += 2; break
            i += 1
        end = max(end, i)
    return end * 4

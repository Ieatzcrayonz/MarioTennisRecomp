# Mario Tennis (USA) — N64Recomp port notes

ROM: `Mario Tennis (USA).z64`, game code NM8E, SHA-1 `999047f07cec931ffbdcc7b33b8502ef602807ee`,
big-endian, CIC-6102, libultra 2.0I. No existing decomp/recomp to build on (the only public
"decomp" has 14 C files and no symbols), so everything here is derived from the ROM.

## Boot and compression
- Entry 0x80300000 (ROM 0x1000, 0x350 bytes) zeroes RAM, then `func_80300080` LZ-decompresses
  ROM 0x1350 → 0x80031000 (0x2AE10 bytes) and jumps to 0x80031000 with a0 = 0x116C0.
- LZ format (Camelot): 4-byte header (`01` + 24-bit size), flag byte MSB-first, 0 = literal,
  1 = 2-byte ref (12-bit distance, 4-bit n; n=0 → len = next byte + 0x11, n=1 → 2, else n+1),
  distance 0 ends. Implemented in `tools/mt_lz.py`.
- ROM 0x116C0 is a table of 1275 (start, end) ROM offset pairs: the game's file system.
  `func_80035CC0` is the streaming DMA + decompress loader (`func_80035AB0` = decompressor).

## Code layout
| Section | ROM (real) | VRAM | Size | Notes |
|---|---|---|---|---|
| boot | 0x1000 | 0x80300000 | 0x350 | |
| main | LZ @0x1350 | 0x80031000 | 0x2AE10 | CPU text 0x80031000–0x8003DBF0 and 0x80040E60–0x80049A78; RSP ucode in between |
| seg0 | LZ @0x156C0 | 0x800C8000 | 0x1880 | |
| seg19 | LZ @0x958B70 | 0x802EC000 | 0x94F8 | |
| seg31 | LZ @0x963AE0 | 0x800CA000 | 0xA020 | |
| seg35 | LZ @0x96AA30 | 0x80500000 | 0x140C | expansion-pak range? check |
| seg71 | LZ @0x9CD770 | 0x800E8000 | 0x41DB4 | |
| seg86 | LZ @0xA1F530 | 0x800E2000 | 0x59330 | shares slot with seg462 |
| seg285 | LZ @0xBAF500 | 0x802D8000 | 0x18C8 | |
| seg454 | LZ @0xC67B90 | 0x802D8000 | 0x2C60 | shares slot with seg285; maps TLB 0x10000000 |
| seg462 | LZ @0xC6C320 | 0x800E2000 | 0x418A0 | |
| seg703 | LZ @0xD19000 | 0x80228000 | 0x5CF0 | |
| seg78 | LZ @0xA01F60 | 0x80100000 | 0x1E0 | main calls 0x80100000 |
| mod12…mod981 (13 files) | LZ | 0x80200000 | 0x9C00–0x13AD4 | code+data modules with a 3-entry jump vector (+0/+8/+0x10); code extent found by following the vectors |
| entry 209 (file 0xD1) | LZ @0xB82F30 | **0x50000000 (TLB)** | 0x6370 | **not recompiled yet** — see TLB below |

Overlay VRAMs were inferred from internal `jal` targets landing on function prologues and
cross-checked with absolute `j` targets (seg454 was wrong at first; `j` evidence fixed it). The recomp image appends each
decompressed segment at a virtual ROM offset ≥ 0x01000000 (`syms/overlay_map.json`).

## Graphics / audio microcode
- `RSP Gfx ucode F3DEX fifo 2.08` and `xbus 2.08` (F3DEX2 family) — supported by RT64, which
  provides resolution scaling, MSAA, widescreen and high frame rate.
- rspboot 0x8003DA00–0x8003DAD0. Audio ucode text 0x8003DAD0 (0xC60, IMEM 0x04001080), data
  0x8005A8C0, set up by the seg19 audio driver; recompiled with RSPRecomp → `rsp/aspMain.cpp`.
- Gfx ucode table at 0x80049AC4: F3DEX2 fifo text 0x8003E730 / data 0x8005AB70, xbus text
  0x8003FAC0 / data 0x8005AF90.

## libultra
100 functions identified by behaviour (`tools/libultra_names.py`). Still unresolved:
- controller polling (`osContInit`/`osContStartReadData`/`osContGetReadData`) — candidates
  0x800489D0 / 0x80048CB0; their callers are probably in the overlays.
- Controller Pak / Transfer Pak cluster (0x80047114–0x80049A58). The runtime has no Transfer
  Pak support (used for Mario Tennis GB link-up), so these need stubs that report "no pak".
- 64-bit helpers (`__ll_*`, `lldiv`/`ldiv` at 0x80032FA0…) are deliberately left unnamed and
  recompiled as plain code.

## Status (2026-11-07)
**Boots, renders and runs the intro.** Blimp flyover, stadium and the characters walking onto the
court render cleanly at window resolution, with RT64 frame interpolation on. The earlier "vertex
explosions" (and the apparent freeze) came from `osVirtualToPhysical`: the runtime's version
ignores the TLB, so the game's TLB-mapped model data (0x02xxxxxx) was handed to the RSP at wrong
physical addresses. The game's copy (0x80033160) is now `mt_osVirtualToPhysical`, implemented in
`src/game/mt_tlb.cpp` on top of the software TLB.

Launcher: original Mario Tennis splash (`assets/mt-splash.svg`, layout in `assets/launcher.rml`)
and tennis-ball app icon (`icons/`). Mods button removed. `tools/capture_run.ps1` takes window
screenshots (`-NoAutostart` for the launcher).

### How to build / run
- `sh tools/recompile.sh` — regenerates `include/recomp.h`, symbols, spin hooks, RecompiledFuncs,
  `rsp/aspMain.cpp` from the ROM (`MT_CONFIG=mt.trace.toml` for a trace build; see below).
- `toolsuild_game.bat` — clang-cl 19.1.7 + Ninja build → `build\MarioTennisRecompiled.exe`
  (run with the project folder as working directory so `assets\` is found).
- Env switches: `MT_AUTOSTART=1` (skip the launcher), `MT_LOG=<file>` (stdout/stderr to file),
  `MT_LOG_LOADS=1` (log data-file loads too).
- Config/saves live in `%LOCALAPPDATA%\MarioTennisRecompiled` (the stored ROM copy
  `mt.n64.us.z64` is what the launcher's ROM picker would create).

### Fixes that got it booting
1. Loader hook reads the ROM offset at loader *entry* (the loader advances its own copy).
2. KSEG1 RDRAM pointers patched to KSEG0 (3 instructions), and KSEG1 folded in the software TLB.
3. Busy-wait loops yield to the cooperative scheduler (`tools/find_spins.py`, auto-generated
   hooks): the idle thread and seg78's wait on the gfx-task counter 0x80062608.
4. Audio ucode jump table: 16 entries; `0x02B0` means IMEM 0x12B0 (12-bit RSP PC).
5. **Software TLB** (`tools/make_recomp_h.py` → `include/recomp.h`, `src/game/mt_tlb.cpp`): the game
   maps segment data at 0x02000000+ (and the code module) through the TLB and dereferences it on
   the CPU; every recompiled memory access now translates non-KSEG0 addresses via a page table
   filled by `osMapTLB`. Unmapped accesses log `[mt] access to unmapped virtual address`.
6. Hand-written replacements in `src/game/mt_libultra.cpp` (second osStartThread copy, pak stubs).

### Trace tooling
`python tools/make_trace_config.py main` writes `mt.trace.toml` (first-call log of every main
function); build with `MT_CONFIG=mt.trace.toml sh tools/recompile.sh`. Ultramodern's scheduler
log can be enabled by switching `debug_printf` in `ultramodern/include/ultramodern/ultramodern.hpp`
(revert afterwards with `git checkout` in `lib/N64ModernRuntime`).

## Shadows (fixed 2026-11-07)
Character shadows are each model drawn a second time through a flattening matrix with othermode L
`A0504A75`: Z compare + update, `ZMODE_XLU`, **`G_ZS_PRIM`** (constant prim depth), env alpha 0x58.
On hardware the strict depth test lets only the first overlapping shadow triangle darken a pixel.
RT64 emulates prim depth as `z = primDepth * w` in the vertex shader, which lands an ulp off after
the perspective divide on some vertices, so overlaps depth-fought at high resolution (patchy,
flickering shadows). Fix: for `G_ZS_PRIM` triangle draws RT64 now uses a viewport depth range of
`[primDepth, primDepth]`, giving exactly the prim depth per pixel/sample with no shader changes.
The change is in `lib/rt64/src/render/rt64_framebuffer_renderer.cpp` (shared library folder) and
saved in `patches/rt64-mario-tennis.patch` (with the stretch option) -- re-apply if RT64 is updated.

Display-list inspection: `MT_DUMP_DL=<first> MT_DUMP_DL_STEP=<n> MT_DUMP_DL_COUNT=<k>` prints
render states and triangle counts per graphics task (`src/game/mt_dldump.cpp`; no-op otherwise).

## Narrow 304x224 layout (boot logos, front-end) -- centred 2026-11-07
The game's video mode is 384 wide (custom VI mode: width 384, xScale 0x299, hStart 0x080-0x2D5;
it swaps `framebuffer + 0x80` with origin 0x280, i.e. exactly one row in). The 3D game uses the
full 384x224 viewport, but the boot logos and the seg71 front-end (menus, player/character/court
select) draw into a 304x224 layout in the left 304 columns. RT64 shows the whole 384-wide buffer,
so those screens sat ~10% left of centre, and parts drawn with other viewports/scissors showed
up as an offset "second copy" in the left margin.
Fix, per frame in `src/game/mt_dlfix.cpp` (runs before every graphics task): if the frame uses
a 304/344-wide scissor, every such scissor becomes 40..344, every viewport centred at x=152
(trans.x 608) is re-centred at 192, texture/fill rectangles under the narrow scissor move +40 px,
and a small display list at 0x807FFC00 that clears both 40 px margins is spliced in after the
frame's first clear. (The earlier seg71 viewport patch and scissor instruction patches are kept;
the pass is idempotent with them.) Verified: logos, title, main menu, play mode, player and
character select.

## Display options
- Resolution (Settings > Graphics): **Auto** renders at the window / full-screen resolution (e.g.
  1080p, 1440p, 4K), **Original** is native, **Original 2x** doubles it; **Downsampling** (2x/4x)
  supersamples on top of those two, plus MSAA 2x/4x/8x. Fixed **720p / 1080p / 1440p / 4K**
  render at that many lines whatever the window size (RT64 Manual, multiplier lines/240: 3, 4.5,
  6, 9; width follows the aspect ratio, so 1440p at 16:9 is 2560x1440). They are new
  `Resolution::Res720p..Res2160p` values in N64ModernRuntime (JSON "720p".."2160p"), mapped in
  `set_application_user_config` (`src/main/rt64_render_context.cpp`); `ui_config.cpp` binds
  `res_fixed` (Auto or fixed) to grey out Downsampling. Verified 1440p in the intro.
- Aspect Ratio: **Original** (4:3), **Expand** (widescreen 3D, wider field of view) and **Stretch**
  (4:3 picture scaled to fill the window). Stretch is new: `AspectRatio::Stretch` in
  N64ModernRuntime (`patches/n64modernruntime-mario-tennis.patch`), `RT64::VIRenderer::stretchToWindow`
  in RT64 (`patches/rt64-mario-tennis.patch`), mapped in `src/main/rt64_render_context.cpp`, button
  in `assets/config_menu/graphics.rml`. In Stretch mode the narrow 304-wide menu layout is
  cropped to its content and stretched too: `mt_dlfix.cpp` calls `rt64_set_horizontal_crop(40/384,
  344/384)` on narrow-layout frames (and resets it otherwise); RT64's VI renderer widens its viewport
  so only that span covers the window.
- Rumble works (confirmed on a real controller). `recomp::update_rumble` (src/game/input.cpp) runs on
  the VI thread every VI; it used to call `SDL_GameControllerRumble` right there, and that write
  (milliseconds on many pads, plus the controller-list lock) delayed VI messages, so every shot that
  rumbled dropped a frame. Now the VI thread only computes the strength (32 steps) and a worker
  thread sends it when it changes.
- `MT_PROF=1` (with `MT_LOG=<file>`) logs late gfx tasks (> 40 ms apart) and slow streaming loads
  (`src/game/mt_prof.cpp`). Busy-wait hooks call `mt_spin_yield`, which wraps `yield_self_1ms`.
- Settings pages were trimmed for this game: Controls show N64 button names (`include/recomp_input.h`)
  with generic icons; General keeps Rumble Strength, Joystick Deadzone and Background Input; Sound
  keeps Main Volume (Zelda-only options removed from the `.rml` files).

## Launcher look
Sunny court theme (picked by the user from three mockups): `assets/mt-splash.svg` is original
artwork (sky, sun, clouds, hills, hedge, grass court, ball); the old night-stadium art is kept in
`build_assets/mt-splash-night-backup.svg`. Title outline/shadow and pill-shaped menu buttons are
styled in the `<style>` block of `assets/launcher.rml`. Title and buttons use Luckiest Guy
(`assets/LuckiestGuy-Regular.ttf`, Apache 2.0, licence in `assets/LuckiestGuy-LICENSE.txt`, from
google/fonts), loaded in `src/ui/ui_state.cpp`, uppercase because its lowercase is small caps.
Settings menus keep the stock fonts.

## Save data
16 Kbit EEPROM. With 4K the game probes, rejects it, never reads/writes and shows "The saved data
has been erased due to corruption or damage" every boot. Saves: `%LOCALAPPDATA%\MarioTennisRecompiled\saves\mt.n64.us.bin`.

## Window code modules (0x10000000)
Seventeen small modules (entries 17, 33, 99-106, 147, 163, 178, 200, 774, 965, 989; < 8 KiB, same
entry stub) are linked at 0x10000000 and run through seg454's sliding 2x4 KiB TLB window over
copies loaded side by side at 0x8023x000. Recompiled as sections `win*` (no relocations); the
loader hook registers each copy at its buffer, and `LOOKUP_FUNC` translates TLB-mapped function
pointers to the physical buffer address (include/recomp.h), so whichever copy is mapped is called.

## TLB-mapped code
Entry 209 (file 0xD1) is linked at 0x50000000 and recompiled as relocatable section `tlb209`
(177 HI16/LO16 relocs from `tools/tlb_relocs.py`); the loader hook registers it at 0x5000xxxx and
at its RAM buffer. With the software TLB in place the relocation is belt-and-braces.

## Next steps
1. A full match now plays (Mario vs Luigi, hard court). Next: longer play, other modes/courts, rumble.
2. Settings menus still list Zelda-only options (targeting, autosave, analog camera); remove them
   and set tennis-friendly control defaults.
3. Watch for `[mt] access to unmapped virtual address` lines during play.
4. ~~Shadow flicker~~ **fixed** (see "Shadows" below).

// Mario Tennis (USA) specific runtime glue: boot, overlay registration and the loader hook.
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstdlib>

#include "mt_game.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#ifndef MT_LIVE_RECOMP
#include "../../RecompiledFuncs/recomp_overlays.inl"
#endif

namespace {
    struct OverlayInfo {
        uint32_t real_rom;   // offset of the LZ-compressed segment in the original ROM
        uint32_t vrom;       // virtual ROM offset of the recompiled section
        uint32_t vram;       // address the segment is linked for
        uint32_t size;       // decompressed size
    };

    constexpr OverlayInfo overlay_table[] = {
#include "mt_overlay_table.inc"
    };

    struct Loaded { uint32_t ram, size; };
    std::vector<Loaded> loaded;  // invariant: never overlapping

    // Fully unload anything the range overwrites, then register the section there.
    void activate_section(uint32_t vrom, uint32_t ram, uint32_t size) {
        for (auto it = loaded.begin(); it != loaded.end();) {
            if (it->ram < ram + size && ram < it->ram + it->size) {
                unload_overlays((int32_t)it->ram, it->size);
                it = loaded.erase(it);
            }
            else {
                ++it;
            }
        }
        load_overlays(vrom, (int32_t)ram, size);
        loaded.push_back({ ram, size });
    }

    constexpr uint32_t boot_vram        = 0x80300000;
    constexpr uint32_t boot_lz_offset   = 0x350;       // compressed main inside the boot copy
    constexpr uint32_t main_vram        = 0x80031000;
    constexpr uint32_t main_lz_size     = 0x2AE10;     // from the LZ header; checked below

    // Same algorithm as the boot loader at 0x80300080 (see tools/mt_lz.py).
    uint32_t lz_decompress(uint8_t* rdram, uint32_t src, uint32_t dst) {
        const uint32_t dst_start = dst;
        src += 4;
        while (true) {
            uint8_t flags = MEM_BU(0, (gpr)(int32_t)src++);
            if (flags == 0) {
                for (int i = 0; i < 8; i++) MEM_B(0, (gpr)(int32_t)dst++) = MEM_BU(0, (gpr)(int32_t)src++);
                continue;
            }
            for (int bit = 0; bit < 8; bit++) {
                if (flags & (0x80 >> bit)) {
                    uint8_t b0 = MEM_BU(0, (gpr)(int32_t)src);
                    uint8_t b1 = MEM_BU(1, (gpr)(int32_t)src);
                    uint32_t dist = ((b0 & 0xF0u) << 4) | b1;
                    if (dist == 0) return dst - dst_start;
                    uint32_t n = b0 & 0xF, len;
                    if (n == 0) { len = MEM_BU(2, (gpr)(int32_t)src) + 0x11u; src += 3; }
                    else { len = (n == 1) ? 2 : n + 1; src += 2; }
                    uint32_t from = dst - dist;
                    for (uint32_t k = 0; k < len; k++) MEM_B(0, (gpr)(int32_t)(dst + k)) = MEM_BU(0, (gpr)(int32_t)(from + k));
                    dst += len;
                }
                else {
                    MEM_B(0, (gpr)(int32_t)dst++) = MEM_BU(0, (gpr)(int32_t)src++);
                }
            }
        }
    }

    void clear_ram(uint8_t* rdram, uint32_t start, uint32_t end) {
        for (uint32_t a = start; a < end; a += 4) MEM_W(0, (gpr)(int32_t)a) = 0;
    }
}

#ifndef MT_LIVE_RECOMP // live builds: src/game/mt_live.cpp
void mt::register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}
#endif

void mt::on_init(uint8_t* rdram, recomp_context* ctx) {
    // IPL3 copies ROM 0x1000..+1 MiB to the header entrypoint (0x80300000).
    recomp::do_rom_read(rdram, (gpr)(int32_t)boot_vram, 0x10001000, 0x100000);

    // Boot stub: clear its .bss, clear 0x80000400-0x80100000, decompress main. The runtime's own
    // 1 MiB copy to 0x80031000 is cleared as well (on hardware that range is not written).
    clear_ram(rdram, 0x803106C0, 0x803106D0);
    clear_ram(rdram, 0x80000400, boot_vram);
    uint32_t out = lz_decompress(rdram, boot_vram + boot_lz_offset, main_vram);
    if (out != main_lz_size) {
        fprintf(stderr, "[mt] main decompressed to 0x%X bytes, expected 0x%X\n", out, main_lz_size);
    }
    loaded.clear();
    fprintf(stderr, "[mt] boot: main decompressed (0x%X bytes), entering 0x%08X\n", out, main_vram);

    // Registers as the stub leaves them when it jumps to main.
    ctx->r29 = (gpr)(int32_t)0x803FF000;  // sp
    ctx->r4 = 0x116C0;                    // ROM offset of the segment table
    ctx->r5 = 0;                          // COP0 Count at boot start
    ctx->r6 = 0;                          // COP0 Count after decompression
}

// ROM offset passed to the loader, captured at its entry (the loader advances its own copy).
static thread_local uint32_t loader_start_rom = 0;

void mt_prof_load_begin();
void mt_prof_load_end(uint32_t rom, uint32_t dest, uint32_t size, bool code);

extern "C" void mt_loader_begin(uint32_t rom) {
    loader_start_rom = rom;
    mt_prof_load_begin();
}

extern "C" void mt_on_segment_loaded(uint8_t* rdram, uint32_t rom_end, uint32_t dest, uint32_t size) {
    (void)rdram;
    (void)rom_end;
    uint32_t rom = loader_start_rom;
    uint32_t rom_offset = rom & 0x0FFFFFFF;
    const OverlayInfo* info = nullptr;
    for (const auto& o : overlay_table) {
        if (o.real_rom == rom_offset) { info = &o; break; }
    }
    mt_prof_load_end(rom_offset, dest, size, info != nullptr);
    if (info == nullptr) {  // data segment
        static int logged = 0;
        if (std::getenv("MT_LOG_LOADS") != nullptr && logged++ < 200) {
            fprintf(stderr, "[mt] data segment rom 0x%08X (raw 0x%08X) -> 0x%08X (0x%X bytes)\n", rom_offset, rom, dest, size);
        }
        return;
    }
    fprintf(stderr, "[mt] code segment rom 0x%08X -> 0x%08X (0x%X bytes)\n", rom_offset, dest, size);

    // Code linked outside KSEG0 runs through a TLB mapping onto the buffer it was loaded into.
    const bool tlb = (info->vram & 0xE0000000) != 0x80000000;
    if ((!tlb && dest != info->vram) || size != info->size) {
        fprintf(stderr, "[mt] code segment rom 0x%08X loaded at 0x%08X size 0x%X (expected 0x%08X size 0x%X)\n",
            rom_offset, dest, size, info->vram, info->size);
    }

    auto activate = [&](uint32_t ram) { activate_section(info->vrom, ram, info->size); };

    // seg71: centre the 304x224 screen layout (boot logos etc.) in the 384-wide framebuffer by moving
    // its static viewport {608,448,511,0},{608,448,511,0} at 0x80124648 right by 40 px (trans.x 608 ->
    // 768). The matching scissors are widened by instruction patches in mt.us.toml.
    if (rom_offset == 0x009CD770 && dest == 0x800E8000) {
        constexpr gpr vp = (gpr)(int32_t)0x80124648;
        if (MEM_HU(0, vp) == 608 && MEM_HU(2, vp) == 448 && MEM_HU(8, vp) == 608 && MEM_HU(10, vp) == 448) {
            MEM_H(8, vp) = 768;
        }
    }

    if (tlb) {
        // Registered at its linked address and (last, so it is the relocation base) at the buffer.
        // Calls through any TLB alias or sliding window resolve via LOOKUP_FUNC's TLB translation
        // (include/recomp.h) to the buffer address.
        activate(info->vram);
    }
    activate(dest);
}

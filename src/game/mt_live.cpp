// Runtime ("live") recompilation of the game's code from the player's own ROM.
//
// Builds with MT_LIVE_RECOMP=ON contain no recompiled game code. When the game starts, this file:
//   1. decompresses the ROM's code segments into a recompiler image (same layout as tools/gen_symbols.py),
//   2. builds an N64Recomp context from the tables in mt_live_data.inc (our own symbol/patch data),
//   3. recompiles every function to native code in memory with N64Recomp's live generator, and
//   4. registers the resulting section table with the runtime, exactly like the offline build's
//      RecompiledFuncs/recomp_overlays.inl does.
// Patches and hooks from mt.us.toml are applied the same way N64Recomp's CLI applies them; hooks run as
// events (see the "@event:N" support in the N64Recomp patch).

#include <chrono>
#include <io.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"
#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"
#include "ultramodern/error_handling.hpp"

namespace {
    struct MtLiveSegment { uint32_t real_rom; uint32_t vrom; };
    struct MtLiveFunc { const char* name; uint32_t vram; uint32_t size; };
    struct MtLiveSection { const char* name; uint32_t rom; uint32_t vram; uint32_t size; uint32_t first_func; uint32_t num_funcs;
                           bool relocatable; uint32_t first_reloc; uint32_t num_relocs; };
    struct MtLiveReloc { uint32_t vram; uint32_t target_vram; const char* type; };
    struct MtLivePatch { const char* func; uint32_t vram; uint32_t value; };
    struct MtLiveHook { const char* func; uint32_t before_vram; };
    struct MtLiveNative { const char* name; recomp_func_t* func; };
}

#include "mt_live_data.inc"

extern "C" {
    gpr mt_xlat_slow(gpr address);
    gpr mt_xlat_func_slow(gpr address);
    gpr cop0_status_read(recomp_context* ctx);
    void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
    void do_break(uint32_t vram);
    void pause_self(uint8_t* rdram);
    extern int32_t* section_addresses;
}

namespace {
    constexpr uint32_t entrypoint_vram = 0x80031000;

    // Mario Tennis' LZ format (see tools/mt_lz.py).
    std::vector<uint8_t> lz_decompress(std::span<const uint8_t> src, size_t start) {
        std::vector<uint8_t> out;
        size_t i = start + 4;
        while (i < src.size()) {
            uint8_t flags = src[i++];
            if (flags == 0) {
                for (int k = 0; k < 8 && i < src.size(); k++) out.push_back(src[i++]);
                continue;
            }
            for (int bit = 0; bit < 8; bit++) {
                if (flags & (0x80 >> bit)) {
                    uint8_t b0 = src[i], b1 = src[i + 1];
                    size_t dist = ((b0 & 0xF0) << 4) | b1;
                    if (dist == 0) return out;
                    size_t n = b0 & 0xF, length;
                    if (n == 0) { length = src[i + 2] + 0x11; i += 3; }
                    else { length = n == 1 ? 2 : n + 1; i += 2; }
                    size_t p = out.size() - dist;
                    for (size_t k = 0; k < length; k++) out.push_back(out[p + k]);
                }
                else {
                    out.push_back(src[i++]);
                }
            }
        }
        return out;
    }

    N64Recomp::RelocType reloc_type(const char* name) {
        std::string n = name;
        if (n == "R_MIPS_HI16") return N64Recomp::RelocType::R_MIPS_HI16;
        if (n == "R_MIPS_LO16") return N64Recomp::RelocType::R_MIPS_LO16;
        if (n == "R_MIPS_26") return N64Recomp::RelocType::R_MIPS_26;
        if (n == "R_MIPS_32") return N64Recomp::RelocType::R_MIPS_32;
        return N64Recomp::RelocType::R_MIPS_NONE;
    }

    RelocEntryType reloc_entry_type(N64Recomp::RelocType t) {
        switch (t) {
            case N64Recomp::RelocType::R_MIPS_HI16: return R_MIPS_HI16;
            case N64Recomp::RelocType::R_MIPS_LO16: return R_MIPS_LO16;
            case N64Recomp::RelocType::R_MIPS_26: return R_MIPS_26;
            case N64Recomp::RelocType::R_MIPS_32: return R_MIPS_32;
            default: return R_MIPS_NONE;
        }
    }

    recomp_func_t* live_get_function(int32_t vram) {
        if (((uint32_t)vram & 0xE0000000u) != 0x80000000u) {
            vram = (int32_t)mt_xlat_func_slow((gpr)vram);
        }
        return get_function(vram);
    }

    void live_trigger_event(uint8_t* rdram, recomp_context* ctx, uint32_t index) {
        mt_live_run_hook(rdram, ctx, index);
    }

    void live_cop0_status_write(recomp_context* ctx, gpr value) {
        cop0_status_write(ctx, value);
    }

    // Everything the runtime keeps pointers into must outlive the game.
    std::unique_ptr<N64Recomp::LiveGeneratorOutput> live_output;
    std::vector<std::vector<FuncEntry>> section_funcs;
    std::vector<std::vector<RelocEntry>> section_relocs;
    std::vector<SectionTableEntry> section_table;
    int overlay_sections_by_index[] = { -1 };
    recomp_func_t* entry_func = nullptr;

    [[noreturn]] void fail(const std::string& msg) {
        ultramodern::error_handling::message_box(("Could not prepare the game code from your ROM:\n" + msg).c_str());
        std::exit(EXIT_FAILURE);
    }

    // N64Recomp prints [Info] notes with fmt::print, which throws (and so ends the program) if the stream
    // can't be written -- the normal case for a double-clicked GUI exe with no console. Point any missing
    // standard stream at NUL first.
    void ensure_writable_std_streams() {
        for (FILE* f : { stdout, stderr }) {
            int fd = _fileno(f);
            if (fd < 0 || _get_osfhandle(fd) == -1 || _get_osfhandle(fd) == -2) {
                FILE* dummy = nullptr;
                freopen_s(&dummy, "NUL", "w", f);
            }
        }
    }

    void build() {
        ensure_writable_std_streams();
        auto t0 = std::chrono::steady_clock::now();
        std::span<const uint8_t> rom = recomp::get_rom();

        // 1. Recompiler image: the ROM with every code segment decompressed at its virtual ROM offset.
        std::vector<uint8_t> image(rom.begin(), rom.end());
        for (const MtLiveSegment& seg : mt_live_segments) {
            std::vector<uint8_t> data = lz_decompress(rom, seg.real_rom);
            if (image.size() < seg.vrom + data.size()) image.resize((seg.vrom + data.size() + 0xF) & ~size_t(0xF), 0);
            std::memcpy(image.data() + seg.vrom, data.data(), data.size());
        }

        // 2. Context (mirrors Context::from_symbol_file and N64Recomp's main.cpp).
        N64Recomp::Context context{};
        context.section_functions.resize(std::size(mt_live_sections));
        for (const MtLiveSection& s : mt_live_sections) {
            uint16_t section_index = (uint16_t)context.sections.size();
            N64Recomp::Section& section = context.sections.emplace_back();
            section.rom_addr = s.rom;
            section.ram_addr = s.vram;
            section.size = s.size;
            section.name = s.name;
            section.executable = true;
            section.relocatable = s.relocatable;
            for (uint32_t i = 0; i < s.num_funcs; i++) {
                const MtLiveFunc& f = mt_live_funcs[s.first_func + i];
                size_t function_index = context.functions.size();
                N64Recomp::Function func{};
                func.name = f.name;
                func.vram = f.vram;
                func.rom = f.vram - s.vram + s.rom;
                func.section_index = section_index;
                if (func.rom + f.size > image.size()) fail(std::string("function out of bounds: ") + f.name);
                func.words.resize(f.size / 4);
                std::memcpy(func.words.data(), image.data() + func.rom, func.words.size() * 4);
                section.function_addrs.push_back(func.vram);
                context.functions_by_name[func.name] = function_index;
                context.functions_by_vram[func.vram].push_back(function_index);
                context.section_functions[section_index].push_back(function_index);
                context.functions.emplace_back(std::move(func));
            }
            for (uint32_t i = 0; i < s.num_relocs; i++) {
                const MtLiveReloc& r = mt_live_relocs[s.first_reloc + i];
                N64Recomp::Reloc reloc{};
                reloc.address = r.vram;
                reloc.target_section_offset = r.target_vram - s.vram;
                reloc.symbol_index = (uint32_t)-1;
                reloc.target_section = section_index;
                reloc.type = reloc_type(r.type);
                section.relocs.push_back(reloc);
            }
        }
        context.rom = image;

        std::unordered_map<std::string, recomp_func_t*> natives;
        for (const MtLiveNative& n : mt_live_natives) natives[n.name] = n.func;

        for (N64Recomp::Function& func : context.functions) {
            if (N64Recomp::reimplemented_funcs.contains(func.name)) {
                func.reimplemented = true;
                func.ignored = true;
            }
            else if (N64Recomp::ignored_funcs.contains(func.name)) {
                func.ignored = true;
            }
        }
        for (const char* name : mt_live_ignored) {
            if (name == nullptr) continue;
            auto it = context.functions_by_name.find(name);
            if (it == context.functions_by_name.end()) fail(std::string("ignored function not found: ") + name);
            context.functions[it->second].ignored = true;
        }
        for (const MtLivePatch& p : mt_live_patches) {
            auto it = context.functions_by_name.find(p.func);
            if (it == context.functions_by_name.end()) fail(std::string("patched function not found: ") + p.func);
            N64Recomp::Function& func = context.functions[it->second];
            func.words[(p.vram - func.vram) / 4] = byteswap(p.value);
        }
        for (size_t i = 0; i < std::size(mt_live_hooks); i++) {
            const MtLiveHook& h = mt_live_hooks[i];
            auto it = context.functions_by_name.find(h.func);
            if (it == context.functions_by_name.end()) fail(std::string("hooked function not found: ") + h.func);
            N64Recomp::Function& func = context.functions[it->second];
            int32_t index = h.before_vram == 0 ? -1 : (int32_t)((h.before_vram - func.vram) / 4);
            func.function_hooks[index] = "@event:" + std::to_string(i);
        }

        // 3. Recompile.
        N64Recomp::LiveGeneratorInputs inputs{
            .base_event_index = 0,
            .cop0_status_write = live_cop0_status_write,
            .cop0_status_read = cop0_status_read,
            .switch_error = switch_error,
            .do_break = do_break,
            .get_function = live_get_function,
            .syscall_handler = nullptr,
            .pause_self = pause_self,
            .trigger_event = live_trigger_event,
            .reference_section_addresses = nullptr,
            .local_section_addresses = section_addresses,
            .run_hook = nullptr,
            .entry_func_hooks = {},
            .return_func_hooks = {},
            .original_section_indices = {},
            .translate_address = mt_xlat_slow,
            .lookup_uncompiled_functions = true,
        };
        N64Recomp::LiveGenerator generator{ context.functions.size(), inputs };
        std::vector<std::vector<uint32_t>> static_funcs(context.sections.size());
        size_t compiled = 0;
        for (size_t i = 0; i < context.functions.size(); i++) {
            const N64Recomp::Function& func = context.functions[i];
            if (func.ignored || func.words.empty()) continue;
            std::ostringstream dummy;
            if (!N64Recomp::recompile_function_live(generator, context, i, dummy, static_funcs, false)) {
                fail("failed to recompile " + func.name);
            }
            compiled++;
        }
        live_output = std::make_unique<N64Recomp::LiveGeneratorOutput>(generator.finish());
        if (!live_output->good) fail("code generation failed");

        // 4. Section table.
        section_funcs.assign(context.sections.size(), {});
        section_relocs.assign(context.sections.size(), {});
        section_table.clear();
        for (size_t si = 0; si < context.sections.size(); si++) {
            const N64Recomp::Section& section = context.sections[si];
            for (size_t fi : context.section_functions[si]) {
                const N64Recomp::Function& func = context.functions[fi];
                recomp_func_t* ptr = live_output->functions[fi];
                uint32_t size = (uint32_t)(func.words.size() * 4);
                if (func.ignored) {
                    auto it = natives.find(func.name);
                    if (it == natives.end()) continue; // never called (or a libultra internal the game doesn't use)
                    ptr = it->second;
                    if (func.reimplemented) size = 0;
                }
                if (ptr == nullptr) continue;
                section_funcs[si].push_back(FuncEntry{ .func = ptr, .offset = func.rom - section.rom_addr, .rom_size = size });
                if (func.vram == entrypoint_vram && si == 0) entry_func = ptr;
            }
            for (const N64Recomp::Reloc& r : section.relocs) {
                section_relocs[si].push_back(RelocEntry{ .offset = r.address - section.ram_addr, .target_section_offset = r.target_section_offset,
                                                         .target_section = r.target_section, .type = reloc_entry_type(r.type) });
            }
            section_table.push_back(SectionTableEntry{
                .rom_addr = section.rom_addr, .ram_addr = section.ram_addr, .size = section.size,
                .funcs = section_funcs[si].data(), .num_funcs = section_funcs[si].size(),
                .relocs = section_relocs[si].empty() ? nullptr : section_relocs[si].data(), .num_relocs = section_relocs[si].size(),
                .index = si });
        }
        if (entry_func == nullptr) fail("entrypoint not found");

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[mt] live recompiled %zu functions (%zu KiB native code) in %lld ms\n",
                     compiled, live_output->code_size / 1024, (long long)ms);
    }

    void on_rom_loaded() {
        // The runtime's section address array must exist before code is generated (its address is baked
        // in), so register a provisional table first, then the real one.
        static SectionTableEntry empty{};
        recomp::overlays::register_overlays({ .code_sections = &empty, .num_code_sections = 0, .total_num_sections = std::size(mt_live_sections) },
                                            { .table = overlay_sections_by_index, .len = std::size(overlay_sections_by_index) });
        build();
        recomp::overlays::register_overlays({ .code_sections = section_table.data(), .num_code_sections = section_table.size(),
                                              .total_num_sections = std::size(mt_live_sections) },
                                            { .table = overlay_sections_by_index, .len = std::size(overlay_sections_by_index) });
    }
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx) {
    entry_func(rdram, ctx);
}

namespace mt {
    void register_overlays() {
        recomp::set_rom_loaded_callback(on_rom_loaded);
    }
}

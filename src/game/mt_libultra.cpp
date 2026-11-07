// Hand-written replacements for Mario Tennis functions that touch libultra internals the runtime
// does not emulate. Each one is listed in `ignored` in mt.us.toml (or is a libultra name the
// runtime leaves out); see tools/libultra_names.py for how they were identified.
#include <cstdio>
#include "recomp.h"

namespace {
    // libultra PFS error codes (os_pfs.h).
    constexpr int32_t PFS_ERR_NOPACK = 1;

    void return_no_pack(recomp_context* ctx) { ctx->r2 = PFS_ERR_NOPACK; }
}

extern "C" void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx);

// 0x80031C4C: a second, byte-identical copy of osStartThread linked into the game.
extern "C" void func_80031C4C(uint8_t* rdram, recomp_context* ctx) {
    osStartThread_recomp(rdram, ctx);
}

// Controller Pak RAM access. The runtime has no pak bus; the only users are the Game Boy
// link-up (Transfer Pak) paths in seg71, which treat "no pack" as an unplugged accessory.
extern "C" void __osContRamRead_recomp(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }
extern "C" void __osContRamWrite_recomp(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }
extern "C" void __osPfsGetStatus_recomp(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }

// Transfer Pak (osGbpak*) entry points in main: report that no pak is inserted.
extern "C" void func_80048F38(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }
extern "C" void func_80049224(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }
extern "C" void func_80049388(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }
extern "C" void func_80049554(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }
extern "C" void func_8004960C(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }
extern "C" void func_80049950(uint8_t*, recomp_context* ctx) { return_no_pack(ctx); }

// Diagnostics hook target (see mt.us.toml): logs memory-fill calls that would leave RDRAM.
extern "C" void mt_check_range(const char* what, uint32_t addr, uint32_t size, uint32_t ra) {
    if (addr < 0x80000000u || addr + size > 0x80800000u || size > 0x800000u) {
        fprintf(stderr, "[mt] %s(0x%08X, 0x%X) from ra 0x%08X leaves RDRAM\n", what, addr, size, ra);
    }
}

// Trace hook target for trace builds (tools/make_trace_config.py): logs each function's first call.
#include <mutex>
#include <unordered_set>
#include <thread>
#include <sstream>
extern "C" void mt_trace(uint32_t vram) {
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard lock{ m };
    if (seen.insert(vram).second) {
        std::ostringstream tid; tid << std::this_thread::get_id();
        fprintf(stderr, "[trace] %08X (thread %s)\n", vram, tid.str().c_str());
    }
}

extern "C" void mt_log3(const char* what, uint32_t a, uint32_t b, uint32_t c) {
    fprintf(stderr, "[mt] %s = 0x%08X, 0x%X, %u\n", what, a, b, c);
}

// Diagnostics: dump an OSViMode (type, comRegs, fldRegs[2]) from RDRAM.
extern "C" void mt_dump_vimode(uint8_t* rdram, uint32_t addr) {
    fprintf(stderr, "[mt] OSViMode @%08X:", addr);
    for (int i = 0; i < 20; i++) fprintf(stderr, " %08X", (uint32_t)MEM_W(4 * i, (gpr)(int32_t)addr));
    fprintf(stderr, "\n");
}

// MT_PROF=1: timing diagnostics for stutter hunting, written to stderr (see MT_LOG in main.cpp).
//   - gfx task intervals longer than 40 ms (a missed frame at 30 fps), with what happened since the
//     previous task: streaming-loader calls, time spent in them, busy-wait yields;
//   - every streaming-loader call that takes longer than 2 ms.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {
    using clk = std::chrono::steady_clock;
    const bool enabled = std::getenv("MT_PROF") != nullptr;
    const clk::time_point t0 = clk::now();
    double now_ms() { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }

    thread_local double load_start = 0;
    double last_task = 0;
    unsigned loads = 0, yields[3] = {};
    double load_ms = 0, yield_ms = 0;
}

extern "C" void yield_self_1ms(uint8_t* rdram);

extern "C" void mt_spin_yield(uint8_t* rdram, uint32_t id) {
    if (!enabled) { yield_self_1ms(rdram); return; }
    double s = now_ms();
    yield_self_1ms(rdram);
    yield_ms += now_ms() - s;
    yields[id < 3 ? id : 0]++;
}

extern "C" void mt_prof_event(const char* what, uint32_t arg);
void mt_prof_load_begin() {
    if (enabled) { load_start = now_ms(); mt_prof_event("load begin", 0); }
}

bool mt_heap_check(const char* after, uint32_t rom, uint32_t dest, uint32_t size);

void mt_prof_load_end(uint32_t rom, uint32_t dest, uint32_t size, bool code) {
    if (!enabled) return;
    mt_heap_check(code ? "code load" : "data load", rom, dest, size);
    double d = now_ms() - load_start;
    loads++; load_ms += d;
    mt_prof_event(code ? "code load end rom" : "data load end rom", rom);
    if (d > 2.0) fprintf(stderr, "[prof] %9.1f ms: %s load rom 0x%08X -> 0x%08X (0x%X bytes) took %.2f ms\n",
        load_start, code ? "code" : "data", rom, dest, size, d);
}

void mt_prof_flush_events(bool print);
extern "C" void mt_prof_event(const char* what, uint32_t arg);

void mt_prof_gfx_task() {
    if (!enabled) return;
    mt_heap_check("frame", 0, 0, 0);
    double t = now_ms();
    bool late = last_task != 0 && t - last_task > 40.0;
    mt_prof_flush_events(late);
    mt_prof_event("gfx task", 0);
    if (late) {
        fprintf(stderr, "[prof] %9.1f ms: gfx task gap %.1f ms (loads %u / %.1f ms, yields %u+%u / %.1f ms)\n",
            t, t - last_task, loads, load_ms, yields[1], yields[2], yield_ms);
    }
    last_task = t;
    loads = 0; load_ms = 0; yields[0] = yields[1] = yields[2] = 0; yield_ms = 0;
}

// Times a span measured elsewhere (renderer, RSP tasks); logs it when it exceeds threshold_ms.
extern "C" void mt_prof_span(const char* what, double ms, double threshold_ms) {
    if (enabled && ms > threshold_ms) fprintf(stderr, "[prof] %9.1f ms: %s took %.1f ms\n", now_ms(), what, ms);
}
extern "C" int mt_prof_enabled() { return enabled ? 1 : 0; }

// Event timeline: buffered per gfx task and printed for the tasks that came late.
#include <mutex>
#include <vector>
namespace {
    struct Ev { double t; const char* what; uint32_t arg; };
    std::mutex ev_mutex;
    std::vector<Ev> events;
}
extern "C" void mt_prof_event(const char* what, uint32_t arg) {
    if (!enabled) return;
    std::lock_guard lock{ ev_mutex };
    if (events.size() < 4096) events.push_back({ now_ms(), what, arg });
}
void mt_prof_flush_events(bool print) {
    std::lock_guard lock{ ev_mutex };
    if (print) for (const Ev& e : events) fprintf(stderr, "[prof]      %9.1f  %s 0x%X\n", e.t, e.what, e.arg);
    events.clear();
}

// Freeze watchdog (MT_PROF): if no gfx task arrives for 4 s, print every game thread's state and the
// message queue it is blocked on, so a hang can be traced to what each thread is waiting for.
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ultramodern/ultramodern.hpp"

extern "C" const char* volatile mt_rt64_phase;

namespace {
    std::mutex watch_mutex;
    std::vector<uint32_t> watched_threads;
    uint8_t* watch_rdram = nullptr;

    uint32_t rd32(uint32_t addr) { return *reinterpret_cast<uint32_t*>(watch_rdram + (addr & 0x1FFFFFFC)); }

    void dump_threads(double stalled_ms) {
        std::lock_guard lock{ watch_mutex };
        fprintf(stderr, "[prof] FREEZE? no gfx task for %.0f ms. Renderer last finished step: %s. Game threads:\n", stalled_ms, mt_rt64_phase);
        for (uint32_t t : watched_threads) {
            const OSThread* th = reinterpret_cast<const OSThread*>(watch_rdram + (t & 0x1FFFFFFF));
            uint32_t q = (uint32_t)th->queue;
            fprintf(stderr, "[prof]   thread %d @%08X pri %d blocked on mq %08X (count %d)", th->id, t, th->priority, th->pad2,
                    th->pad2 ? (int32_t)rd32(th->pad2 + 8) : 0);
            q = 0;
            if (q != 0) {
                // A blocked thread's queue is &mq->blocked_on_recv (offset 0) or &mq->blocked_on_send (4).
                fprintf(stderr, " (as recv: count %d/%d; as send: count %d/%d)",
                    (int32_t)rd32(q + 8), (int32_t)rd32(q + 16), (int32_t)rd32(q + 4), (int32_t)rd32(q + 12));
            }
            fprintf(stderr, "\n");
        }
        // First freeze only: save the 8 MiB of game memory (byte order as in the runtime) for offline study.
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            const char* log = std::getenv("MT_LOG");
            std::string path = log ? std::string(log) + ".rdram.bin" : "freeze_rdram.bin";
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fwrite(watch_rdram, 1, 0x800000, f);
                fclose(f);
                fprintf(stderr, "[prof] game memory saved to %s\n", path.c_str());
            }
        }
        fflush(stderr);
    }
}

void mt_dump_all_stacks();

// Heap integrity (MT_PROF): the game's allocator (func_80038040) keeps a table of 24-byte heap
// descriptors at 0x8005DC20; +8 is the head of a circular list of block headers
// { +0 size, +4 in-use, +8 next }. Reports the first time any list stops being well formed,
// together with what happened just before (a file load, or just a frame).
bool mt_heap_check(const char* after, uint32_t rom, uint32_t dest, uint32_t size) {
    static bool reported = false;
    static std::string last_ok_event = "(none)";
    if (reported || watch_rdram == nullptr) return true;
    constexpr uint32_t table = 0x8005DC20;
    auto valid = [](uint32_t p) { return p >= 0x80000000u && p < 0x80800000u && (p & 3) == 0; };
    for (uint32_t h = 0; h < 16; h++) {
        uint32_t sentinel = table + h * 24;
        uint32_t node = rd32(sentinel + 8);
        if (!valid(node)) continue;   // unused descriptor
        uint32_t prev = sentinel;
        for (int n = 0; n < 20000 && node != sentinel; n++) {
            uint32_t next = rd32(node + 8);
            if (!valid(next) || n == 19999) {
                reported = true;
                fprintf(stderr, "[prof] HEAP BROKEN after %s rom 0x%08X -> 0x%08X (0x%X bytes): heap %u, block %08X "
                        "(size %08X used %08X next %08X), previous block %08X. Last good check: %s\n",
                        after, rom, dest, size, h, node, rd32(node), rd32(node + 4), next, prev, last_ok_event.c_str());
                // Show every block of this heap up to the broken one.
                uint32_t q = rd32(sentinel + 8);
                for (int k = 0; k < 400 && valid(q) && q != sentinel; k++) {
                    fprintf(stderr, "[prof]     block %08X size %08X used %08X next %08X\n", q, rd32(q), rd32(q + 4), rd32(q + 8));
                    if (q == node) break;
                    q = rd32(q + 8);
                }
                fflush(stderr);
                return false;
            }
            prev = node; node = next;
        }
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "%s rom 0x%08X -> 0x%08X (0x%X bytes)", after, rom, dest, size);
    if (rom != 0) last_ok_event = buf;
    return true;
}

void mt_prof_thread_created(uint8_t* rdram) {
    if (!enabled) return;
    {
        std::lock_guard lock{ watch_mutex };
        watch_rdram = rdram;
        watched_threads.push_back((uint32_t)ultramodern::this_thread());
    }
    static bool started = false;
    if (!started) {
        started = true;
        std::thread([] {
            double last_dump = 0;
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                double t = now_ms();
                if (last_task != 0 && t - last_task > 4000 && t - last_dump > 10000) {
                    dump_threads(t - last_task);
                    mt_dump_all_stacks();
                    last_dump = t;
                }
            }
        }).detach();
    }
}

#ifdef _WIN32
// Freeze watchdog, part 2: print every thread's call stack (symbols from the .pdb next to the exe).
#define NOMINMAX
#include <cstring>
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// Game call chain: recompiled game code has no unwind info, so scan raw stack memory for return
// addresses that fall inside it (a few entries may be stale leftovers, but the order is innermost first).
#ifdef MT_LIVE_RECOMP
extern "C" const char* mt_live_describe(const void* addr, uintptr_t* offset);
#else
extern "C" const char* mt_live_describe(const void*, uintptr_t*) { return nullptr; }
#endif
extern "C" void mt_print_game_stack(const void* pc, const void* sp, size_t bytes) {
    uintptr_t off = 0;
    if (const char* n = mt_live_describe(pc, &off)) fprintf(stderr, "[mt]     in game code: %s+0x%llX\n", n, (unsigned long long)off);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(sp, &mbi, sizeof(mbi))) return;
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    uintptr_t p = reinterpret_cast<uintptr_t>(sp) & ~uintptr_t(7);
    int shown = 0;
    for (; p + 8 <= end && p < reinterpret_cast<uintptr_t>(sp) + bytes && shown < 16; p += 8) {
        const void* v = *reinterpret_cast<const void* const*>(p);
        if (const char* n = mt_live_describe(v, &off); n != nullptr && off != 0) {
            fprintf(stderr, "[mt]       called from %s+0x%llX\n", n, (unsigned long long)off);
            shown++;
        }
    }
}

// Crash reports: print the faulting thread's call stack with module and function names (symbols come from
// .pdb files next to the exe and DLLs, e.g. MarioTennisRecompiled.pdb and librashader.pdb).
void mt_print_crash_stack(void* context_record) {
    HANDLE process = GetCurrentProcess();
    static bool sym_ready = false;
    if (!sym_ready) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        sym_ready = true;
    }
    CONTEXT ctx = *static_cast<CONTEXT*>(context_record);
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0; depth < 32; depth++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) break;
        char module_path[MAX_PATH] = "?";
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)pc, &module)) {
            GetModuleFileNameA(module, module_path, MAX_PATH);
        }
        const char* module_name = strrchr(module_path, '\\') ? strrchr(module_path, '\\') + 1 : module_path;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        uintptr_t game_offset = 0;
        const char* game_name = mt_live_describe(reinterpret_cast<void*>(pc), &game_offset);
        if (game_name != nullptr) {
            fprintf(stderr, "[mt]   at game code %s+0x%llX\n", game_name, (unsigned long long)game_offset);
        }
        else if (SymFromAddr(process, pc, &disp, sym)) {
            fprintf(stderr, "[mt]   at %s!%s+0x%llX\n", module_name, sym->Name, (unsigned long long)disp);
        }
        else {
            fprintf(stderr, "[mt]   at %s+0x%llX\n", module_name, (unsigned long long)(pc - (DWORD64)(uintptr_t)module));
        }
    }
    fflush(stderr);
}

void mt_dump_all_stacks() {
    static bool sym_ready = false;
    HANDLE process = GetCurrentProcess();
    if (!sym_ready) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        sym_ready = true;
    }
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
                char line_buf[2048]; size_t len = 0;
                bool interesting = false;
                for (int depth = 0; depth < 14; depth++) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, th, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
                    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
                    DWORD64 disp = 0;
                    const char* name = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
                    if (strstr(name, "RT64") || strstr(name, "render") || strstr(name, "Render")) interesting = true;
                    len += snprintf(line_buf + len, sizeof(line_buf) - len, "\n[prof]       %s", name);
                    if (len >= sizeof(line_buf) - 64) break;
                }
                if (interesting) fprintf(stderr, "[prof]   host thread %lu:%s\n", te.th32ThreadID, line_buf);
                // Threads running (or stuck in) game code: show where.
                CONTEXT gctx{}; gctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &gctx)) {
                    uintptr_t o = 0;
                    bool in_game = mt_live_describe(reinterpret_cast<void*>(gctx.Rip), &o) != nullptr;
                    // Or blocked in a host call made from game code: look for game return addresses.
                    for (uintptr_t p = gctx.Rsp; !in_game && p < gctx.Rsp + 0x400; p += 8) {
                        void* v = nullptr; SIZE_T got = 0;
                        if (!ReadProcessMemory(process, reinterpret_cast<void*>(p), &v, 8, &got)) break;
                        in_game = mt_live_describe(v, &o) != nullptr;
                    }
                    if (in_game) {
                        fprintf(stderr, "[prof]   host thread %lu runs game code (first frame %s):\n", te.th32ThreadID, line_buf);
                        mt_print_game_stack(reinterpret_cast<void*>(gctx.Rip), reinterpret_cast<void*>(gctx.Rsp), 0x4000);
                    }
                }
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    fflush(stderr);
}
#else
void mt_dump_all_stacks() {}
#endif

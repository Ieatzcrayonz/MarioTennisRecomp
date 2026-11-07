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

void mt_prof_load_end(uint32_t rom, uint32_t dest, uint32_t size, bool code) {
    if (!enabled) return;
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

// Display-list inspector (diagnostics). Set MT_DUMP_DL=<n> to summarise the n-th graphics task and
// the following MT_DUMP_DL_COUNT (default 2) tasks: every distinct RDP/RSP render state with the
// number of triangles drawn in it. Called from a hook before osSpTaskLoad in the gfx thread.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <tuple>
#include "recomp.h"

namespace {
    struct State {
        uint32_t oml = 0, omh = 0, cmb_hi = 0, cmb_lo = 0, prim = 0, env = 0, blend = 0, geom = 0;
        auto key() const { return std::make_tuple(oml, omh, cmb_hi, cmb_lo, prim, env, blend, geom); }
    };
    struct Stats { uint32_t tris = 0, first_dl = 0, first_vtx = 0; };

    uint32_t read32(uint8_t* rdram, uint32_t phys) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF)));
    }
}

extern "C" void mt_dump_gfx_task(uint8_t* rdram, uint32_t task) {
    static int task_index = 0;
    static const char* env = std::getenv("MT_DUMP_DL");
    static const char* env_count = std::getenv("MT_DUMP_DL_COUNT");
    int cur = task_index++;
    if (env == nullptr) return;
    static const char* env_step = std::getenv("MT_DUMP_DL_STEP");
    int first = std::atoi(env), count = env_count ? std::atoi(env_count) : 2;
    int step = env_step ? std::atoi(env_step) : 1;
    if (cur < first || cur >= first + count * step || (cur - first) % step != 0) return;

    uint32_t dl = (uint32_t)MEM_W(0x30, (gpr)(int32_t)task) & 0x1FFFFFFF;
    uint32_t seg[16] = {};
    uint32_t stack[32]; int sp = 0;
    uint32_t pc = dl, vtx_addr = 0;
    State st;
    std::map<decltype(st.key()), Stats> stats;
    auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0x1FFFFFFF; };

    for (int n = 0; n < 400000; n++) {
        uint32_t w0 = read32(rdram, pc), w1 = read32(rdram, pc + 4);
        uint32_t cur_pc = pc; pc += 8;
        uint8_t op = w0 >> 24;
        switch (op) {
            case 0x01: vtx_addr = resolve(w1); break;                                   // G_VTX
            case 0x05: case 0x06: case 0x07: {                                         // TRI1/TRI2/QUAD
                Stats& s = stats[st.key()];
                if (s.tris == 0) { s.first_dl = cur_pc; s.first_vtx = vtx_addr; }
                s.tris += (op == 0x05) ? 1 : 2;
                break;
            }
            case 0xD9: st.geom = (st.geom & (w0 & 0xFFFFFF)) | w1; break;              // GEOMETRYMODE
            case 0xDB: if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x1FFFFFFF; break;
            case 0xDE:                                                                  // G_DL
                if (((w0 >> 16) & 0xFF) == 0 && sp < 32) stack[sp++] = pc;
                pc = resolve(w1);
                break;
            case 0xDF: if (sp == 0) goto done; pc = stack[--sp]; break;                // ENDDL
            case 0xE2: case 0xE3: {                                                    // SETOTHERMODE_L/H
                uint32_t len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
                uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                uint32_t& m = (op == 0xE2) ? st.oml : st.omh;
                m = (m & ~mask) | (w1 & mask);
                break;
            }
            case 0xEF: st.omh = w0 & 0xFFFFFF; st.oml = w1; break;                      // RDPSETOTHERMODE
            case 0xFC: st.cmb_hi = w0 & 0xFFFFFF; st.cmb_lo = w1; break;                 // SETCOMBINE
            case 0xFA: st.prim = w1; break;
            case 0xFB: st.env = w1; break;
            case 0xF9: st.blend = w1; break;
            case 0xE4:                                                                  // TEXRECT + 2 halves
                fprintf(stderr, "[dl]   texrect %u,%u - %u,%u (dl 0x%06X)\n", ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4,
                    ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4, cur_pc);
                pc += 16;
                break;
            case 0xF6:                                                                  // FILLRECT
                fprintf(stderr, "[dl]   fillrect %u,%u - %u,%u\n", ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4,
                    ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4);
                break;
            case 0xDC:                                                                  // MOVEMEM
                if ((w0 & 0xFF) == 8) {                                                 // G_MV_VIEWPORT
                    uint32_t vp = resolve(w1);
                    uint32_t a = read32(rdram, vp), b = read32(rdram, vp + 4), c = read32(rdram, vp + 8), d = read32(rdram, vp + 12);
                    fprintf(stderr, "[dl]   viewport @%06X scale %d,%d trans %d,%d (dl 0x%06X)\n", vp,
                        (int16_t)(a >> 16), (int16_t)a, (int16_t)(c >> 16), (int16_t)c, cur_pc);
                    (void)b; (void)d;
                }
                break;
            case 0xFF:                                                                  // SETCIMG
                fprintf(stderr, "[dl]   color image 0x%06X width %u\n", resolve(w1), (w0 & 0xFFF) + 1);
                break;
            case 0xED:                                                                  // SETSCISSOR
                fprintf(stderr, "[dl]   scissor %u,%u - %u,%u\n", ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4, ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4);
                break;
            default: break;
        }
    }
done:
    fprintf(stderr, "[dl] task %d dl 0x%08X: %zu render states\n", cur, dl, stats.size());
    for (auto& [k, s] : stats) {
        auto [oml, omh, ch, cl, prim, envc, blend, geom] = k;
        fprintf(stderr, "[dl]   tris %5u  othermodeL %08X H %08X  combine %06X:%08X  prim %08X env %08X blend %08X geom %08X  (first at dl 0x%06X vtx 0x%06X)\n",
            s.tris, oml, omh, ch, cl, prim, envc, blend, geom, s.first_dl, s.first_vtx);
    }
}

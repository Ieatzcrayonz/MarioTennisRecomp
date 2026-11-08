// Display-list fix-ups applied to every graphics task just before the RSP gets it (same hook as the
// inspector in mt_dldump.cpp).
//
// Narrow 304x224 layout: the boot logos and the seg71 front-end (menus, player/character/court
// select) draw into the left 304 columns of the game's 384-wide framebuffer, so RT64 shows them left
// of centre. In any frame that uses that layout, everything belonging to it is moved right by 40 px
// ((384-304)/2) and the two side margins are cleared to black:
//   - scissors 0..304 (and 0..344 from the instruction patches) become 40..344;
//   - viewports centred at x=152 (trans.x 608) are re-centred at 192 (trans.x 768);
//   - texture/fill rectangles drawn under the narrow scissor are shifted by 40 px;
//   - a small display list clearing both margins is spliced in after the frame's first clear.
#include <cstdint>
#include <unordered_map>
#include <utility>
#include "recomp.h"

extern "C" void rt64_set_horizontal_crop(float left, float right);   // lib/rt64 (Mario Tennis patch)

namespace {
    constexpr uint32_t q(uint32_t px) { return px * 4; }          // 10.2 fixed point
    constexpr uint32_t shift = q(40);
    constexpr uint32_t margin_dl = 0x7FFC00;                       // top of 8 MiB RDRAM, unused by the game

    // Rectangles already shifted, keyed by RDRAM address: static display lists are replayed every
    // frame and must not be shifted again. Dynamic buffers get fresh words, which differ from these.
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> shifted;

    uint32_t rd(uint8_t* rdram, uint32_t phys) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF)));
    }
    void wr(uint8_t* rdram, uint32_t phys, uint32_t v) {
        MEM_W(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF))) = (int32_t)v;
    }
    uint16_t rd16(uint8_t* rdram, uint32_t phys) {
        return (uint16_t)MEM_HU(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF)));
    }
    void wr16(uint8_t* rdram, uint32_t phys, uint16_t v) {
        MEM_H(0, (gpr)(int32_t)(0x80000000u | (phys & 0x7FFFFF))) = (int16_t)v;
    }
    uint32_t add12(uint32_t word, int pos, uint32_t amount) {
        uint32_t v = (word >> pos) & 0xFFF;
        v = (v + amount > 0xFFF) ? 0xFFF : v + amount;
        return (word & ~(0xFFFu << pos)) | (v << pos);
    }

    // Walks a task's display list (following G_DL calls and segment changes) and calls fn(addr, w0, w1)
    // for every command. fn returns false to stop.
    template <typename Fn>
    void walk(uint8_t* rdram, uint32_t task, Fn&& fn) {
        uint32_t seg[16] = {};
        uint32_t stack[32]; int sp = 0;
        uint32_t pc = (uint32_t)MEM_W(0x30, (gpr)(int32_t)task) & 0x1FFFFFFF;
        auto resolve = [&](uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0x1FFFFFFF; };
        for (int n = 0; n < 400000; n++) {
            uint32_t w0 = rd(rdram, pc), w1 = rd(rdram, pc + 4);
            uint32_t cur = pc; pc += 8;
            uint8_t op = w0 >> 24;
            if (!fn(cur, w0, w1, resolve)) return;
            switch (op) {
                case 0xDB: if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x1FFFFFFF; break;
                case 0xDE:
                    if (((w0 >> 16) & 0xFF) == 0 && sp < 32) stack[sp++] = pc;
                    pc = resolve(w1);
                    break;
                case 0xDF: if (sp == 0) return; pc = stack[--sp]; break;
                case 0xE4: case 0xE5: pc += 16; break;                       // RDPHALF_1 / RDPHALF_2
                default: break;
            }
        }
    }

    bool is_narrow_scissor(uint32_t w0, uint32_t w1) {
        uint32_t ulx = (w0 >> 12) & 0xFFF, lrx = (w1 >> 12) & 0xFFF;
        return (ulx == 0 || ulx == shift) && (lrx == q(304) || lrx == q(344));
    }
}

void mt_prof_gfx_task();
void mt_widescreen_update();

extern "C" void mt_fixup_gfx_task(uint8_t* rdram, uint32_t task) {
    mt_prof_gfx_task();
    mt_widescreen_update();
    // Pass 1: does this frame use the narrow layout?
    bool narrow_frame = false;
    walk(rdram, task, [&](uint32_t, uint32_t w0, uint32_t w1, auto&) {
        if ((w0 >> 24) == 0xED && is_narrow_scissor(w0, w1)) { narrow_frame = true; return false; }
        return true;
    });
    // In Stretch mode the presenter shows only the narrow layout's columns (40..344 of 384), so
    // menus fill the window too; other frames show the whole picture. No effect in other modes.
    rt64_set_horizontal_crop(narrow_frame ? 40.0f / 384.0f : 0.0f, narrow_frame ? 344.0f / 384.0f : 1.0f);
    if (!narrow_frame) return;

    // Pass 2: move the layout and splice in the margin clear.
    bool narrow = false, spliced = false, after_first_fill = false, fb_set_after_fill = false;
    walk(rdram, task, [&](uint32_t cur, uint32_t w0, uint32_t w1, auto& resolve) {
        uint8_t op = w0 >> 24;
        switch (op) {
            case 0xED:                                                      // SETSCISSOR
                narrow = is_narrow_scissor(w0, w1);
                if (narrow) {
                    uint32_t n0 = (w0 & ~(0xFFFu << 12)) | (shift << 12);
                    uint32_t n1 = (w1 & ~(0xFFFu << 12)) | (q(344) << 12);
                    if (n0 != w0) wr(rdram, cur, n0);
                    if (n1 != w1) wr(rdram, cur + 4, n1);
                }
                break;
            case 0xDC:                                                      // MOVEMEM
                if ((w0 & 0xFF) == 8) {                                     // G_MV_VIEWPORT
                    uint32_t vp = resolve(w1);
                    if (rd16(rdram, vp + 8) == q(152)) wr16(rdram, vp + 8, q(192));   // trans.x
                }
                break;
            case 0xE4: case 0xE5: case 0xF6:                                // TEXRECT / FLIP / FILLRECT
                if (op == 0xF6 && !after_first_fill) after_first_fill = true;
                if (narrow) {
                    auto it = shifted.find(cur);
                    bool done = (it != shifted.end()) && it->second.first == w0 && it->second.second == w1;
                    if (!done) {
                        uint32_t n0 = add12(w0, 12, shift), n1 = add12(w1, 12, shift);
                        wr(rdram, cur, n0);
                        wr(rdram, cur + 4, n1);
                        shifted[cur] = { n0, n1 };
                    }
                }
                break;
            case 0xFF:                                                      // SETCIMG
                if (after_first_fill && !fb_set_after_fill) { fb_set_after_fill = true; return true; }
                break;
            default:
                break;
        }

        // Right after the frame's first clear and the switch to the colour image, replace the next
        // command with a call to the margin-clearing list (which re-runs that command and returns).
        // The cycle type is still FILL from the clear and the scissor is still full-width there.
        if (fb_set_after_fill && !spliced && op == 0xDE && w1 == (0x80000000u | margin_dl)) {
            spliced = true;                                                 // already spliced (static list)
        }
        if (fb_set_after_fill && !spliced && op != 0xFF && op != 0xDE && op != 0xDF && op != 0xE4 && op != 0xE5) {
            spliced = true;
            const uint32_t m = margin_dl;
            const uint32_t list[][2] = {
                { 0xF7000000, 0x00010001 },                                 // SETFILLCOLOR black (RGBA5551 x2)
                { 0xF6000000 | (q(39) << 12) | q(223), 0 },                 // FILLRECT   0,0 -  39,223
                { 0xF6000000 | (q(383) << 12) | q(223), (q(344) << 12) },   // FILLRECT 344,0 - 383,223
                { w0, w1 },                                                 // the replaced command
                { 0xDF000000, 0 },                                          // ENDDL
            };
            for (uint32_t i = 0; i < 5; i++) { wr(rdram, m + 8 * i, list[i][0]); wr(rdram, m + 8 * i + 4, list[i][1]); }
            wr(rdram, cur, 0xDE000000);                                     // G_DL push
            wr(rdram, cur + 4, 0x80000000u | m);
        }
        return true;
    });
}

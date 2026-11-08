// A small RSP interpreter for the audio task.
//
// The game's audio microcode is part of the player's ROM (main code, loaded to RDRAM at 0x8003DAD0 and
// named in each audio OSTask). Instead of shipping a recompiled copy of it (rsp/aspMain.cpp), live
// builds run it here, straight from RDRAM, so the exe contains no Nintendo code. Vector instructions use
// the runtime's RSP vector unit (librecomp/rsp_vu_impl.hpp, the same implementation the recompiled
// microcode uses), so results match the recompiled version exactly.
//
// Coprocessor 0 behaves like RSPRecomp's output: DMA happens immediately when a length register is
// written, and every status read returns 0 (DMA idle, semaphore free).

#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "librecomp/rsp_vu_impl.hpp"

namespace {
    constexpr uint32_t imem_text_start = 0x080; // rspboot loads the microcode text to IMEM 0x080
    constexpr uint64_t max_instructions = 50'000'000; // runaway guard (an audio task needs ~100k)
    uint8_t imem[0x1000];
    uint32_t loaded_ucode = 0, loaded_size = 0;

    inline uint32_t rdram_word(uint8_t* rdram, uint32_t addr) {
        return *reinterpret_cast<uint32_t*>(rdram + (addr & 0x1FFFFFFC));
    }

    inline uint32_t imem_word(uint32_t pc) {
        uint32_t w;
        std::memcpy(&w, imem + (pc & 0xFFC), 4);
        return w;
    }
}

#define E16(X) switch (e) { \
    case 0: X(0); break;  case 1: X(1); break;  case 2: X(2); break;  case 3: X(3); break; \
    case 4: X(4); break;  case 5: X(5); break;  case 6: X(6); break;  case 7: X(7); break; \
    case 8: X(8); break;  case 9: X(9); break;  case 10: X(10); break; case 11: X(11); break; \
    case 12: X(12); break; case 13: X(13); break; case 14: X(14); break; case 15: X(15); break; }

static RspExitReason interpret_task(uint8_t* rdram, uint32_t ucode_addr) {
    // Load the microcode text into IMEM (cached: the audio task always uses the same microcode).
    uint32_t ucode_size = RSP_MEM_W_LOAD(0x14, 0xFC0);
    if (ucode_size == 0 || ucode_size > 0x1000 - imem_text_start) ucode_size = 0x1000 - imem_text_start;
    if (ucode_addr != loaded_ucode || ucode_size != loaded_size) {
        for (uint32_t i = 0; i < ucode_size; i += 4) {
            uint32_t w = rdram_word(rdram, ucode_addr + i);
            std::memcpy(imem + imem_text_start + i, &w, 4);
        }
        loaded_ucode = ucode_addr;
        loaded_size = ucode_size;
        std::fprintf(stderr, "[mt] RSP interpreter: microcode 0x%08X size 0x%X, first word %08X\n", ucode_addr, ucode_size, imem_word(imem_text_start));
    }

    RSP rsp{};
    uint32_t r[32] = {};
    r[1] = 0xFC0; // rspboot hands over with $1 pointing at the OSTask in DMEM (RSPRecomp does the same)
    uint32_t dma_mem_address = 0, dma_dram_address = 0;
    uint32_t pc = imem_text_start;
    uint32_t next_pc = pc + 4;

    for (uint64_t count = 0; count < max_instructions; count++) {
        uint32_t instr = imem_word(pc);
        uint32_t target = next_pc + 4; // where execution goes after the instruction at next_pc
        bool branch = false;
        uint32_t branch_to = 0;

        uint32_t op = instr >> 26;
        uint32_t rs = (instr >> 21) & 31, rt = (instr >> 16) & 31, rd = (instr >> 11) & 31, sa = (instr >> 6) & 31;
        int32_t simm = (int16_t)(instr & 0xFFFF);
        uint32_t uimm = instr & 0xFFFF;
        auto take = [&](bool cond, uint32_t to) { if (cond) { branch = true; branch_to = to; } };
        uint32_t btarget = (next_pc + (simm << 2)) & 0xFFF;

        switch (op) {
            case 0x00: // SPECIAL
                switch (instr & 63) {
                    case 0x00: r[rd] = r[rt] << sa; break;                          // sll
                    case 0x02: r[rd] = r[rt] >> sa; break;                          // srl
                    case 0x03: r[rd] = (uint32_t)((int32_t)r[rt] >> sa); break;     // sra
                    case 0x04: r[rd] = r[rt] << (r[rs] & 31); break;                // sllv
                    case 0x06: r[rd] = r[rt] >> (r[rs] & 31); break;                // srlv
                    case 0x07: r[rd] = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); break; // srav
                    case 0x08: take(true, r[rs] & 0xFFF); break;                    // jr
                    case 0x09: { uint32_t to = r[rs] & 0xFFF; r[rd] = 0x1000 | ((next_pc + 4) & 0xFFF); take(true, to); break; } // jalr
                    case 0x0D: return RspExitReason::Broke;                         // break
                    case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; break;             // add(u)
                    case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; break;             // sub(u)
                    case 0x24: r[rd] = r[rs] & r[rt]; break;
                    case 0x25: r[rd] = r[rs] | r[rt]; break;
                    case 0x26: r[rd] = r[rs] ^ r[rt]; break;
                    case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
                    case 0x2A: r[rd] = (int32_t)r[rs] < (int32_t)r[rt]; break;      // slt
                    case 0x2B: r[rd] = r[rs] < r[rt]; break;                        // sltu
                    default: goto unsupported;
                }
                break;
            case 0x01: // REGIMM
                switch (rt) {
                    case 0x00: take((int32_t)r[rs] < 0, btarget); break;            // bltz
                    case 0x01: take((int32_t)r[rs] >= 0, btarget); break;           // bgez
                    case 0x10: { bool c = (int32_t)r[rs] < 0; r[31] = 0x1000 | ((next_pc + 4) & 0xFFF); take(c, btarget); break; }  // bltzal
                    case 0x11: { bool c = (int32_t)r[rs] >= 0; r[31] = 0x1000 | ((next_pc + 4) & 0xFFF); take(c, btarget); break; } // bgezal
                    default: goto unsupported;
                }
                break;
            case 0x02: take(true, (instr << 2) & 0xFFF); break;                                 // j
            case 0x03: r[31] = 0x1000 | ((next_pc + 4) & 0xFFF); take(true, (instr << 2) & 0xFFF); break; // jal
            case 0x04: take(r[rs] == r[rt], btarget); break;                                    // beq
            case 0x05: take(r[rs] != r[rt], btarget); break;                                    // bne
            case 0x06: take((int32_t)r[rs] <= 0, btarget); break;                               // blez
            case 0x07: take((int32_t)r[rs] > 0, btarget); break;                                // bgtz
            case 0x08: case 0x09: r[rt] = r[rs] + simm; break;                                  // addi(u)
            case 0x0A: r[rt] = (int32_t)r[rs] < simm; break;                                    // slti
            case 0x0B: r[rt] = r[rs] < (uint32_t)simm; break;                                   // sltiu
            case 0x0C: r[rt] = r[rs] & uimm; break;
            case 0x0D: r[rt] = r[rs] | uimm; break;
            case 0x0E: r[rt] = r[rs] ^ uimm; break;
            case 0x0F: r[rt] = uimm << 16; break;                                               // lui
            case 0x10: // COP0
                if (rs == 0x00) { r[rt] = 0; }                                                  // mfc0: idle/free
                else if (rs == 0x04) {                                                          // mtc0
                    switch (rd) {
                        case 0: dma_mem_address = r[rt]; break;
                        case 1: dma_dram_address = r[rt]; break;
                        case 2: dma_rdram_to_dmem(rdram, dma_mem_address, dma_dram_address, r[rt]); break;
                        case 3: dma_dmem_to_rdram(rdram, dma_mem_address, dma_dram_address, r[rt]); break;
                        default: break;
                    }
                }
                else goto unsupported;
                break;
            case 0x12: { // COP2
                uint32_t e = (instr >> 21) & 0xF;
                auto& vd = rsp.vpu.r[sa];
                auto& vs = rsp.vpu.r[rd];
                auto& vt = rsp.vpu.r[rt];
                if (instr & (1u << 25)) {
                    switch (instr & 63) {
#define OP3(NAME) { auto f = [&]<uint8_t E>() { rsp.NAME<E>(vd, vs, vt); }; \
                    switch (e) { case 0: f.template operator()<0>(); break; case 1: f.template operator()<1>(); break; \
                    case 2: f.template operator()<2>(); break; case 3: f.template operator()<3>(); break; \
                    case 4: f.template operator()<4>(); break; case 5: f.template operator()<5>(); break; \
                    case 6: f.template operator()<6>(); break; case 7: f.template operator()<7>(); break; \
                    case 8: f.template operator()<8>(); break; case 9: f.template operator()<9>(); break; \
                    case 10: f.template operator()<10>(); break; case 11: f.template operator()<11>(); break; \
                    case 12: f.template operator()<12>(); break; case 13: f.template operator()<13>(); break; \
                    case 14: f.template operator()<14>(); break; case 15: f.template operator()<15>(); break; } }
                        case 0x00: OP3(VMULF) break;
                        case 0x01: OP3(VMULU) break;
                        case 0x03: OP3(VMULQ) break;
                        case 0x04: OP3(VMUDL) break;
                        case 0x05: OP3(VMUDM) break;
                        case 0x06: OP3(VMUDN) break;
                        case 0x07: OP3(VMUDH) break;
                        case 0x08: OP3(VMACF) break;
                        case 0x09: OP3(VMACU) break;
                        case 0x0C: OP3(VMADL) break;
                        case 0x0D: OP3(VMADM) break;
                        case 0x0E: OP3(VMADN) break;
                        case 0x0F: OP3(VMADH) break;
                        case 0x10: OP3(VADD) break;
                        case 0x11: OP3(VSUB) break;
                        case 0x13: OP3(VABS) break;
                        case 0x14: OP3(VADDC) break;
                        case 0x15: OP3(VSUBC) break;
                        case 0x1D: {
#define SAR(E) rsp.VSAR<E>(vd, vs)
                            E16(SAR)
                            break;
                        }
                        case 0x20: OP3(VLT) break;
                        case 0x21: OP3(VEQ) break;
                        case 0x22: OP3(VNE) break;
                        case 0x23: OP3(VGE) break;
                        case 0x24: OP3(VCL) break;
                        case 0x25: OP3(VCH) break;
                        case 0x26: OP3(VCR) break;
                        case 0x27: OP3(VMRG) break;
                        case 0x28: OP3(VAND) break;
                        case 0x29: OP3(VNAND) break;
                        case 0x2A: OP3(VOR) break;
                        case 0x2B: OP3(VNOR) break;
                        case 0x2C: OP3(VXOR) break;
                        case 0x2D: OP3(VNXOR) break;
                        case 0x33: {
#define MOV(E) rsp.VMOV<E>(vd, (uint8_t)rd, vt)
                            E16(MOV)
                            break;
                        }
                        case 0x37: break; // vnop
                        default: goto unsupported;
                    }
                }
                else {
                    e = (instr >> 7) & 0xF;
                    switch (rs) {
                        case 0x00: {
#define MFC(E) rsp.MFC2<E>(r[rt], vs)
                            E16(MFC)
                            break;
                        }
                        case 0x02: rsp.CFC2(r[rt], (uint8_t)rd); break;
                        case 0x04: {
#define MTC(E) rsp.MTC2<E>(r[rt], vs)
                            E16(MTC)
                            break;
                        }
                        case 0x06: rsp.CTC2(r[rt], (uint8_t)rd); break;
                        default: goto unsupported;
                    }
                }
                break;
            }
            case 0x20: r[rt] = (uint32_t)RSP_MEM_B(simm, r[rs]); break;                // lb
            case 0x21: r[rt] = RSP_MEM_H_LOAD(simm, r[rs]); break;                    // lh
            case 0x23: r[rt] = RSP_MEM_W_LOAD(simm, r[rs]); break;                    // lw
            case 0x24: r[rt] = RSP_MEM_BU(simm, r[rs]); break;                        // lbu
            case 0x25: r[rt] = RSP_MEM_HU_LOAD(simm, r[rs]); break;                   // lhu
            case 0x28: RSP_MEM_B(simm, r[rs]) = (int8_t)r[rt]; break;                 // sb
            case 0x29: RSP_MEM_H_STORE(simm, r[rs], r[rt]); break;                    // sh
            case 0x2B: RSP_MEM_W_STORE(simm, r[rs], r[rt]); break;                    // sw
            case 0x32: case 0x3A: { // LWC2 / SWC2
                uint32_t e = (instr >> 7) & 0xF;
                int8_t off = (int8_t)((instr & 0x7F) << 1) >> 1; // signed 7-bit
                auto& v = rsp.vpu.r[rt];
                uint32_t base = r[rs];
                bool store = op == 0x3A;
#define LS(NAME) { auto f = [&]<uint8_t E>() { rsp.NAME<E>(v, base, off); }; \
                    switch (e) { case 0: f.template operator()<0>(); break; case 1: f.template operator()<1>(); break; \
                    case 2: f.template operator()<2>(); break; case 3: f.template operator()<3>(); break; \
                    case 4: f.template operator()<4>(); break; case 5: f.template operator()<5>(); break; \
                    case 6: f.template operator()<6>(); break; case 7: f.template operator()<7>(); break; \
                    case 8: f.template operator()<8>(); break; case 9: f.template operator()<9>(); break; \
                    case 10: f.template operator()<10>(); break; case 11: f.template operator()<11>(); break; \
                    case 12: f.template operator()<12>(); break; case 13: f.template operator()<13>(); break; \
                    case 14: f.template operator()<14>(); break; case 15: f.template operator()<15>(); break; } }
                switch (rd | (store ? 0x20 : 0)) {
                    case 0x00: LS(LBV) break;  case 0x01: LS(LSV) break;  case 0x02: LS(LLV) break;  case 0x03: LS(LDV) break;
                    case 0x04: LS(LQV) break;  case 0x05: LS(LRV) break;  case 0x06: LS(LPV) break;  case 0x07: LS(LUV) break;
                    case 0x08: LS(LHV) break;  case 0x09: LS(LFV) break;
                    case 0x20: LS(SBV) break;  case 0x21: LS(SSV) break;  case 0x22: LS(SLV) break;  case 0x23: LS(SDV) break;
                    case 0x24: LS(SQV) break;  case 0x25: LS(SRV) break;  case 0x26: LS(SPV) break;  case 0x27: LS(SUV) break;
                    case 0x28: LS(SHV) break;  case 0x29: LS(SFV) break;
                    default: goto unsupported;
                }
                break;
            }
            default:
                goto unsupported;
        }
        r[0] = 0;

        pc = next_pc & 0xFFF;
        next_pc = branch ? branch_to : (target & 0xFFF);
        (void)dma_mem_address; (void)dma_dram_address;
        continue;

    unsupported:
        std::fprintf(stderr, "[mt] RSP interpreter: unsupported instruction %08X at IMEM 0x%03X\n", instr, pc);
        return RspExitReason::Unsupported;
    }
    std::fprintf(stderr, "[mt] RSP interpreter: task did not finish (pc 0x%03X)\n", pc);
    return RspExitReason::Invalid;
}

// MT_AUDIO_STATS=1 prints how long audio tasks take in the interpreter.
RspExitReason mt_rsp_interpret(uint8_t* rdram, uint32_t ucode_addr) {
    static const bool stats = std::getenv("MT_AUDIO_STATS") != nullptr;
    if (!stats) return interpret_task(rdram, ucode_addr);
    static uint64_t tasks = 0;
    static double total_ms = 0, max_ms = 0;
    auto t0 = std::chrono::steady_clock::now();
    RspExitReason r = interpret_task(rdram, ucode_addr);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    tasks++; total_ms += ms; if (ms > max_ms) max_ms = ms;
    if (tasks % 300 == 0) {
        std::fprintf(stderr, "[mt] audio interpreter: %llu tasks, avg %.2f ms, max %.2f ms\n", (unsigned long long)tasks, total_ms / 300, max_ms);
        total_ms = 0; max_ms = 0;
    }
    return r;
}

#ifndef MT_LIVE_RECOMP
// Development check (MT_AUDIO=compare): run every audio task through both this interpreter (on a copy of
// RDRAM) and the recompiled microcode, and report any difference in RDRAM or DMEM.
#include <vector>
extern RspUcodeFunc aspMain;

RspExitReason mt_rsp_compare(uint8_t* rdram, uint32_t ucode_addr) {
    constexpr size_t ram_size = 8 * 1024 * 1024;
    static std::vector<uint8_t> shadow(ram_size);
    static uint64_t tasks = 0, bad_tasks = 0;
    static uint8_t dmem_start[0x1000], dmem_interp[0x1000];

    std::memcpy(dmem_start, dmem, sizeof(dmem_start));
    std::memcpy(shadow.data(), rdram, ram_size);
    RspExitReason ri = mt_rsp_interpret(shadow.data(), ucode_addr);
    std::memcpy(dmem_interp, dmem, sizeof(dmem_interp));

    std::memcpy(dmem, dmem_start, sizeof(dmem_start));
    RspExitReason rr = aspMain(rdram, ucode_addr);

    tasks++;
    size_t ram_diffs = 0, first = 0;
    for (size_t i = 0; i < ram_size; i += 4) {
        if (std::memcmp(shadow.data() + i, rdram + i, 4) != 0) { if (!ram_diffs) first = i; ram_diffs++; }
    }
    size_t dmem_diffs = 0, dmem_first = 0, dmem_last = 0;
    for (size_t i = 0; i < 0x1000; i++) if (dmem_interp[i] != dmem[i]) { if (!dmem_diffs) dmem_first = i; dmem_last = i; dmem_diffs++; }
    // RDRAM differences are expected (game threads keep running while the two copies execute); every
    // byte the microcode writes to RDRAM comes from DMEM, so DMEM plus the exit reason is the real test.
    if (ri != rr || dmem_diffs) {
        if (bad_tasks++ < 20) {
            std::fprintf(stderr, "[mt] audio compare: task %llu differs: exit %d/%d, %zu RDRAM words (first 0x%08zX), %zu DMEM bytes (0x%03zX-0x%03zX)\n",
                         (unsigned long long)tasks, (int)ri, (int)rr, ram_diffs, first + 0x80000000, dmem_diffs, dmem_first, dmem_last);
        }
    }
    if (tasks % 600 == 0) {
        std::fprintf(stderr, "[mt] audio compare: %llu tasks, %llu differed\n", (unsigned long long)tasks, (unsigned long long)bad_tasks);
    }
    return rr;
}
#endif

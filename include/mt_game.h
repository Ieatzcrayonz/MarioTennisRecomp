#ifndef __MT_GAME_H__
#define __MT_GAME_H__

#include <cstdint>
#include "recomp.h"

namespace mt {
    // Registers the recompiled code sections with librecomp.
    void register_overlays();
    // Runs after the runtime's 1 MiB boot copy: rebuilds RAM exactly as the cartridge boot stub
    // at 0x80300000 does (clear RAM, LZ-decompress main to 0x80031000) and sets up main's args.
    void on_init(uint8_t* rdram, recomp_context* ctx);
}

// Called from the hook in the game's segment loader (func_80035CC0) once a segment has been
// decompressed into RAM. Activates the matching recompiled overlay, if the segment is code.
extern "C" void mt_on_segment_loaded(uint8_t* rdram, uint32_t rom, uint32_t dest, uint32_t size);

#endif

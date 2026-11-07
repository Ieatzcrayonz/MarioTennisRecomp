// The inherited debug menu expects Zelda's warp/time hooks. Mario Tennis has neither, so the
// actions do nothing. The menu indexes the first area/scene unconditionally, so the list holds
// one placeholder entry rather than being empty.
#include "zelda_debug.h"

std::vector<zelda64::AreaWarps> zelda64::game_warps{
    { "Mario Tennis", { { 0, "(no warps)", { "(none)" } } } },
};

void zelda64::do_warp(int, int, int) {}
void zelda64::set_time(uint8_t, uint8_t, uint8_t) {}

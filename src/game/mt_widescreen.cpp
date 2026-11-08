// Widescreen culling: the game skips objects whose bounding sphere lies outside its 4:3 view
// (func_800346A0, the sphere-vs-frustum test on the camera block at 0x80062B88). With RT64's
// "Expand" aspect ratio the picture shows more on the left and right, so those objects popped in
// and out at the screen edges. Hooks in mt.us.toml scale the test's horizontal factor (camera
// block +0x80) by mt_cull_x_scale(): (4:3) / window aspect in Expand mode, 1 otherwise.
// Only the visibility test is changed; the camera block itself (also used to place sprites
// on screen) is left as the game wrote it.

#include <algorithm>
#include <atomic>

#include "ultramodern/config.hpp"
#include "recomp_ui.h"

namespace {
    std::atomic<float> cull_x_scale{1.0f};
}

// Called once per graphics task (mt_fixup_gfx_task), so the value follows window resizes and
// settings changes with at most a frame of delay.
void mt_widescreen_update() {
    float scale = 1.0f;
    if (ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Expand) {
        int width = 0, height = 0;
        recompui::get_window_size(width, height);
        if (width > 0 && height > 0) {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            scale = std::min(1.0f, (4.0f / 3.0f) / aspect);
        }
    }
    cull_x_scale.store(scale, std::memory_order_relaxed);
}

extern "C" float mt_cull_x_scale() {
    return cull_x_scale.load(std::memory_order_relaxed);
}

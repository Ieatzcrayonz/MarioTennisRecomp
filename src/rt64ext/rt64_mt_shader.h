//
// Mario Tennis recomp: RetroArch Slang shader presets (e.g. CRT-Royale) applied to the final picture through
// librashader (loaded at runtime from librashader.dll; the game runs normally without it).
//

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "rhi/rt64_render_interface.h"

namespace RT64 {
    // Sets the .slangp preset to apply (path relative to the working directory); empty turns the shader off.
    // Safe to call from any thread; takes effect on the next presented frame.
    void mtSetShaderPreset(const std::string &presetPath);

    struct MtShaderPass {
        MtShaderPass();
        ~MtShaderPass();

        // True when a preset is selected and has not failed to load.
        bool wanted();

        // Records the shader chain into commandList: src (SHADER_READ layout) -> dst (COLOR_WRITE layout).
        // Leaves dst in COLOR_WRITE. Returns false if nothing was recorded (caller should draw normally).
        bool apply(RenderDevice *device, RenderCommandList *commandList, RenderTexture *src, uint32_t srcWidth, uint32_t srcHeight,
            RenderTexture *dst, uint32_t dstWidth, uint32_t dstHeight);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
};

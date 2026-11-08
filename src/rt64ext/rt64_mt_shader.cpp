//
// Mario Tennis recomp: RetroArch Slang shader presets through librashader. See rt64_mt_shader.h.
// Compiled into the rt64 library (CMakeLists.txt) so it can reach the native D3D12 / Vulkan objects.
//

#include "rt64_mt_shader.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "vulkan/rt64_vulkan.h"
#ifdef _WIN32
#   include "d3d12/rt64_d3d12.h"
#   define LIBRA_RUNTIME_D3D12
#endif
#define LIBRA_RUNTIME_VULKAN
#include "librashader_ld.h"

namespace RT64 {
    namespace {
        std::mutex presetMutex;
        // MT_SHADER=<path to .slangp> overrides the menu setting (for testing).
        std::string requestedPreset = (std::getenv("MT_SHADER") != nullptr) ? std::getenv("MT_SHADER") : "";
        bool presetFromEnvironment = (std::getenv("MT_SHADER") != nullptr);

        libra_instance_t &libra() {
            static libra_instance_t instance = librashader_load_instance();
            static bool reported = false;
            if (!instance.instance_loaded && !reported) {
                reported = true;
                fprintf(stderr, "[mt] librashader.dll not found or incompatible; shaders are unavailable.\n");
            }
            return instance;
        }

        void logError(const char *what, libra_error_t error) {
            if (error == nullptr) return;
            char *message = nullptr;
            libra().error_write(error, &message);
            fprintf(stderr, "[mt] shader: %s failed: %s\n", what, message != nullptr ? message : "(unknown error)");
            if (message != nullptr) libra().error_free_string(&message);
            libra().error_free(&error);
        }
    }

    void mtSetShaderPreset(const std::string &presetPath) {
        std::lock_guard<std::mutex> lock(presetMutex);
        if (!presetFromEnvironment) {
            requestedPreset = presetPath;
        }
    }

    struct MtShaderPass::Impl {
        std::string loadedPreset;     // preset the current chain was built from
        std::string failedPreset;     // preset that failed to load (not retried until changed)
        RenderDevice *chainDevice = nullptr;
#   ifdef _WIN32
        libra_d3d12_filter_chain_t d3d12Chain = nullptr;
#   endif
        libra_vk_filter_chain_t vkChain = nullptr;
        size_t frameCount = 0;

        // Called only between frames: the present queue waits for each frame's command list to finish.
        void freeChain() {
#       ifdef _WIN32
            if (d3d12Chain != nullptr) libra().d3d12_filter_chain_free(&d3d12Chain);
            d3d12Chain = nullptr;
#       endif
            if (vkChain != nullptr) libra().vk_filter_chain_free(&vkChain);
            vkChain = nullptr;
            loadedPreset.clear();
            chainDevice = nullptr;
        }
    };

    MtShaderPass::MtShaderPass() : impl(std::make_unique<Impl>()) { }

    MtShaderPass::~MtShaderPass() {
        if (libra().instance_loaded) impl->freeChain();
    }

    bool MtShaderPass::wanted() {
        std::string preset;
        {
            std::lock_guard<std::mutex> lock(presetMutex);
            preset = requestedPreset;
        }

        if (preset.empty()) {
            if (!impl->loadedPreset.empty()) impl->freeChain();
            impl->failedPreset.clear();
            return false;
        }

        return libra().instance_loaded && (preset != impl->failedPreset);
    }

    bool MtShaderPass::apply(RenderDevice *device, RenderCommandList *commandList, RenderTexture *src, uint32_t srcWidth, uint32_t srcHeight,
        RenderTexture *dst, uint32_t dstWidth, uint32_t dstHeight)
    {
        std::string preset;
        {
            std::lock_guard<std::mutex> lock(presetMutex);
            preset = requestedPreset;
        }

        if (preset.empty() || !libra().instance_loaded || (preset == impl->failedPreset)) {
            return false;
        }

        // Diagnostics: MT_SHADER_TEST=2 skips librashader entirely, =1 builds the chain but never draws with it.
        static const int testMode = (std::getenv("MT_SHADER_TEST") != nullptr) ? std::atoi(std::getenv("MT_SHADER_TEST")) : 0;
        if (testMode == 2) {
            return false;
        }

#   ifdef _WIN32
        D3D12Device *d3d12Device = dynamic_cast<D3D12Device *>(device);
#   endif
        VulkanDevice *vulkanDevice = dynamic_cast<VulkanDevice *>(device);

        // (Re)build the chain when the preset or device changed. Creation is deferred: its uploads are recorded
        // into this frame's command list, so librashader never submits work on a queue of its own.
        if ((preset != impl->loadedPreset) || (device != impl->chainDevice)) {
            impl->freeChain();

            libra_shader_preset_t shaderPreset = nullptr;
            libra_error_t error = libra().preset_create(preset.c_str(), &shaderPreset);
            if (error != nullptr) {
                logError("loading preset", error);
                impl->failedPreset = preset;
                return false;
            }

#       ifdef _WIN32
            if (d3d12Device != nullptr) {
                D3D12CommandList *d3d12CommandList = static_cast<D3D12CommandList *>(commandList);
                filter_chain_d3d12_opt_t options = {};
                options.version = LIBRASHADER_CURRENT_VERSION;
                options.frames_in_flight = 3;
                error = libra().d3d12_filter_chain_create_deferred(&shaderPreset, d3d12Device->d3d, d3d12CommandList->d3d, &options, &impl->d3d12Chain);
            }
            else
#       endif
            if (vulkanDevice != nullptr) {
                VulkanCommandList *vulkanCommandList = static_cast<VulkanCommandList *>(commandList);
                vulkanCommandList->endActiveRenderPass();
                libra_device_vk_t vk = {};
                vk.physical_device = vulkanDevice->physicalDevice;
                vk.instance = vulkanDevice->renderInterface->instance;
                vk.device = vulkanDevice->vk;
                vk.queue = VK_NULL_HANDLE;
                vk.entry = vkGetInstanceProcAddr;
                filter_chain_vk_opt_t options = {};
                options.version = LIBRASHADER_CURRENT_VERSION;
                options.frames_in_flight = 3;
                options.use_dynamic_rendering = false;
                error = libra().vk_filter_chain_create_deferred(&shaderPreset, vk, vulkanCommandList->vk, &options, &impl->vkChain);
            }
            else {
                fprintf(stderr, "[mt] shader: unsupported graphics API.\n");
                libra().preset_free(&shaderPreset);
                impl->failedPreset = preset;
                return false;
            }

            if (shaderPreset != nullptr) libra().preset_free(&shaderPreset);
            if (error != nullptr) {
                logError("creating the filter chain", error);
                impl->freeChain();
                impl->failedPreset = preset;
                return false;
            }

            impl->loadedPreset = preset;
            impl->chainDevice = device;
            impl->frameCount = 0;
            fprintf(stderr, "[mt] shader: loaded %s\n", preset.c_str());
        }

        if (testMode == 1) {
            return false;
        }

        libra_viewport_t viewport = { 0.0f, 0.0f, dstWidth, dstHeight };
        libra_error_t error = nullptr;
#   ifdef _WIN32
        if (impl->d3d12Chain != nullptr) {
            D3D12CommandList *d3d12CommandList = static_cast<D3D12CommandList *>(commandList);
            libra_image_d3d12_t input = {};
            input.image_type = LIBRA_D3D12_IMAGE_TYPE_RESOURCE;
            input.handle.resource = static_cast<D3D12Texture *>(src)->d3d;
            libra_image_d3d12_t output = {};
            output.image_type = LIBRA_D3D12_IMAGE_TYPE_RESOURCE;
            output.handle.resource = static_cast<D3D12Texture *>(dst)->d3d;
            error = libra().d3d12_filter_chain_frame(&impl->d3d12Chain, d3d12CommandList->d3d, impl->frameCount, input, output, &viewport, nullptr, nullptr);

            // librashader bound its own heaps, root signatures and pipelines: make RT64 bind its own again.
            d3d12CommandList->descriptorHeapsSet = false;
            d3d12CommandList->activeComputePipelineLayout = nullptr;
            d3d12CommandList->activeGraphicsPipelineLayout = nullptr;
            d3d12CommandList->activeGraphicsPipeline = nullptr;
            d3d12CommandList->activeTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
            d3d12CommandList->targetFramebuffer = nullptr;
            d3d12CommandList->targetFramebufferSamplePositionsSet = false;
        }
        else
#   endif
        if (impl->vkChain != nullptr) {
            VulkanCommandList *vulkanCommandList = static_cast<VulkanCommandList *>(commandList);
            vulkanCommandList->endActiveRenderPass();
            VulkanTexture *vkSrc = static_cast<VulkanTexture *>(src);
            VulkanTexture *vkDst = static_cast<VulkanTexture *>(dst);
            libra_image_vk_t input = { vkSrc->vk, vkSrc->imageFormat, srcWidth, srcHeight };
            libra_image_vk_t output = { vkDst->vk, vkDst->imageFormat, dstWidth, dstHeight };
            error = libra().vk_filter_chain_frame(&impl->vkChain, vulkanCommandList->vk, impl->frameCount, input, output, &viewport, nullptr, nullptr);
            vulkanCommandList->targetFramebuffer = nullptr;
            vulkanCommandList->activeComputePipelineLayout = nullptr;
            vulkanCommandList->activeGraphicsPipelineLayout = nullptr;
            vulkanCommandList->activeRaytracingPipelineLayout = nullptr;
        }

        if (error != nullptr) {
            logError("drawing a frame", error);
            impl->failedPreset = preset;
            return false;
        }

        impl->frameCount++;
        return true;
    }
};

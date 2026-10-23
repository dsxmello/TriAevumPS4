// TriAevum PS4 - Phase 2: Vulkan (RADV-on-GNM) implementation of the common
// PICA render-backend contract (Fast::Renderer3ds::PicaRenderBackend, exposed
// as Oot3d::Renderer::PicaRenderBackend).
//
// Same plug point as the archived GLES2/Piglet backend (triaevum-ps4-port
// ports/ps4/src/ps4_gles_pica_backend.*), but without its workarounds: the
// canonical generated GLSL 4.50 (set=0 bindings 0-5/13, Vulkan depth range)
// is compiled to SPIR-V ON THE CONSOLE by a statically linked glslang (the
// DolphinPS4 model) and RADV's ACO turns it into GCN. No translation, no
// CPU vertex path, no corpus dependency: every program the title emits
// compiles exactly as it does on the desktop Vulkan product.
//
// v1 scope (first image first; semantics follow the desktop
// gfx_vulkan_pica*.cpp backend where they are cheap to follow):
//   * one queue, one submission per flush, synchronous fence wait (flushes
//     happen at EndFrame and before every guest materialization readback);
//   * images stay in VK_IMAGE_LAYOUT_GENERAL with full barriers between
//     render passes and transfers (gfx7 has no DCC to lose);
//   * colour targets are RGBA8, depth D24S8 (D32S8 fallback), native extent;
//   * textures decode on the CPU (DecodePicaTextureRgba8), base level only;
//   * draws the v1 path cannot express (shadow producer, gas, exclude
//     scissor) are SKIPPED and counted instead of failing the frame;
//   * raw-copy display transfers materialize the source target back to
//     guest RAM with the desktop's tiled encoder (EncodePicaFramebuffer);
//   * presentation draws into the host's VideoOut framebuffers, which are
//     linear VkImages owned by this backend (the VK probe recipe).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "oot3d/renderer/pica_render_backend.h"

namespace triaevum::ps4 {

struct VkBackendStats {
    uint64_t draws = 0;
    uint64_t drawFailures = 0;
    uint64_t drawsSkipped = 0;
    uint64_t skipNoSource = 0;
    uint64_t skipNoGeometry = 0;
    uint64_t skipFragOp = 0;
    uint64_t skipScissor = 0;
    uint64_t skipExtent = 0;
    uint64_t skipShader = 0;
    uint64_t skipPipeline = 0;
    uint64_t shadersCompiled = 0;
    uint64_t shadersFromDisk = 0;
    uint64_t shaderFailures = 0;
    uint64_t shaderCompileUs = 0;
    uint64_t pipelines = 0;
    uint64_t pipelineCompileUs = 0;
    uint64_t textures = 0;
    uint64_t textureUploadBytes = 0;
    uint64_t targets = 0;
    uint64_t transfers = 0;
    uint64_t transferFailures = 0;
    uint64_t presents = 0;
    uint64_t materializations = 0;
    uint64_t fills = 0;
    uint64_t clears = 0;
    uint64_t completions = 0;
    uint64_t submits = 0;
    uint64_t gpuWaitUs = 0;
    uint64_t vkErrors = 0;
    char lastError[160] = "";
};

class Ps4VkPicaRenderBackend final : public Oot3d::Renderer::PicaRenderBackend {
  public:
    using LogFn = void (*)(const char *text);

    // Brings up RADV (instance, device, queue family 0), the shared Vulkan
    // objects and the two linear scanout images. Returns nullptr (after
    // logging why) when any step fails; the host then keeps its CPU-only
    // presentation path.
    static std::unique_ptr<Ps4VkPicaRenderBackend> Create(LogFn log, uint32_t scanoutWidth,
                                                          uint32_t scanoutHeight);
    ~Ps4VkPicaRenderBackend() override;

    Ps4VkPicaRenderBackend(const Ps4VkPicaRenderBackend &) = delete;
    Ps4VkPicaRenderBackend &operator=(const Ps4VkPicaRenderBackend &) = delete;

    // PicaRenderBackend (required surface):
    bool SubmitPicaDraw(const Oot3d::Renderer::PicaDrawView &draw, std::string *error) override;
    bool SubmitPicaDisplayTransfer(const Oot3d::Renderer::PicaDisplayTransferView &transfer,
                                   std::string *error) override;
    bool ClearPicaRenderTarget(uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress,
                               std::string *error) override;
    bool SubmitPicaMemoryFill(const Oot3d::Renderer::PicaMemoryFillView &fill,
                              std::string *error) override;
    bool QueuePicaCompletion(uint64_t completionId, std::string *error) override;
    std::vector<uint64_t> TakePicaCompletions() override;
    bool ResetPicaState(std::string *error) override;

    // Scanout images (linear R8G8B8A8, CPU-mapped, 2 MiB aligned): the host
    // registers them with VideoOut and may write them with the CPU while no
    // GPU work targets them (status page, HUD strip).
    // Scanout buffers: the module host flips two, the product three
    // (rendering continues while a flip waits for vblank).
    static constexpr uint32_t kScanoutCount = 3;
    void *ScanoutPixels(uint32_t index) const;
    uint32_t ScanoutPitchPixels() const;

    // Frame bracket: presentation transfers drained between BeginFrame and
    // EndFrame draw into scanout image `scanoutIndex`. EndFrame submits the
    // recorded work, waits for it and reports whether a display image was
    // presented into the scanout image this frame.
    void BeginFrame(uint32_t scanoutIndex);
    bool EndFrame();

    // Persists the VkPipelineCache when pipelines were added since the last
    // save (cheap no-op otherwise).
    void SavePipelineCache();

    // Presentation settings (host menu). The render scale multiplies every
    // PICA render target (desktop render-scale semantics: viewports and
    // scissors scale, guest materialization resamples to the native extent);
    // a change applies at the next BeginFrame by recreating the targets.
    enum class ScreenLayout : uint8_t {
        TopOnly = 0,
        SideBySide = 1,
        BottomInCorner = 2,
    };
    struct PresentOptions {
        uint32_t renderScale = 4;
        ScreenLayout layout = ScreenLayout::TopOnly;
        bool linearFilter = true;
        bool stretch = false;
        // Output aspect (width / height) the top screen fills, the desktop's
        // swapchain-shaped top target (render_resolution_policy.h): above the
        // native 5:3 the scene renders wider (widescreen). 0 keeps 5:3.
        float outputAspect = 0.0F;
    };
    void SetPresentOptions(const PresentOptions &options);
    const PresentOptions &CurrentPresentOptions() const;

    // Product mode (ports/ps4-product): a presented transfer draws at once
    // into the scanout pass, fitted exactly like the TopScreen UI canvas
    // (FitUiPresentationViewport), so the HUD and ImGui recorded after it
    // land on top. The default (module host) composes at EndFrame instead.
    void SetImmediatePresentation(bool immediate);

    // Internal state for the product's rendering API, which records Fast3D
    // UI draws and ImGui into the same command buffer and scanout pass
    // (ps4_vk_pica_backend_impl.h). Not a stable interface.
    struct Impl;
    Impl *Internal() { return mImpl.get(); }

    const VkBackendStats &Stats() const;

  private:
    Ps4VkPicaRenderBackend();
    std::unique_ptr<Impl> mImpl;
};

}  // namespace triaevum::ps4

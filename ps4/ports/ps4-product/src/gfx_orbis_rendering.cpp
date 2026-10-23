// TriAevum PS4 product - Fast3D rendering API on RADV/GNM.
//
// Three parts share one command buffer and one scanout render pass (the
// Vulkan PICA backend's, ps4-vk-game/src/ps4_vk_pica_backend*):
//   * native PICA: forwarded to Ps4VkPicaRenderBackend (the renderer that runs
//     the title on the console since 0.92), in immediate-presentation mode so
//     the top screen lands in the scanout before anything drawn after it;
//   * Fast3D: the compatibility-combiner path at the scope the desktop Vulkan
//     backend implements (framebuffer 0 only) - what the TopScreen HUD
//     (oot3d::ui::Fast3dUiRenderBackend) and the preparation screen draw with;
//   * ImGui: imgui_impl_vulkan on the scanout pass (the F1 / L3+R3 window).
// Semantics follow gfx_vulkan.cpp / gfx_vulkan_resources.cpp /
// gfx_vulkan_shaders.cpp (vertex layout, combiner sources, draw uniforms,
// pipeline state) so the shared UI code sees the desktop contract.

#include "fast/backends/gfx_orbis.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/render_resolution_policy.h"
#include "fast/oot3d/renderer_presentation_controller.h"
#include "fast/renderer/compatibility_combiner.h"
#include "fast/renderer/compatibility_shader_sources.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ps4_display.h"
#include "ps4_vk_pica_backend.h"
#include "ps4_vk_pica_backend_impl.h"

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                              const char* name);

namespace Fast {
namespace {

constexpr uint32_t kTxMirror = 0x1; // G_TX_MIRROR
constexpr uint32_t kTxClamp = 0x2;  // G_TX_CLAMP
constexpr uint32_t kAbsent = UINT32_MAX;

using Backend = triaevum::ps4::Ps4VkPicaRenderBackend;
using Impl = Backend::Impl;

void BackendLog(const char* text) {
    std::printf("[triaevum] %s\n", text);
}

VkBlendFactor ToVk(GfxNativeBlendFactor factor) {
    switch (factor) {
        case GfxNativeBlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
        case GfxNativeBlendFactor::One: return VK_BLEND_FACTOR_ONE;
        case GfxNativeBlendFactor::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
        case GfxNativeBlendFactor::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GfxNativeBlendFactor::DestColor: return VK_BLEND_FACTOR_DST_COLOR;
        case GfxNativeBlendFactor::OneMinusDestColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case GfxNativeBlendFactor::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case GfxNativeBlendFactor::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GfxNativeBlendFactor::DestAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case GfxNativeBlendFactor::OneMinusDestAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GfxNativeBlendFactor::ConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case GfxNativeBlendFactor::OneMinusConstantColor:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case GfxNativeBlendFactor::ConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case GfxNativeBlendFactor::OneMinusConstantAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case GfxNativeBlendFactor::SourceAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp ToVk(GfxNativeBlendEquation equation) {
    switch (equation) {
        case GfxNativeBlendEquation::Add: return VK_BLEND_OP_ADD;
        case GfxNativeBlendEquation::Subtract: return VK_BLEND_OP_SUBTRACT;
        case GfxNativeBlendEquation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case GfxNativeBlendEquation::Min: return VK_BLEND_OP_MIN;
        case GfxNativeBlendEquation::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkSamplerAddressMode ToVk(GfxNativeTextureWrap wrap) {
    switch (wrap) {
        case GfxNativeTextureWrap::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case GfxNativeTextureWrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case GfxNativeTextureWrap::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

bool IsLinear(GfxNativeTextureFilter filter) {
    return filter == GfxNativeTextureFilter::Linear ||
           filter == GfxNativeTextureFilter::LinearMipmapNearest ||
           filter == GfxNativeTextureFilter::LinearMipmapLinear;
}

// The desktop VulkanShaderProgram, as BuildShaderProgram lays it out.
struct OrbisShaderProgram {
    uint64_t ShaderId0 = 0;
    uint64_t ShaderId1 = 0;
    VkShaderModule Vertex = VK_NULL_HANDLE;
    VkShaderModule Fragment = VK_NULL_HANDLE;
    uint8_t NumInputs = 0;
    bool UsedTextures[2] = {};
    bool UsesTexture2 = false;
    bool HasAlpha = false;
    bool HasFog = false;
    bool HasGrayscale = false;
    std::array<uint32_t, 3> TextureCoordinateOffsets{kAbsent, kAbsent, kAbsent};
    uint32_t FogOffset = kAbsent;
    uint32_t GrayscaleOffset = kAbsent;
    std::array<uint32_t, 8> InputOffsets{kAbsent, kAbsent, kAbsent, kAbsent,
                                         kAbsent, kAbsent, kAbsent, kAbsent};
    uint32_t ShadowCoordinateOffset = kAbsent;
    uint32_t VertexStride = 0;
};

// The desktop VulkanDrawUniforms (std430 DrawUniformState of the combiner).
struct alignas(16) DrawUniforms {
    std::array<int32_t, 4> TextureWidth{};
    std::array<int32_t, 4> TextureHeight{};
    std::array<int32_t, 4> TextureFiltering{};
    uint32_t FrameCount = 0;
    float NoiseScale = 1.0F;
    float PrimDepth = 0.0F;
    int32_t FogFlip = 0;
    int32_t AlphaTestEnabled = 0;
    int32_t AlphaTestFunction = 1;
    int32_t AlphaTestReference = 0;
    int32_t ShadowTextureBias = 0;
    int32_t ShadowOrthographic = 1;
    int32_t ShadowInvert = 0;
    int32_t Reserved = 0;
    std::array<uint32_t, 128> FogLut{};
};

struct TextureRecord {
    Impl::Image Image;
    bool Uploaded = false;
    GfxNativeSamplerState Sampler{};
    VkDescriptorSet ImGuiSet = VK_NULL_HANDLE;
};

class GfxRenderingAPIOrbis final : public GfxRenderingAPI, public ImGuiRenderHooks {
  public:
    // ---- lifecycle ------------------------------------------------------
    const char* GetName() override { return "PS4 Vulkan"; }
    int GetMaxTextureSize() override {
        return mVk != nullptr ? static_cast<int>(mVk->properties.limits.maxImageDimension2D) : 4096;
    }
    GfxClipParameters GetClipParameters() override { return {true, true}; }

    void Init() override {
        auto& display = triaevum::ps4::Ps4Display::Instance();
        mPica = Backend::Create(BackendLog, display.Width(), display.Height());
        if (mPica == nullptr) {
            throw std::runtime_error("PS4 Vulkan renderer initialization failed");
        }
        mPica->SetImmediatePresentation(true);
        mVk = mPica->Internal();
        void* buffers[triaevum::ps4::Ps4VkPicaRenderBackend::kScanoutCount];
        for (uint32_t index = 0; index < std::size(buffers); ++index)
            buffers[index] = mPica->ScanoutPixels(index);
        display.Register(buffers, std::size(buffers), display.Width(), display.Height(),
                         mPica->ScanoutPitchPixels());
        // The window session hands the guest-memory callbacks to this object;
        // the PICA backend reaches them through it at call time.
        mPica->SetPicaPhysicalMemoryAccess(
            [this](uint32_t address, std::span<uint8_t> out) {
                return mPhysicalMemoryRead && mPhysicalMemoryRead(address, out);
            },
            [this](std::span<const Renderer3ds::PicaPhysicalMemoryWrite> writes) {
                return mPhysicalMemoryCommit && mPhysicalMemoryCommit(writes);
            },
            nullptr, nullptr, nullptr);
        CreateFast3dObjects();
        mInitialized = true;
    }
    void Shutdown() override {
        if (mPica != nullptr) mPica->SavePipelineCache();
    }
    void OnResize() override {}

    // ---- frame ----------------------------------------------------------
    void StartFrame() override {
        if (!mInitialized) return;
        auto& display = triaevum::ps4::Ps4Display::Instance();
        const auto flipWaitStart = std::chrono::steady_clock::now();
        display.WaitPendingFlip();
        mPerf.flipWait += std::chrono::steady_clock::now() - flipWaitStart;
        mPica->BeginFrame(display.NextBuffer());
        mFrameActive = true;
        ++mDrawUniforms.FrameCount;
        ApplyPresentationSettings();
    }
    bool HasActiveFrame() const override { return mInitialized; }
    void EndFrame() override {
        if (!mInitialized || !mFrameActive) return;
        mPica->EndFrame();
        mFrameActive = false;
        // Images replaced or deleted this frame are idle after the flush.
        for (auto& image : mRetiredImages) mVk->DestroyImage(image);
        mRetiredImages.clear();
        ReportPerformance();
        if (++mFramesSinceCacheSave >= 600U) {
            mFramesSinceCacheSave = 0;
            mPica->SavePipelineCache();
        }
    }
    void FinishRender() override {}
    void StartDrawToFramebuffer(int fbId, float noiseScale) override {
        mCurrentFramebuffer = fbId;
        mDrawUniforms.NoiseScale = noiseScale;
    }
    void ClearFramebuffer(bool color, bool depth) override {
        (void)depth;  // the scanout pass carries no depth attachment
        if (!mFrameActive || mCurrentFramebuffer != 0 || !color) return;
        mVk->OpenScanoutPass();
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        std::memcpy(clear.clearValue.color.float32, mClearColor, sizeof mClearColor);
        const VkClearRect rect = {{{0, 0}, {mVk->scanoutWidth, mVk->scanoutHeight}}, 0, 1};
        mVk->vk.vkCmdClearAttachments(mVk->mainCb, 1, &clear, 1, &rect);
    }

    // Offscreen Fast3D framebuffers are not drawn (desktop Vulkan scope).
    int CreateFramebuffer() override { return mNextFramebuffer++; }
    void UpdateFramebufferParameters(int, uint32_t, uint32_t, uint32_t, bool, bool, bool,
                                     bool) override {}
    bool UpdateFramebufferParametersWithColorFormat(int fbId, uint32_t, uint32_t, uint32_t, bool,
                                                    bool, bool, bool,
                                                    GfxFramebufferColorFormat format) override {
        return fbId == 0 && format == GfxFramebufferColorFormat::Rgba8;
    }
    void CopyFramebuffer(int, int, int, int, int, int, int, int, int, int) override {}
    void ReadFramebufferToCPU(int, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override {
        if (rgba16Buf != nullptr) std::fill_n(rgba16Buf, size_t{width} * height, uint16_t{0});
    }
    void ResolveMSAAColorBuffer(int, int) override {}
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int, const std::set<std::pair<float, float>>& coordinates) override {
        std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> result;
        for (const auto& coordinate : coordinates) result.emplace(coordinate, uint16_t{0});
        return result;
    }
    void* GetFramebufferTextureId(int) override { return nullptr; }
    void SelectTextureFb(int) override {}

    // ---- shaders --------------------------------------------------------
    void UnloadShader(ShaderProgram*) override { mCurrentShader = nullptr; }
    void LoadShader(ShaderProgram* program) override {
        mCurrentShader = reinterpret_cast<OrbisShaderProgram*>(program);
    }
    void ClearShaderCache() override {
        mCurrentShader = nullptr;  // modules/pipelines stay (pointers are handed out)
    }
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override {
        auto& shader = mShaders[{shaderId0, shaderId1}];
        if (shader.Vertex == VK_NULL_HANDLE) BuildShader(shader, shaderId0, shaderId1);
        mCurrentShader = &shader;
        return reinterpret_cast<ShaderProgram*>(&shader);
    }
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override {
        const auto found = mShaders.find({shaderId0, shaderId1});
        return found == mShaders.end() ? nullptr : reinterpret_cast<ShaderProgram*>(&found->second);
    }
    void ShaderGetInfo(ShaderProgram* program, uint8_t* numInputs, bool usedTextures[2]) override {
        const auto* shader = reinterpret_cast<const OrbisShaderProgram*>(program);
        *numInputs = shader != nullptr ? shader->NumInputs : 0;
        usedTextures[0] = shader != nullptr && shader->UsedTextures[0];
        usedTextures[1] = shader != nullptr && shader->UsedTextures[1];
    }

    // ---- textures -------------------------------------------------------
    uint32_t NewTexture() override {
        const uint32_t id = mNextTexture++;
        mTextures.emplace(id, TextureRecord{});
        return id;
    }
    void SelectTexture(int tile, uint32_t textureId) override {
        if (tile < 0 || tile >= 3) return;
        mCurrentUnit = static_cast<uint32_t>(tile);
        mSelected[mCurrentUnit] = textureId;
    }
    void UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height) override {
        if (!mInitialized || rgba == nullptr || width == 0 || height == 0) return;
        const auto found = mTextures.find(mSelected[mCurrentUnit]);
        if (found == mTextures.end()) return;
        auto& texture = found->second;
        const VkDeviceSize bytes = VkDeviceSize{width} * height * 4U;
        if (!mVk->RingReserve(bytes + 16U)) return;
        // A re-upload gets a fresh image: draws recorded earlier this frame
        // still sample the old one (uploads run ahead of the frame's work).
        if (texture.Image.image != VK_NULL_HANDLE) RetireTexture(texture);
        if (!mVk->CreateImage(texture.Image, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, false)) {
            return;
        }
        const VkDeviceSize offset = mVk->RingAlloc(bytes, 16U);
        std::memcpy(mVk->ringPointer + offset, rgba, bytes);
        mVk->ToGeneral(mVk->uploadCb, texture.Image);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        mVk->vk.vkCmdCopyBufferToImage(mVk->uploadCb, mVk->ring, texture.Image.image,
                                       VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        mVk->uploadsRecorded = true;
        texture.Uploaded = true;
    }
    void SetSamplerParameters(int sampler, bool linear, uint32_t cms, uint32_t cmt) override {
        if (sampler < 0 || sampler >= 3) return;
        const auto found = mTextures.find(mSelected[static_cast<size_t>(sampler)]);
        if (found == mTextures.end()) return;
        auto& state = found->second.Sampler;
        state.MinFilter = linear ? GfxNativeTextureFilter::Linear : GfxNativeTextureFilter::Nearest;
        state.MagFilter = state.MinFilter;
        const auto wrap = [](uint32_t mode) {
            return (mode & kTxClamp) != 0 ? GfxNativeTextureWrap::ClampToEdge
                   : (mode & kTxMirror) != 0 ? GfxNativeTextureWrap::MirroredRepeat
                                             : GfxNativeTextureWrap::Repeat;
        };
        state.WrapS = wrap(cms);
        state.WrapT = wrap(cmt);
    }
    bool SetNativeSamplerParameters(int sampler, const GfxNativeSamplerState& state) override {
        if (sampler < 0 || sampler >= 3) return false;
        const auto found = mTextures.find(mSelected[static_cast<size_t>(sampler)]);
        if (found == mTextures.end()) return false;
        found->second.Sampler = state;
        return true;
    }
    void DeleteTexture(uint32_t textureId) override {
        const auto found = mTextures.find(textureId);
        if (found == mTextures.end()) return;
        RetireTexture(found->second);
        mTextures.erase(found);
    }
    void SetTextureFilter(FilteringMode mode) override { mFilterMode = mode; }
    FilteringMode GetTextureFilter() override { return mFilterMode; }
    void SetSrgbMode() override { mSrgbMode = true; }
    ImTextureID GetTextureById(int id) override {
        const auto found = mTextures.find(static_cast<uint32_t>(id));
        if (found == mTextures.end() || !found->second.Uploaded || !mImGuiReady) {
            return static_cast<ImTextureID>(0);
        }
        auto& texture = found->second;
        if (texture.ImGuiSet == VK_NULL_HANDLE) {
            texture.ImGuiSet = ImGui_ImplVulkan_AddTexture(SamplerFor(texture.Sampler),
                                                           texture.Image.view,
                                                           VK_IMAGE_LAYOUT_GENERAL);
        }
        return (ImTextureID)(uintptr_t)texture.ImGuiSet;
    }

    // ---- state ----------------------------------------------------------
    void SetDepthTestAndMask(bool depthTest, bool zUpd) override {
        mCurrentDepthTest = depthTest;
        mCurrentDepthMask = zUpd;
    }
    void SetZmodeDecal(bool decal) override { mCurrentZmodeDecal = decal; }
    void SetViewport(int x, int y, int width, int height) override {
        mViewport = {static_cast<float>(x),     static_cast<float>(y),
                     static_cast<float>(width), static_cast<float>(height),
                     0.0F,                      1.0F};
    }
    void SetScissor(int x, int y, int width, int height) override {
        mScissor = {{x, y}, {static_cast<uint32_t>(std::max(0, width)),
                             static_cast<uint32_t>(std::max(0, height))}};
    }
    void SetUseAlpha(bool useAlpha) override {
        mBlend = {};
        mBlend.Enabled = useAlpha;
        if (useAlpha) {
            mBlend.SourceRgb = GfxNativeBlendFactor::SourceAlpha;
            mBlend.DestRgb = GfxNativeBlendFactor::OneMinusSourceAlpha;
            mBlend.SourceAlpha = GfxNativeBlendFactor::SourceAlpha;
            mBlend.DestAlpha = GfxNativeBlendFactor::OneMinusSourceAlpha;
        }
    }
    bool SetNativeBlendState(const GfxNativeBlendState& state) override {
        mBlend = state;
        return true;
    }
    bool SetNativeCullMode(GfxNativeCullMode mode) override {
        mCullMode = mode;
        return true;
    }
    bool SetOot3dNativeTransform(const float* rowMajor) override {
        if (rowMajor == nullptr) return false;
        for (size_t row = 0; row < 4; ++row)
            for (size_t column = 0; column < 4; ++column)
                mTransform[column * 4 + row] = rowMajor[row * 4 + column];
        return true;
    }
    bool SetOot3dPicaAlphaTestShaderParameters(bool enabled, uint32_t function,
                                               uint8_t reference) override {
        mDrawUniforms.AlphaTestEnabled = enabled ? 1 : 0;
        mDrawUniforms.AlphaTestFunction = static_cast<int32_t>(function);
        mDrawUniforms.AlphaTestReference = reference;
        return true;
    }
    void SetCurrentPrimDepth(float depth) override { mDrawUniforms.PrimDepth = depth; }

    void DrawTriangles(float vbo[], size_t vboLength, size_t triangleCount) override;

    // ---- native PICA (forwarded) ----------------------------------------
    bool SubmitPicaDraw(const GfxNativePicaDrawView& draw, std::string* error) override {
        return mPica != nullptr && mPica->SubmitPicaDraw(draw, error);
    }
    bool SubmitPicaDisplayTransfer(const GfxNativePicaDisplayTransferView& transfer,
                                   std::string* error) override {
        return mPica != nullptr && mPica->SubmitPicaDisplayTransfer(transfer, error);
    }
    bool ClearPicaRenderTarget(uint64_t ns, uint32_t address, std::string* error) override {
        return mPica != nullptr && mPica->ClearPicaRenderTarget(ns, address, error);
    }
    bool SubmitPicaMemoryFill(const GfxNativePicaMemoryFillView& fill,
                              std::string* error) override {
        return mPica != nullptr && mPica->SubmitPicaMemoryFill(fill, error);
    }
    bool QueuePicaCompletion(uint64_t completionId, std::string* error) override {
        return mPica != nullptr && mPica->QueuePicaCompletion(completionId, error);
    }
    std::vector<uint64_t> TakePicaCompletions() override {
        return mPica != nullptr ? mPica->TakePicaCompletions() : std::vector<uint64_t>{};
    }
    bool ResetPicaState(std::string* error) override {
        return mPica == nullptr || mPica->ResetPicaState(error);
    }
    // Raw-copy versions are materialized to guest RAM at once; nothing is
    // retained for replay, so pinning has nothing to keep alive.
    bool SetPinnedPicaTextureCopies(std::span<const Renderer3ds::PicaTextureCopyReference>,
                                    std::string*) override {
        return true;
    }

    // ---- ImGui ----------------------------------------------------------
    bool InitImGuiBackend() override;
    void ShutdownImGuiBackend() override {
        if (!mImGuiReady) return;
        ImGui_ImplVulkan_Shutdown();
        mImGuiReady = false;
    }
    bool ImGuiBackendReady() const override { return mImGuiReady; }
    void NewImGuiFrame() override {
        if (InitImGuiBackend()) ImGui_ImplVulkan_NewFrame();
        // ImGui::NewFrame dereferences the default font's atlas: without a
        // built atlas (backend not up yet) it faults instead of drawing.
        ImFontAtlas* fonts = ImGui::GetIO().Fonts;
        if (!fonts->IsBuilt()) {
            std::printf("[triaevum] imgui: font atlas built without the Vulkan backend (ready=%d)\n",
                        mImGuiReady ? 1 : 0);
            fonts->Build();
        }
    }
    void RenderImGuiDrawData(ImDrawData* drawData) override {
        if (!mImGuiReady || drawData == nullptr || !mFrameActive || drawData->CmdListsCount == 0)
            return;
        mVk->OpenScanoutPass();
        ImGui_ImplVulkan_RenderDrawData(drawData, mVk->mainCb);
    }

  private:
    void CreateFast3dObjects();
    void BuildShader(OrbisShaderProgram& shader, uint64_t shaderId0, uint64_t shaderId1);
    VkPipeline Pipeline(const OrbisShaderProgram& shader);
    VkSampler SamplerFor(const GfxNativeSamplerState& state);
    void RetireTexture(TextureRecord& texture) {
        if (texture.ImGuiSet != VK_NULL_HANDLE && mImGuiReady) {
            ImGui_ImplVulkan_RemoveTexture(texture.ImGuiSet);
        }
        texture.ImGuiSet = VK_NULL_HANDLE;
        if (texture.Image.image != VK_NULL_HANDLE) mRetiredImages.push_back(texture.Image);
        texture.Image = Impl::Image{};
        texture.Uploaded = false;
    }

    std::unique_ptr<Backend> mPica;
    Impl* mVk = nullptr;
    bool mInitialized = false;
    bool mFrameActive = false;
    bool mImGuiReady = false;
    uint32_t mFramesSinceCacheSave = 0;

    // Presentation counters, logged every five seconds: frame rate, and per
    // frame the backend's draws, GPU submissions and the time spent waiting
    // for the GPU and for the previous flip.
    struct PerfWindow {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration flipWait{};
        uint32_t frames = 0;
        uint64_t flips = 0;
        triaevum::ps4::VkBackendStats stats{};
    } mPerf;
    // The settings runtime runs display changes (window mode, resolution,
    // vsync) as a transaction the renderer acknowledges, and saves nothing
    // while one is open (gfx_vulkan_frame.cpp on the desktop). The PS4 always
    // scans out 1920x1080 through VideoOut, so every request applies as is,
    // and a candidate needs no confirmation: the picture does not change.
    void ApplyPresentationSettings() {
        auto& runtime = Oot3d::GraphicsSettingsRuntime::Instance();
        ApplyRenderResolution(runtime);
        runtime.TickPresentation();
        const auto settings = runtime.Snapshot();
        const auto request = Oot3d::ResolveRendererPresentationRequest(
            settings, runtime.PresentationStatus(), mPresentationSettingsApplied);
        if (!request.has_value()) return;
        mPresentationSettingsApplied = true;
        if (!request->RequiresAcknowledgement()) return;
        if (!runtime.AcknowledgePresentationApplied(settings)) {
            std::printf("[triaevum] display settings: acknowledgement refused\n");
            return;
        }
        if (request->Kind == Oot3d::RendererPresentationRequestKind::Candidate) {
            runtime.ConfirmPresentation();
        }
    }
    bool mPresentationSettingsApplied = false;

    // Resolution and Internal Render Scale (F1): the PS4 always scans out
    // 1920x1080, so the chosen resolution sets how finely the 3DS screen is
    // rendered - the desktop's top target follows the output height x the
    // internal scale (render_resolution_policy.h); here that is a whole
    // multiple of the native 240 lines, 1x-6x. The top screen fills the
    // scanout's 16:9, as the window size the projection widens for.
    void ApplyRenderResolution(Oot3d::GraphicsSettingsRuntime& runtime) {
        const auto rendering = runtime.SnapshotForRendering();
        if (rendering.Revision == mRenderResolutionRevision) return;
        mRenderResolutionRevision = rendering.Revision;
        const auto& settings = rendering.Value;
        const float lines = static_cast<float>(std::max<uint32_t>(settings.OutputHeight, 240U)) *
                            Oot3d::ResolveInternalResolutionScale(settings);
        auto options = mPica->CurrentPresentOptions();
        options.renderScale = static_cast<uint32_t>(
            std::clamp<long>(std::lround(lines / 240.0F), 1L, 6L));
        const auto& display = triaevum::ps4::Ps4Display::Instance();
        options.outputAspect =
            static_cast<float>(display.Width()) / static_cast<float>(display.Height());
        mPica->SetPresentOptions(options);
        Oot3d::GraphicsDisplayMetrics metrics;
        metrics.OutputWidth = display.Width();
        metrics.OutputHeight = display.Height();
        metrics.InternalScale = Oot3d::ResolveInternalResolutionScale(settings);
        metrics.Window = settings.Window;
        runtime.PublishDisplayMetrics(metrics);
    }
    uint64_t mRenderResolutionRevision = 0;

    void ReportPerformance() {
        ++mPerf.frames;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - mPerf.start).count();
        if (seconds < 5.0) return;
        const auto& stats = mPica->Stats();
        const double frames = mPerf.frames;
        auto perFrame = [&](uint64_t current, uint64_t previous) {
            return static_cast<double>(current - previous) / frames;
        };
        const uint64_t flips = triaevum::ps4::Ps4Display::Instance().Flips();
        std::printf("[triaevum] perf: %.1f frames/s, %.1f flips/s, %.1f game presents/s | per frame:"
                    " %.0f draws, %.1f submits, gpu wait %.1f ms, flip wait %.1f ms,"
                    " %.1f materializations, %.0f KiB textures, pipelines %.1f ms (%llu total)\n",
                    frames / seconds, static_cast<double>(flips - mPerf.flips) / seconds,
                    static_cast<double>(stats.presents - mPerf.stats.presents) / seconds,
                    perFrame(stats.draws, mPerf.stats.draws),
                    perFrame(stats.submits, mPerf.stats.submits),
                    perFrame(stats.gpuWaitUs, mPerf.stats.gpuWaitUs) / 1000.0,
                    std::chrono::duration<double, std::milli>(mPerf.flipWait).count() / frames,
                    perFrame(stats.materializations, mPerf.stats.materializations),
                    perFrame(stats.textureUploadBytes, mPerf.stats.textureUploadBytes) / 1024.0,
                    perFrame(stats.pipelineCompileUs, mPerf.stats.pipelineCompileUs) / 1000.0,
                    static_cast<unsigned long long>(stats.pipelines));
        mPerf.start = now;
        mPerf.flipWait = {};
        mPerf.frames = 0;
        mPerf.stats = stats;
        mPerf.flips = flips;
    }
    int mCurrentFramebuffer = 0;
    int mNextFramebuffer = 1;

    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
    std::map<std::pair<uint64_t, uint64_t>, OrbisShaderProgram> mShaders;
    OrbisShaderProgram* mCurrentShader = nullptr;
    std::map<std::string, VkPipeline> mPipelines;
    std::map<std::string, VkSampler> mSamplers;

    std::unordered_map<uint32_t, TextureRecord> mTextures;
    uint32_t mNextTexture = 1;
    std::array<uint32_t, 3> mSelected{};
    uint32_t mCurrentUnit = 0;
    std::vector<Impl::Image> mRetiredImages;

    VkViewport mViewport{0.0F, 0.0F, 1920.0F, 1080.0F, 0.0F, 1.0F};
    VkRect2D mScissor{{0, 0}, {1920, 1080}};
    GfxNativeBlendState mBlend{};
    GfxNativeCullMode mCullMode = GfxNativeCullMode::KeepAll;
    std::array<float, 16> mTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    DrawUniforms mDrawUniforms{};
    FilteringMode mFilterMode = FILTER_LINEAR;
};

void GfxRenderingAPIOrbis::CreateFast3dObjects() {
    const VkShaderStageFlags fragment = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = static_cast<uint32_t>(std::size(bindings));
    setInfo.pBindings = bindings;
    mVk->Check(mVk->vk.vkCreateDescriptorSetLayout(mVk->device, &setInfo, nullptr, &mSetLayout),
               "vkCreateDescriptorSetLayout(fast3d)");
    const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &mSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    mVk->Check(mVk->vk.vkCreatePipelineLayout(mVk->device, &layoutInfo, nullptr, &mPipelineLayout),
               "vkCreatePipelineLayout(fast3d)");
}

// gfx_vulkan_shaders.cpp BuildShaderProgram: interleaved float layout.
void GfxRenderingAPIOrbis::BuildShader(OrbisShaderProgram& shader, uint64_t shaderId0,
                                       uint64_t shaderId1) {
    CCFeatures features{};
    gfx_cc_get_features(shaderId0, shaderId1, &features);
    shader.ShaderId0 = shaderId0;
    shader.ShaderId1 = shaderId1;
    shader.NumInputs = static_cast<uint8_t>(features.numInputs);
    shader.UsedTextures[0] = features.usedTextures[0];
    shader.UsedTextures[1] = features.usedTextures[1];
    shader.UsesTexture2 = Renderer::IsOot3dPicaTexture2Shader(features.shader_id);
    shader.HasAlpha = features.opt_alpha;
    shader.HasFog = features.opt_fog;
    shader.HasGrayscale = features.opt_grayscale;
    uint32_t floatOffset = 4;
    for (size_t unit = 0; unit < 2; ++unit) {
        if (!features.usedTextures[unit]) continue;
        shader.TextureCoordinateOffsets[unit] = floatOffset * sizeof(float);
        floatOffset += 2;
        for (size_t axis = 0; axis < 2; ++axis)
            if (features.clamp[unit][axis]) ++floatOffset;
    }
    if (shader.UsesTexture2) {
        shader.TextureCoordinateOffsets[2] = floatOffset * sizeof(float);
        floatOffset += 2;
    }
    if (features.opt_fog) {
        shader.FogOffset = floatOffset * sizeof(float);
        floatOffset += 4;
    }
    if (features.opt_grayscale) {
        shader.GrayscaleOffset = floatOffset * sizeof(float);
        floatOffset += 4;
    }
    const uint32_t components = features.opt_alpha ? 4 : 3;
    for (int input = 0; input < features.numInputs; ++input) {
        if (static_cast<size_t>(input) < shader.InputOffsets.size())
            shader.InputOffsets[static_cast<size_t>(input)] = floatOffset * sizeof(float);
        floatOffset += components;
    }
    if (Renderer::IsOot3dShadow2dShader(features.shader_id)) {
        shader.ShadowCoordinateOffset = floatOffset * sizeof(float);
        floatOffset += 3;
    }
    shader.VertexStride = floatOffset * sizeof(float);
    std::string log;
    if (!mVk->BuildModule(Renderer::BuildCompatibilityVertexShader(features), true,
                          &shader.Vertex, &log) ||
        !mVk->BuildModule(Renderer::BuildCompatibilityFragmentShader(features, mSrgbMode), false,
                          &shader.Fragment, &log)) {
        mVk->NoteError("fast3d shader %llx/%llx: %s", (unsigned long long)shaderId0,
                       (unsigned long long)shaderId1, log.c_str());
    }
}

VkSampler GfxRenderingAPIOrbis::SamplerFor(const GfxNativeSamplerState& state) {
    const bool linearMin = IsLinear(state.MinFilter) || mFilterMode != FILTER_NONE;
    const bool linearMag = IsLinear(state.MagFilter);
    std::string key;
    key.push_back(static_cast<char>(linearMin));
    key.push_back(static_cast<char>(linearMag));
    key.push_back(static_cast<char>(state.WrapS));
    key.push_back(static_cast<char>(state.WrapT));
    const auto found = mSamplers.find(key);
    if (found != mSamplers.end()) return found->second;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = linearMag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = IsLinear(state.MinFilter) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = ToVk(state.WrapS);
    info.addressModeV = ToVk(state.WrapT);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0F;
    VkSampler sampler = mVk->nearestClamp;
    mVk->Check(mVk->vk.vkCreateSampler(mVk->device, &info, nullptr, &sampler),
               "vkCreateSampler(fast3d)");
    mSamplers.emplace(key, sampler);
    return sampler;
}

// gfx_vulkan_resources.cpp GetOrCreatePipeline, on the depthless scanout pass.
VkPipeline GfxRenderingAPIOrbis::Pipeline(const OrbisShaderProgram& shader) {
    struct Key {
        uint64_t Id0, Id1;
        uint8_t Cull, Blend, EqRgb, EqAlpha, SrcRgb, DstRgb, SrcAlpha, DstAlpha;
    } key;
    std::memset(&key, 0, sizeof key);
    key.Id0 = shader.ShaderId0;
    key.Id1 = shader.ShaderId1;
    key.Cull = static_cast<uint8_t>(mCullMode);
    key.Blend = mBlend.Enabled;
    key.EqRgb = static_cast<uint8_t>(mBlend.EquationRgb);
    key.EqAlpha = static_cast<uint8_t>(mBlend.EquationAlpha);
    key.SrcRgb = static_cast<uint8_t>(mBlend.SourceRgb);
    key.DstRgb = static_cast<uint8_t>(mBlend.DestRgb);
    key.SrcAlpha = static_cast<uint8_t>(mBlend.SourceAlpha);
    key.DstAlpha = static_cast<uint8_t>(mBlend.DestAlpha);
    std::string mapKey(reinterpret_cast<const char*>(&key), sizeof key);
    const auto found = mPipelines.find(mapKey);
    if (found != mPipelines.end()) return found->second;

    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, shader.Vertex, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, shader.Fragment, "main", nullptr},
    };
    const VkVertexInputBindingDescription binding = {0, shader.VertexStride,
                                                     VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.push_back({0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    if (shader.UsedTextures[0])
        attributes.push_back({1, 0, VK_FORMAT_R32G32_SFLOAT, shader.TextureCoordinateOffsets[0]});
    if (shader.UsedTextures[1])
        attributes.push_back({2, 0, VK_FORMAT_R32G32_SFLOAT, shader.TextureCoordinateOffsets[1]});
    if (shader.UsesTexture2)
        attributes.push_back({3, 0, VK_FORMAT_R32G32_SFLOAT, shader.TextureCoordinateOffsets[2]});
    if (shader.HasFog)
        attributes.push_back({4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, shader.FogOffset});
    if (shader.HasGrayscale)
        attributes.push_back({5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, shader.GrayscaleOffset});
    for (uint32_t input = 0; input < shader.NumInputs && input < 8; ++input) {
        attributes.push_back({6 + input, 0,
                              shader.HasAlpha ? VK_FORMAT_R32G32B32A32_SFLOAT
                                              : VK_FORMAT_R32G32B32_SFLOAT,
                              shader.InputOffsets[input]});
    }
    if (shader.ShadowCoordinateOffset != kAbsent)
        attributes.push_back({14, 0, VK_FORMAT_R32G32B32_SFLOAT, shader.ShadowCoordinateOffset});
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0F;
    raster.cullMode = mCullMode == GfxNativeCullMode::KeepAll ? VK_CULL_MODE_NONE
                                                              : VK_CULL_MODE_BACK_BIT;
    raster.frontFace = mCullMode == GfxNativeCullMode::KeepClockwise ? VK_FRONT_FACE_CLOCKWISE
                                                                     : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = mBlend.Enabled;
    blendAttachment.colorBlendOp = ToVk(mBlend.EquationRgb);
    blendAttachment.alphaBlendOp = ToVk(mBlend.EquationAlpha);
    blendAttachment.srcColorBlendFactor = ToVk(mBlend.SourceRgb);
    blendAttachment.dstColorBlendFactor = ToVk(mBlend.DestRgb);
    blendAttachment.srcAlphaBlendFactor = ToVk(mBlend.SourceAlpha);
    blendAttachment.dstAlphaBlendFactor = ToVk(mBlend.DestAlpha);
    blendAttachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                            VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = mPipelineLayout;
    info.renderPass = mVk->scanoutPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!mVk->Check(mVk->vk.vkCreateGraphicsPipelines(mVk->device, mVk->pipelineCache, 1, &info,
                                                      nullptr, &pipeline),
                    "vkCreateGraphicsPipelines(fast3d)")) {
        pipeline = VK_NULL_HANDLE;
    }
    mVk->pipelineCacheDirty = true;
    mPipelines.emplace(std::move(mapKey), pipeline);
    return pipeline;
}

void GfxRenderingAPIOrbis::DrawTriangles(float vbo[], size_t vboLength, size_t triangleCount) {
    if (!mFrameActive || mCurrentFramebuffer != 0 || mCurrentShader == nullptr ||
        mCurrentShader->Vertex == VK_NULL_HANDLE || vbo == nullptr || vboLength == 0 ||
        triangleCount == 0) {
        return;
    }
    const size_t vertexCount = triangleCount * 3;
    if (vboLength % vertexCount != 0 ||
        (vboLength / vertexCount) * sizeof(float) != mCurrentShader->VertexStride) {
        mVk->NoteError("fast3d vertex stride mismatch (shader %llx/%llx)",
                       (unsigned long long)mCurrentShader->ShaderId0,
                       (unsigned long long)mCurrentShader->ShaderId1);
        return;
    }
    const VkPipeline pipeline = Pipeline(*mCurrentShader);
    if (pipeline == VK_NULL_HANDLE) return;

    // One reservation (a flush recycles the ring and the descriptor pool).
    const VkDeviceSize storageAlign = std::max<VkDeviceSize>(
        16U, mVk->properties.limits.minStorageBufferOffsetAlignment);
    const VkDeviceSize vertexBytes = vboLength * sizeof(float);
    if (mVk->setsAllocated + 1U >= 4096U) mVk->Flush();
    if (!mVk->RingReserve(vertexBytes + sizeof(DrawUniforms) + storageAlign + 32U)) return;

    const auto resolve = [&](uint32_t unit, bool used) -> TextureRecord* {
        if (!used) return nullptr;
        const auto found = mTextures.find(mSelected[unit]);
        return found != mTextures.end() && found->second.Uploaded ? &found->second : nullptr;
    };
    TextureRecord* textures[3] = {resolve(0, mCurrentShader->UsedTextures[0]),
                                  resolve(1, mCurrentShader->UsedTextures[1]),
                                  resolve(2, mCurrentShader->UsesTexture2)};
    VkDescriptorImageInfo images[4] = {};
    for (uint32_t unit = 0; unit < 3; ++unit) {
        if (textures[unit] != nullptr) {
            images[unit] = {SamplerFor(textures[unit]->Sampler), textures[unit]->Image.view,
                            VK_IMAGE_LAYOUT_GENERAL};
            mDrawUniforms.TextureWidth[unit] = static_cast<int32_t>(textures[unit]->Image.width);
            mDrawUniforms.TextureHeight[unit] = static_cast<int32_t>(textures[unit]->Image.height);
        } else {
            images[unit] = {mVk->linearClamp, mVk->whiteImage.view, VK_IMAGE_LAYOUT_GENERAL};
            mDrawUniforms.TextureWidth[unit] = 1;
            mDrawUniforms.TextureHeight[unit] = 1;
        }
        mDrawUniforms.TextureFiltering[unit] = static_cast<int32_t>(mFilterMode);
    }
    images[3] = {mVk->nearestClamp, mVk->uintImage.view, VK_IMAGE_LAYOUT_GENERAL};
    mDrawUniforms.TextureWidth[3] = 1;
    mDrawUniforms.TextureHeight[3] = 1;
    mDrawUniforms.TextureFiltering[3] = static_cast<int32_t>(FILTER_NONE);

    const VkDeviceSize vertexOffset = mVk->RingAlloc(vertexBytes, 16U);
    std::memcpy(mVk->ringPointer + vertexOffset, vbo, vertexBytes);
    const VkDeviceSize uniformOffset = mVk->RingAlloc(sizeof(DrawUniforms), storageAlign);
    std::memcpy(mVk->ringPointer + uniformOffset, &mDrawUniforms, sizeof(DrawUniforms));

    VkDescriptorSet set = mVk->AllocateSet(mSetLayout);
    if (set == VK_NULL_HANDLE) return;
    const VkDescriptorBufferInfo uniformInfo = {mVk->ring, uniformOffset, sizeof(DrawUniforms)};
    VkWriteDescriptorSet writes[5] = {};
    for (uint32_t binding = 0; binding < 5; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        if (binding < 4) {
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &images[binding];
        } else {
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &uniformInfo;
        }
    }
    mVk->vk.vkUpdateDescriptorSets(mVk->device, 5, writes, 0, nullptr);

    mVk->OpenScanoutPass();
    const VkCommandBuffer cb = mVk->mainCb;
    mVk->vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    mVk->vk.vkCmdBindVertexBuffers(cb, 0, 1, &mVk->ring, &vertexOffset);
    mVk->vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1,
                                    &set, 0, nullptr);
    mVk->vk.vkCmdPushConstants(cb, mPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(float) * 16, mTransform.data());
    mVk->vk.vkCmdSetViewport(cb, 0, 1, &mViewport);
    VkRect2D scissor = mScissor;
    scissor.offset.x = std::max(0, scissor.offset.x);
    scissor.offset.y = std::max(0, scissor.offset.y);
    mVk->vk.vkCmdSetScissor(cb, 0, 1, &scissor);
    mVk->vk.vkCmdSetBlendConstants(cb, mBlend.ConstantColor);
    mVk->vk.vkCmdDraw(cb, static_cast<uint32_t>(vertexCount), 1, 0, 0);
    mVk->workRecorded = true;
}

// Stands in for the surface/swapchain entry points: the PS4 has no Vulkan
// WSI (scanout goes through VideoOut), and only imgui's ImGui_ImplVulkanH_*
// window helpers, which this backend never calls, use them.
VKAPI_ATTR VkResult VKAPI_CALL ImGuiVulkanWsiUnavailable() {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

// imgui_impl_vulkan without prototypes: its entry points come from RADV's
// ICD lookup (there is no Vulkan loader on the PS4). LoadFunctions demands
// every entry point it knows, the WSI ones included.
PFN_vkVoidFunction ImGuiVulkanLoader(const char* name, void* user) {
    auto* vk = static_cast<Impl*>(user);
    PFN_vkVoidFunction function = vk->vk.vkGetDeviceProcAddr(vk->device, name);
    if (function == nullptr) function = vk_icdGetInstanceProcAddr(vk->instance, name);
    if (function == nullptr && std::strstr(name, "KHR") != nullptr &&
        (std::strstr(name, "Surface") != nullptr || std::strstr(name, "Swapchain") != nullptr ||
         std::strcmp(name, "vkAcquireNextImageKHR") == 0 ||
         std::strcmp(name, "vkQueuePresentKHR") == 0)) {
        function = reinterpret_cast<PFN_vkVoidFunction>(&ImGuiVulkanWsiUnavailable);
    }
    if (function == nullptr) std::printf("[triaevum] imgui: missing Vulkan entry point %s\n", name);
    return function;
}

bool GfxRenderingAPIOrbis::InitImGuiBackend() {
    if (mImGuiReady) return true;
    if (!mInitialized) {
        std::printf("[triaevum] imgui: backend requested before the renderer is up\n");
        return false;
    }
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, ImGuiVulkanLoader, mVk)) {
        mVk->NoteError("ImGui Vulkan entry points unavailable");
        return false;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = mVk->instance;
    info.PhysicalDevice = mVk->gpu;
    info.Device = mVk->device;
    info.QueueFamily = 0;
    info.Queue = mVk->queue;
    info.RenderPass = mVk->scanoutPass;
    info.MinImageCount = 2;
    info.ImageCount = 2;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineCache = mVk->pipelineCache;
    info.DescriptorPoolSize = 256;
    mImGuiReady = ImGui_ImplVulkan_Init(&info);
    if (!mImGuiReady) mVk->NoteError("ImGui_ImplVulkan_Init failed");
    else std::printf("[triaevum] imgui: Vulkan backend ready\n");
    return mImGuiReady;
}

} // namespace

GfxRenderingAPI* CreateOrbisRenderingApi() {
    return new GfxRenderingAPIOrbis();
}

} // namespace Fast

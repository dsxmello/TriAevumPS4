// Host (Linux) harness for ps4_vk_pica_backend.cpp: the exact backend source,
// built against the system Vulkan loader (+ Khronos validation layers) and
// the system glslang, fed with the canonical shaders of the title corpus
// profile and synthetic PICA geometry. It exercises every contract entry
// point the console drain uses - draws (all corpus fragments, 8/16-bit
// indices, an unsupported-format attribute, a per-instance binding),
// memory fills, a presented display transfer and a materialized raw copy -
// so Vulkan usage errors surface here instead of on the console.
//
//   hosttest/run.sh   (builds and runs with VK_LAYER_KHRONOS_validation)

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <nlohmann/json.hpp>

#include "fast/renderer3ds/pica_shader_source_identity.h"
#include "ps4_vk_pica_backend.h"

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                          const char *name);

// The console links RADV's ICD entry point; on the host the loader stands in.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                              const char *name) {
    return vkGetInstanceProcAddr(instance, name);
}

namespace {

void Log(const char *text) { std::printf("[backend] %s\n", text); }

int g_failures = 0;
#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("EXPECT FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: %s <corpus profile json>\n", argv[0]);
        return 2;
    }
    std::ifstream stream(argv[1]);
    const nlohmann::json profile = nlohmann::json::parse(stream);
    std::vector<std::string> vertexSources;
    std::vector<std::string> fragmentSources;
    for (const auto &entry : profile.at("shaders")) {
        const std::string stage = entry.value("stage", "");
        if (stage == "vertex") vertexSources.push_back(entry.at("source"));
        if (stage == "fragment") fragmentSources.push_back(entry.at("source"));
    }
    std::printf("corpus: %zu vertex, %zu fragment\n", vertexSources.size(),
                fragmentSources.size());

    auto backend = triaevum::ps4::Ps4VkPicaRenderBackend::Create(Log, 1920, 1080);
    if (backend == nullptr) {
        std::printf("backend creation failed\n");
        return 1;
    }
    triaevum::ps4::Ps4VkPicaRenderBackend::PresentOptions presentOptions;
    presentOptions.renderScale = 3;
    presentOptions.layout = triaevum::ps4::Ps4VkPicaRenderBackend::ScreenLayout::SideBySide;
    backend->SetPresentOptions(presentOptions);
    uint64_t committedBytes = 0;
    backend->SetPicaPhysicalMemoryAccess(
        nullptr,
        [&](std::span<const Fast::Renderer3ds::PicaPhysicalMemoryWrite> writes) {
            for (const auto &write : writes) committedBytes += write.Bytes.size();
            return true;
        },
        nullptr, nullptr, nullptr);

    // Synthetic geometry: binding 0 = float4 position + 3 x u8 colour + pad
    // (u8x3 exercises the unsupported-format conversion on GPUs without it),
    // binding 1 = s16x2, binding 2 = one per-instance float4.
    struct Vertex {
        float position[4];
        uint8_t color[4];
    };
    std::vector<Vertex> vertices = {
        {{-0.5F, -0.5F, -0.5F, 1.0F}, {255, 0, 0, 0}},
        {{0.5F, -0.5F, -0.5F, 1.0F}, {0, 255, 0, 0}},
        {{0.0F, 0.5F, -0.5F, 1.0F}, {0, 0, 255, 0}},
        {{0.5F, 0.5F, -0.5F, 1.0F}, {255, 255, 0, 0}},
    };
    std::vector<int16_t> shorts = {0, 0, 16, 0, 16, 16, 0, 16};
    const float instanceValue[4] = {1.0F, 2.0F, 3.0F, 4.0F};
    const Fast::Renderer3ds::PicaVertexBindingView bindings[3] = {
        {0, sizeof(Vertex), false,
         std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(vertices.data()),
                                  vertices.size() * sizeof(Vertex))},
        {1, 4, false,
         std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(shorts.data()),
                                  shorts.size() * sizeof(int16_t))},
        {2, 16, true,
         std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(instanceValue), 16)},
    };
    const Fast::Renderer3ds::PicaVertexAttributeView attributes[4] = {
        {0, 0, Fast::Renderer3ds::PicaVertexFormat::Float, 4, 0},
        {1, 0, Fast::Renderer3ds::PicaVertexFormat::UnsignedByte, 3, 16},
        {2, 1, Fast::Renderer3ds::PicaVertexFormat::SignedShort, 2, 0},
        {3, 2, Fast::Renderer3ds::PicaVertexFormat::Float, 4, 0},
    };
    // Packed vertex uniforms: b@0, flip@4, i[4]@16, f[96]@80. The f block
    // holds an identity-ish matrix in the first rows.
    std::vector<uint8_t> vertexUniforms(80 + 96 * 16, 0);
    for (int row = 0; row < 4; ++row) {
        float one = 1.0F;
        std::memcpy(vertexUniforms.data() + 80 + row * 16 + row * 4, &one, 4);
    }
    std::vector<uint8_t> fragmentUniforms(6112, 0);
    std::vector<uint8_t> textureBytes(16 * 16 * 4 + 8 * 8 * 4, 0x80);
    std::vector<uint32_t> lut(24 * 256, 0x123456);
    const uint8_t indices8[6] = {0, 1, 2, 2, 1, 3};
    const uint16_t indices16[3] = {0, 1, 2};

    Fast::Renderer3ds::PicaTextureView texture{};
    texture.Slot = 0;
    texture.Width = 16;
    texture.Height = 16;
    texture.MaxMipLevel = 1;
    texture.MipLinear = true;
    texture.MinLinear = true;
    texture.NativeFormat = 0;
    texture.PhysicalAddress = 0x18400000;
    texture.NativeBytes = textureBytes;
    texture.NativeContentHash = 0xABCDEF;
    texture.NativeContentHashAvailable = true;
    texture.MagLinear = true;
    texture.NativeWrapS = 2;
    texture.NativeWrapT = 3;

    constexpr uint32_t kColor = 0x18000000;
    constexpr uint32_t kDepth = 0x18100000;
    constexpr uint16_t kWidth = 240;
    constexpr uint16_t kHeight = 400;

    for (int frame = 0; frame < 3; ++frame) {
        backend->BeginFrame(static_cast<uint32_t>(frame & 1));
        // Clear the colour + depth buffers through a memory fill.
        Fast::Renderer3ds::PicaMemoryFillView fill{};
        fill.RenderTargetNamespace = 1;
        fill.StartPhysicalAddress = kColor;
        fill.EndPhysicalAddress = kColor + kWidth * kHeight * 4;
        fill.Value = 0x204080FF;
        fill.Control = 1U | (1U << 9U);
        std::string error;
        EXPECT(backend->SubmitPicaMemoryFill(fill, &error));
        fill.StartPhysicalAddress = kDepth;
        fill.EndPhysicalAddress = kDepth + kWidth * kHeight * 4;
        fill.Value = 0x00FFFFFF;
        EXPECT(backend->SubmitPicaMemoryFill(fill, &error));

        size_t drawIndex = 0;
        for (const std::string &fragmentSource : fragmentSources) {
            Fast::Renderer3ds::PicaDrawView draw{};
            draw.RenderTargetNamespace = 1;
            draw.VertexShaderSource = vertexSources.at(0);
            draw.VertexShaderSourceIdentity =
                Fast::Renderer3ds::IdentifyPicaShaderSource(draw.VertexShaderSource);
            draw.FragmentShaderSource = fragmentSource;
            draw.FragmentShaderSourceIdentity =
                Fast::Renderer3ds::IdentifyPicaShaderSource(fragmentSource);
            draw.VertexUniformBytes = vertexUniforms;
            draw.FragmentUniformBytes = fragmentUniforms;
            draw.VertexBindings = bindings;
            draw.VertexAttributes = attributes;
            draw.Textures = std::span<const Fast::Renderer3ds::PicaTextureView>(&texture, 1);
            draw.LightingLut.PackedEntries = lut;
            draw.LightingLut.ContentHash = 0x55;
            draw.LightingLut.ContentHashAvailable = true;
            draw.Topology = (drawIndex % 3U) == 2U ? Fast::Renderer3ds::PicaTopology::TriangleFan
                                                    : Fast::Renderer3ds::PicaTopology::TriangleList;
            draw.CullMode = (drawIndex & 1U) != 0U
                                ? Fast::Renderer3ds::NativeCullMode::KeepCounterClockwise
                                : Fast::Renderer3ds::NativeCullMode::KeepAll;
            draw.ViewportWidth = kWidth;
            draw.ViewportHeight = kHeight;
            draw.FramebufferWidth = kWidth;
            draw.FramebufferHeight = kHeight;
            draw.FramebufferColorPhysicalAddress = kColor;
            draw.FramebufferDepthPhysicalAddress = kDepth;
            draw.FramebufferColorFormat = 0;
            draw.FramebufferDepthFormat = 3;
            draw.ColorWriteMask = 0xF;
            draw.DepthTestEnabled = true;
            draw.DepthWriteEnabled = true;
            draw.DepthCompare = Fast::Renderer3ds::PicaCompareFunction::GreaterOrEqual;
            draw.Stencil.Enabled = (drawIndex % 4U) == 1U;
            draw.Stencil.Pass = Fast::Renderer3ds::PicaStencilAction::Replace;
            draw.Stencil.Reference = 1;
            draw.Stencil.WriteMask = 0xFF;
            draw.Stencil.CompareMask = 0xFF;
            draw.Blend.Enabled = (drawIndex % 2U) == 0U;
            draw.Blend.SourceRgb = Fast::Renderer3ds::NativeBlendFactor::SourceAlpha;
            draw.Blend.DestRgb = Fast::Renderer3ds::NativeBlendFactor::OneMinusSourceAlpha;
            draw.LogicOperation = Fast::Renderer3ds::PicaLogicOperation::Xor;
            draw.ScissorMode = (drawIndex % 5U) == 3U ? 3U : 0U;
            draw.ScissorX1 = 10;
            draw.ScissorY1 = 20;
            draw.ScissorX2 = 200;
            draw.ScissorY2 = 300;
            if ((drawIndex % 3U) == 0U) {
                draw.Indexed = true;
                draw.IndicesAre16Bit = false;
                draw.IndexBytes = std::span<const uint8_t>(indices8, 6);
                draw.VertexCount = 6;
            } else if ((drawIndex % 3U) == 1U) {
                draw.Indexed = true;
                draw.IndicesAre16Bit = true;
                draw.IndexBytes = std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t *>(indices16), sizeof indices16);
                draw.VertexCount = 3;
            } else {
                draw.VertexCount = 4;
            }
            EXPECT(backend->SubmitPicaDraw(draw, &error));
            ++drawIndex;
        }

        // A raw copy (materialized) and the presented LCD transfer.
        Fast::Renderer3ds::PicaDisplayTransferView transfer{};
        transfer.RenderTargetNamespace = 1;
        transfer.InputPhysicalAddress = kColor;
        transfer.OutputPhysicalAddress = 0x18300000;
        transfer.InputWidth = kWidth;
        transfer.InputHeight = kHeight;
        transfer.OutputWidth = kWidth;
        transfer.OutputHeight = kHeight;
        transfer.TextureCopyBytes = 4096;
        transfer.MaterializeGuest = true;
        EXPECT(backend->SubmitPicaDisplayTransfer(transfer, &error));
        transfer.OutputPhysicalAddress = 0x18200000;
        transfer.TextureCopyBytes = 0;
        transfer.MaterializeGuest = false;
        transfer.Present = true;
        EXPECT(backend->SubmitPicaDisplayTransfer(transfer, &error));
        EXPECT(backend->QueuePicaCompletion(static_cast<uint64_t>(frame) + 1U, &error));
        EXPECT(backend->TakePicaCompletions().size() == 1U);
        const bool presented = backend->EndFrame();
        EXPECT(presented);
        const auto *pixels = static_cast<const uint32_t *>(backend->ScanoutPixels(frame & 1));
        const uint32_t pitch = backend->ScanoutPitchPixels();
        std::printf("frame %d: presented=%d corner=%#x centre=%#x pitch=%u committed=%llu\n",
                    frame, presented ? 1 : 0, pixels[0], pixels[540 * pitch + 960], pitch,
                    (unsigned long long)committedBytes);
        EXPECT(pixels[540 * pitch + 960] != 0U);
    }
    const auto &stats = backend->Stats();
    std::printf("stats: draws=%llu skipped=%llu fail=%llu shaders=%llu shaderFail=%llu "
                "pipelines=%llu textures=%llu targets=%llu presents=%llu mats=%llu "
                "submits=%llu vkErrors=%llu last='%s'\n",
                (unsigned long long)stats.draws, (unsigned long long)stats.drawsSkipped,
                (unsigned long long)stats.drawFailures, (unsigned long long)stats.shadersCompiled,
                (unsigned long long)stats.shaderFailures, (unsigned long long)stats.pipelines,
                (unsigned long long)stats.textures, (unsigned long long)stats.targets,
                (unsigned long long)stats.presents, (unsigned long long)stats.materializations,
                (unsigned long long)stats.submits, (unsigned long long)stats.vkErrors,
                stats.lastError);
    EXPECT(stats.drawsSkipped == 0U);
    EXPECT(stats.drawFailures == 0U);
    EXPECT(stats.shaderFailures == 0U);
    EXPECT(stats.vkErrors == 0U);
    EXPECT(stats.materializations == 3U);
    EXPECT(committedBytes > 0U);

    // Product mode, widescreen: the native top screen (480x400, stored
    // rotated) widened to the 16:9 output, a scene draw and a placed UI draw
    // (raster canvas), presented at once. The picture must reach the
    // scanout's left edge, which the native 5:3 fit leaves black.
    {
        backend->SetImmediatePresentation(true);
        presentOptions.renderScale = 2;
        presentOptions.layout = triaevum::ps4::Ps4VkPicaRenderBackend::ScreenLayout::TopOnly;
        presentOptions.outputAspect = 16.0F / 9.0F;
        backend->SetPresentOptions(presentOptions);
        constexpr uint32_t kTopColor = 0x18500000;
        constexpr uint32_t kTopDepth = 0x18600000;
        constexpr uint16_t kTopWidth = 480;
        constexpr uint16_t kTopHeight = 400;
        for (int frame = 0; frame < 2; ++frame) {
            backend->BeginFrame(static_cast<uint32_t>(frame & 1));
            std::string error;
            Fast::Renderer3ds::PicaMemoryFillView fill{};
            fill.RenderTargetNamespace = 1;
            fill.StartPhysicalAddress = kTopColor;
            fill.EndPhysicalAddress = kTopColor + kTopWidth * kTopHeight * 4;
            fill.Value = 0x204080FF;
            fill.Control = 1U | (1U << 9U);
            EXPECT(backend->SubmitPicaMemoryFill(fill, &error));
            fill.StartPhysicalAddress = kTopDepth;
            fill.EndPhysicalAddress = kTopDepth + kTopWidth * kTopHeight * 4;
            fill.Value = 0x00FFFFFF;
            EXPECT(backend->SubmitPicaMemoryFill(fill, &error));
            for (const auto domain : {Fast::Renderer3ds::PicaCompositionDomain::Scene,
                                      Fast::Renderer3ds::PicaCompositionDomain::Ui}) {
                Fast::Renderer3ds::PicaDrawView draw{};
                draw.RenderTargetNamespace = 1;
                draw.CompositionDomain = domain;
                draw.VertexShaderSource = vertexSources.at(0);
                draw.VertexShaderSourceIdentity =
                    Fast::Renderer3ds::IdentifyPicaShaderSource(draw.VertexShaderSource);
                draw.FragmentShaderSource = fragmentSources.at(0);
                draw.FragmentShaderSourceIdentity =
                    Fast::Renderer3ds::IdentifyPicaShaderSource(draw.FragmentShaderSource);
                draw.VertexUniformBytes = vertexUniforms;
                draw.FragmentUniformBytes = fragmentUniforms;
                draw.VertexBindings = bindings;
                draw.VertexAttributes = attributes;
                draw.Textures = std::span<const Fast::Renderer3ds::PicaTextureView>(&texture, 1);
                draw.LightingLut.PackedEntries = lut;
                draw.LightingLut.ContentHash = 0x55;
                draw.LightingLut.ContentHashAvailable = true;
                draw.Topology = Fast::Renderer3ds::PicaTopology::TriangleList;
                draw.CullMode = Fast::Renderer3ds::NativeCullMode::KeepAll;
                draw.ViewportWidth = kTopWidth;
                draw.ViewportHeight = kTopHeight;
                draw.FramebufferWidth = kTopWidth;
                draw.FramebufferHeight = kTopHeight;
                draw.FramebufferColorPhysicalAddress = kTopColor;
                draw.FramebufferDepthPhysicalAddress = kTopDepth;
                draw.FramebufferColorFormat = 0;
                draw.FramebufferDepthFormat = 3;
                draw.ColorWriteMask = 0xF;
                draw.ScissorMode = domain == Fast::Renderer3ds::PicaCompositionDomain::Ui ? 3U : 0U;
                draw.ScissorX1 = 10;
                draw.ScissorY1 = 20;
                draw.ScissorX2 = 400;
                draw.ScissorY2 = 380;
                draw.Indexed = true;
                draw.IndicesAre16Bit = false;
                draw.IndexBytes = std::span<const uint8_t>(indices8, 6);
                draw.VertexCount = 6;
                EXPECT(backend->SubmitPicaDraw(draw, &error));
            }
            Fast::Renderer3ds::PicaDisplayTransferView transfer{};
            transfer.RenderTargetNamespace = 1;
            transfer.InputPhysicalAddress = kTopColor;
            transfer.OutputPhysicalAddress = 0x18700000;
            transfer.InputWidth = kTopWidth;
            transfer.InputHeight = kTopHeight;
            transfer.OutputWidth = kTopWidth / 2U;
            transfer.OutputHeight = kTopHeight;
            transfer.Flags = 1U << 24U;  // 2:1 horizontal box filter
            transfer.Present = true;
            EXPECT(backend->SubmitPicaDisplayTransfer(transfer, &error));
            EXPECT(backend->EndFrame());
            const auto *pixels = static_cast<const uint32_t *>(backend->ScanoutPixels(frame & 1));
            const uint32_t pitch = backend->ScanoutPitchPixels();
            const uint32_t edge = pixels[540 * pitch + 20];
            std::printf("widescreen frame %d: left edge=%#x centre=%#x\n", frame, edge,
                        pixels[540 * pitch + 960]);
            EXPECT(edge != 0U && edge != 0xff000000U);
        }
        EXPECT(backend->Stats().vkErrors == 0U);
        EXPECT(backend->Stats().drawFailures == 0U);
    }

    std::string error;
    EXPECT(backend->ResetPicaState(&error));
    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}

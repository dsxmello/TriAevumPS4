// TriAevum PS4 - Phase 2: Vulkan PICA render backend (see
// ps4_vk_pica_backend.h for the design). Desktop reference:
// three_ds_recomp src/fast/backends/gfx_vulkan_pica*.cpp; the transfer,
// present and fill math mirror the archived GLES2 backend, which carried the
// desktop GL backend's semantics verbatim.

#include "ps4_vk_pica_backend.h"
#include "ps4_vk_pica_backend_impl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <sys/stat.h>

#include <vulkan/vulkan_core.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include "fast/renderer3ds/pica_framebuffer_encode.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/oot3d/scene_presentation_policy.h"
#include "fast/renderer3ds/pica_composition.h"
#include "fast/renderer3ds/pica_draw_coverage.h"
#include "oot3d/renderer/ui_presentation_layout.h"
#include "oot3d/renderer/pica_texture_decode.h"

// RADV's loader entry point (linked statically; there is no Vulkan loader on
// the PS4 - the VK probe recipe).
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                              const char *name);

namespace triaevum::ps4 {
namespace {

using Fast::Renderer3ds::NativeBlendEquation;
using Fast::Renderer3ds::NativeBlendFactor;
using Fast::Renderer3ds::NativeCullMode;
using Oot3d::Renderer::PicaCompareFunction;
using Oot3d::Renderer::PicaLogicOperation;
using Oot3d::Renderer::PicaStencilAction;
using Oot3d::Renderer::PicaTopology;
using Oot3d::Renderer::PicaVertexFormat;

constexpr const char *kSpirvCacheDir = "/data/triaevum/spv-cache";
constexpr const char *kPipelineCachePath = "/data/triaevum/vk-pipeline-cache.bin";
constexpr VkDeviceSize kRingBytes = 64ULL << 20;
constexpr VkDeviceSize kPoolChunkBytes = 64ULL << 20;
constexpr uint32_t kMaxSetsPerFlush = 4096;
constexpr uint32_t kLightingLutBinding = 13;

// ---------------------------------------------------------------- dispatch --

using detail::Vk;

// ------------------------------------------------------------ PICA -> Vk --

VkCompareOp ToVkCompare(PicaCompareFunction compare) {
    switch (compare) {
        case PicaCompareFunction::Never: return VK_COMPARE_OP_NEVER;
        case PicaCompareFunction::Always: return VK_COMPARE_OP_ALWAYS;
        case PicaCompareFunction::Equal: return VK_COMPARE_OP_EQUAL;
        case PicaCompareFunction::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case PicaCompareFunction::Less: return VK_COMPARE_OP_LESS;
        case PicaCompareFunction::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case PicaCompareFunction::Greater: return VK_COMPARE_OP_GREATER;
        case PicaCompareFunction::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkStencilOp ToVkStencil(PicaStencilAction action) {
    switch (action) {
        case PicaStencilAction::Keep: return VK_STENCIL_OP_KEEP;
        case PicaStencilAction::Zero: return VK_STENCIL_OP_ZERO;
        case PicaStencilAction::Replace: return VK_STENCIL_OP_REPLACE;
        case PicaStencilAction::Increment: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case PicaStencilAction::Decrement: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case PicaStencilAction::Invert: return VK_STENCIL_OP_INVERT;
        case PicaStencilAction::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case PicaStencilAction::DecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkBlendOp ToVkBlendOp(NativeBlendEquation equation) {
    switch (equation) {
        case NativeBlendEquation::Add: return VK_BLEND_OP_ADD;
        case NativeBlendEquation::Subtract: return VK_BLEND_OP_SUBTRACT;
        case NativeBlendEquation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case NativeBlendEquation::Min: return VK_BLEND_OP_MIN;
        case NativeBlendEquation::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkBlendFactor ToVkBlendFactor(NativeBlendFactor factor) {
    switch (factor) {
        case NativeBlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
        case NativeBlendFactor::One: return VK_BLEND_FACTOR_ONE;
        case NativeBlendFactor::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
        case NativeBlendFactor::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case NativeBlendFactor::DestColor: return VK_BLEND_FACTOR_DST_COLOR;
        case NativeBlendFactor::OneMinusDestColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case NativeBlendFactor::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case NativeBlendFactor::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case NativeBlendFactor::DestAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case NativeBlendFactor::OneMinusDestAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case NativeBlendFactor::ConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case NativeBlendFactor::OneMinusConstantColor:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case NativeBlendFactor::ConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case NativeBlendFactor::OneMinusConstantAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case NativeBlendFactor::SourceAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkLogicOp ToVkLogicOp(PicaLogicOperation operation) {
    switch (operation) {
        case PicaLogicOperation::Clear: return VK_LOGIC_OP_CLEAR;
        case PicaLogicOperation::And: return VK_LOGIC_OP_AND;
        case PicaLogicOperation::AndReverse: return VK_LOGIC_OP_AND_REVERSE;
        case PicaLogicOperation::Copy: return VK_LOGIC_OP_COPY;
        case PicaLogicOperation::Set: return VK_LOGIC_OP_SET;
        case PicaLogicOperation::CopyInverted: return VK_LOGIC_OP_COPY_INVERTED;
        case PicaLogicOperation::NoOp: return VK_LOGIC_OP_NO_OP;
        case PicaLogicOperation::Invert: return VK_LOGIC_OP_INVERT;
        case PicaLogicOperation::Nand: return VK_LOGIC_OP_NAND;
        case PicaLogicOperation::Or: return VK_LOGIC_OP_OR;
        case PicaLogicOperation::Nor: return VK_LOGIC_OP_NOR;
        case PicaLogicOperation::Xor: return VK_LOGIC_OP_XOR;
        case PicaLogicOperation::Equivalent: return VK_LOGIC_OP_EQUIVALENT;
        case PicaLogicOperation::AndInverted: return VK_LOGIC_OP_AND_INVERTED;
        case PicaLogicOperation::OrReverse: return VK_LOGIC_OP_OR_REVERSE;
        case PicaLogicOperation::OrInverted: return VK_LOGIC_OP_OR_INVERTED;
    }
    return VK_LOGIC_OP_COPY;
}

VkPrimitiveTopology ToVkTopology(PicaTopology topology) {
    switch (topology) {
        case PicaTopology::TriangleList:
        case PicaTopology::GeometryShader:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PicaTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PicaTopology::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// Non-normalized PICA attributes, as the desktop ToNativeVkVertexFormat.
VkFormat ToVkVertexFormat(PicaVertexFormat format, uint8_t components) {
    static constexpr VkFormat kSignedByte[4] = {VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED,
                                                VK_FORMAT_R8G8B8_SSCALED,
                                                VK_FORMAT_R8G8B8A8_SSCALED};
    static constexpr VkFormat kUnsignedByte[4] = {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED,
                                                  VK_FORMAT_R8G8B8_USCALED,
                                                  VK_FORMAT_R8G8B8A8_USCALED};
    static constexpr VkFormat kSignedShort[4] = {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                                 VK_FORMAT_R16G16B16_SSCALED,
                                                 VK_FORMAT_R16G16B16A16_SSCALED};
    static constexpr VkFormat kFloat[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                           VK_FORMAT_R32G32B32_SFLOAT,
                                           VK_FORMAT_R32G32B32A32_SFLOAT};
    const size_t index = components - 1U;
    switch (format) {
        case PicaVertexFormat::SignedByte: return kSignedByte[index];
        case PicaVertexFormat::UnsignedByte: return kUnsignedByte[index];
        case PicaVertexFormat::SignedShort: return kSignedShort[index];
        case PicaVertexFormat::Float: return kFloat[index];
    }
    return kFloat[index];
}

uint32_t VertexComponentBytes(PicaVertexFormat format) {
    switch (format) {
        case PicaVertexFormat::SignedByte:
        case PicaVertexFormat::UnsignedByte: return 1U;
        case PicaVertexFormat::SignedShort: return 2U;
        case PicaVertexFormat::Float: return 4U;
    }
    return 4U;
}

float ReadVertexComponent(PicaVertexFormat format, const uint8_t *source) {
    switch (format) {
        case PicaVertexFormat::SignedByte: return static_cast<float>(static_cast<int8_t>(*source));
        case PicaVertexFormat::UnsignedByte: return static_cast<float>(*source);
        case PicaVertexFormat::SignedShort: {
            int16_t raw = 0;
            std::memcpy(&raw, source, sizeof raw);
            return static_cast<float>(raw);
        }
        case PicaVertexFormat::Float: {
            float raw = 0.0F;
            std::memcpy(&raw, source, sizeof raw);
            return raw;
        }
    }
    return 0.0F;
}

// The desktop DecodeNativePicaWrap: 0 clamp, 2/6/7 repeat, 3 mirror. Border
// (1) has no border colour state in v1 and clamps to the edge.
VkSamplerAddressMode ToVkAddressMode(uint8_t wrap) {
    switch (wrap) {
        case 2:
        case 6:
        case 7: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

uint32_t ColorBytesPerPixel(uint8_t format) { return format == 0U ? 4U : (format == 1U ? 3U : 2U); }

uint32_t FillElementSize(uint16_t control) {
    return (control & (1U << 9U)) != 0U ? 4U : ((control & (1U << 8U)) != 0U ? 3U : 2U);
}

uint8_t FillByte(const Fast::Renderer3ds::PicaMemoryFillView &fill, uint32_t index) {
    const uint32_t shift = (index % FillElementSize(fill.Control)) * 8U;
    return static_cast<uint8_t>(fill.Value >> shift);
}

// The desktop DecodeNativePicaClearColor/DecodeNativePicaClearDepthStencil.
VkClearColorValue DecodeFillColor(const Fast::Renderer3ds::PicaMemoryFillView &fill,
                                  uint8_t format) {
    const auto unorm = [](uint32_t value, uint32_t maximum) {
        return static_cast<float>(value) / static_cast<float>(maximum);
    };
    const uint8_t b0 = FillByte(fill, 0U);
    const uint8_t b1 = FillByte(fill, 1U);
    const uint8_t b2 = FillByte(fill, 2U);
    const uint8_t b3 = FillByte(fill, 3U);
    VkClearColorValue color{};
    if (format == 0U) {
        color.float32[0] = unorm(b3, 0xFFU);
        color.float32[1] = unorm(b2, 0xFFU);
        color.float32[2] = unorm(b1, 0xFFU);
        color.float32[3] = unorm(b0, 0xFFU);
    } else if (format == 1U) {
        color.float32[0] = unorm(b2, 0xFFU);
        color.float32[1] = unorm(b1, 0xFFU);
        color.float32[2] = unorm(b0, 0xFFU);
        color.float32[3] = 1.0F;
    } else {
        const uint16_t packed = static_cast<uint16_t>(b0 | (b1 << 8U));
        if (format == 2U) {
            color.float32[0] = unorm((packed >> 11U) & 0x1FU, 0x1FU);
            color.float32[1] = unorm((packed >> 5U) & 0x3FU, 0x3FU);
            color.float32[2] = unorm(packed & 0x1FU, 0x1FU);
            color.float32[3] = 1.0F;
        } else if (format == 3U) {
            color.float32[0] = unorm((packed >> 11U) & 0x1FU, 0x1FU);
            color.float32[1] = unorm((packed >> 6U) & 0x1FU, 0x1FU);
            color.float32[2] = unorm((packed >> 1U) & 0x1FU, 0x1FU);
            color.float32[3] = static_cast<float>(packed & 1U);
        } else {
            color.float32[0] = unorm((packed >> 12U) & 0xFU, 0xFU);
            color.float32[1] = unorm((packed >> 8U) & 0xFU, 0xFU);
            color.float32[2] = unorm((packed >> 4U) & 0xFU, 0xFU);
            color.float32[3] = unorm(packed & 0xFU, 0xFU);
        }
    }
    return color;
}

VkClearDepthStencilValue DecodeFillDepth(const Fast::Renderer3ds::PicaMemoryFillView &fill,
                                         uint8_t format) {
    const uint32_t b0 = FillByte(fill, 0U);
    const uint32_t b1 = FillByte(fill, 1U);
    const uint32_t b2 = FillByte(fill, 2U);
    const uint32_t depth = format == 0U ? b0 | (b1 << 8U) : b0 | (b1 << 8U) | (b2 << 16U);
    VkClearDepthStencilValue value{};
    value.depth = static_cast<float>(depth) /
                  static_cast<float>(format == 0U ? 0xFFFFU : 0xFFFFFFU);
    value.stencil = format == 3U ? FillByte(fill, 3U) : 0U;
    return value;
}

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::string IdentityKey(const Fast::Renderer3ds::PicaShaderSourceIdentity &id, char stage) {
    char key[80];
    snprintf(key, sizeof key, "%c-%016llx-%016llx-%llx", stage, (unsigned long long)id.Id,
             (unsigned long long)id.SecondaryHash, (unsigned long long)id.Size);
    return key;
}

bool ReadFile(const char *path, std::vector<uint8_t> &out) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bool ok = size > 0;
    if (ok) {
        out.resize(static_cast<size_t>(size));
        ok = fread(out.data(), 1, out.size(), file) == out.size();
    }
    fclose(file);
    return ok;
}

bool WriteFile(const char *path, const void *data, size_t bytes) {
    FILE *file = fopen(path, "wb");
    if (file == nullptr) return false;
    const bool ok = fwrite(data, 1, bytes, file) == bytes;
    fclose(file);
    return ok;
}

// GLSL 4.50 (Vulkan dialect) -> SPIR-V 1.3. No optimizer: ACO does the real
// work, and glslang's own SPIR-V passes would need SPIRV-Tools.
bool CompileGlsl(EShLanguage stage, const std::string &source, std::vector<uint32_t> &spirv,
                 std::string &log) {
    glslang::TShader shader(stage);
    const char *text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        log = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        log = program.getInfoLog();
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.validate = false;
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
    return !spirv.empty();
}

// Debug fallback for a fragment program glslang rejects: the canonical
// vertex transform still runs, colours are primary * texture0.
constexpr char kFallbackFragment[] =
    "#version 450\n"
    "layout(location=0) in vec4 pica_primary_color;\n"
    "layout(location=1) in vec2 pica_texcoord0;\n"
    "layout(set=0,binding=1) uniform sampler2D pica_texture0;\n"
    "layout(location=0) out vec4 pica_color;\n"
    "void main() { pica_color = pica_primary_color * texture(pica_texture0, pica_texcoord0); }\n";

// Scanout: fullscreen triangle; uv follows the desktop GL scanout shader's
// convention (uv.y = 0 at the BOTTOM of the window) so the transpose + flip
// below is the desktop math verbatim. Vulkan's NDC y = -1 is the top row.
constexpr char kPresentVertex[] =
    "#version 450\n"
    "layout(location=0) out vec2 pica_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    pica_uv = vec2(p.x, 1.0 - p.y);\n"
    "}\n";
constexpr char kPresentFragment[] =
    "#version 450\n"
    "layout(location=0) in vec2 pica_uv;\n"
    "layout(set=0,binding=0) uniform sampler2D pica_scanout;\n"
    "layout(push_constant) uniform PicaPresent { int flip_y; } present;\n"
    "layout(location=0) out vec4 pica_color;\n"
    "void main() {\n"
    "    vec2 uv = vec2(pica_uv.y, pica_uv.x);\n"
    "    if (present.flip_y != 0) uv.y = 1.0 - uv.y;\n"
    "    pica_color = vec4(texture(pica_scanout, uv).rgb, 1.0);\n"
    "}\n";

}  // namespace


// ================================================================ lifecycle ==

Ps4VkPicaRenderBackend::Ps4VkPicaRenderBackend() : mImpl(std::make_unique<Impl>()) {}

Ps4VkPicaRenderBackend::~Ps4VkPicaRenderBackend() {
    if (mImpl != nullptr) mImpl->Shutdown();
}

std::unique_ptr<Ps4VkPicaRenderBackend> Ps4VkPicaRenderBackend::Create(LogFn log,
                                                                       uint32_t scanoutWidth,
                                                                       uint32_t scanoutHeight) {
    std::unique_ptr<Ps4VkPicaRenderBackend> backend(new Ps4VkPicaRenderBackend());
    backend->mImpl->owner = backend.get();
    backend->mImpl->log = log;
    if (!backend->mImpl->Initialize(scanoutWidth, scanoutHeight)) {
        backend->mImpl->Log("vk-backend: initialization failed - renderer inactive (%s)",
                            backend->mImpl->stats.lastError);
        return nullptr;
    }
    return backend;
}

const VkBackendStats &Ps4VkPicaRenderBackend::Stats() const { return mImpl->stats; }

void *Ps4VkPicaRenderBackend::ScanoutPixels(uint32_t index) const {
    return index < kScanoutCount ? mImpl->scanouts[index].pixels : nullptr;
}

uint32_t Ps4VkPicaRenderBackend::ScanoutPitchPixels() const { return mImpl->scanoutPitchPixels; }

bool Ps4VkPicaRenderBackend::Impl::Initialize(uint32_t width, uint32_t height) {
    const auto start = NowUs();
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (createInstance == nullptr) {
        NoteError("vk_icdGetInstanceProcAddr returned no vkCreateInstance");
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "TriAevum PS4";
    app.pEngineName = "TriAevum PICA";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    if (!Check(createInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance")) return false;
#define PS4VK_LOAD_INSTANCE(name)                                                        \
    vk.name = reinterpret_cast<PFN_##name>(vk_icdGetInstanceProcAddr(instance, #name)); \
    if (vk.name == nullptr) {                                                            \
        NoteError("missing instance entry point %s", #name);                            \
        return false;                                                                    \
    }
    PS4VK_INSTANCE_FUNCS(PS4VK_LOAD_INSTANCE)
#undef PS4VK_LOAD_INSTANCE

    uint32_t count = 1;
    if (vk.vkEnumeratePhysicalDevices(instance, &count, &gpu) < 0 || gpu == VK_NULL_HANDLE) {
        NoteError("no physical device");
        return false;
    }
    vk.vkGetPhysicalDeviceProperties(gpu, &properties);
    vk.vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    VkPhysicalDeviceFeatures supported{};
    vk.vkGetPhysicalDeviceFeatures(gpu, &supported);
    logicOpSupported = supported.logicOp == VK_TRUE;

    // Queue family 0: GFX + COMPUTE + TRANSFER (probe evidence: the only one).
    VkPhysicalDeviceFeatures enabled{};
    enabled.logicOp = supported.logicOp;
    enabled.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    enabled.independentBlend = supported.independentBlend;
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = 0;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pEnabledFeatures = &enabled;
    if (!Check(vk.vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice"))
        return false;
#define PS4VK_LOAD_DEVICE(name)                                                       \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(device, #name)); \
    if (vk.name == nullptr) {                                                         \
        NoteError("missing device entry point %s", #name);                           \
        return false;                                                                 \
    }
    PS4VK_DEVICE_FUNCS(PS4VK_LOAD_DEVICE)
#undef PS4VK_LOAD_DEVICE
    vk.vkGetDeviceQueue(device, 0, 0, &queue);

    for (const VkFormat candidate :
         {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties format{};
        vk.vkGetPhysicalDeviceFormatProperties(gpu, candidate, &format);
        if ((format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
            depthFormat = candidate;
            depthHasStencil = candidate != VK_FORMAT_D16_UNORM;
            break;
        }
    }
    if (depthFormat == VK_FORMAT_UNDEFINED) {
        NoteError("no depth attachment format");
        return false;
    }

    glslang::InitializeProcess();
    mkdir("/data/triaevum", 0777);
    mkdir(kSpirvCacheDir, 0777);
    if (!CreateDeviceObjects()) return false;
    if (!CreateScanouts(width, height)) return false;
    Log("vk-backend ready: %s (api %u.%u.%u) depth=%d logicOp=%d ubo-align=%llu in %llu ms",
        properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
        VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
        static_cast<int>(depthFormat), logicOpSupported ? 1 : 0,
        (unsigned long long)properties.limits.minUniformBufferOffsetAlignment,
        (unsigned long long)((NowUs() - start) / 1000U));
    return true;
}

bool Ps4VkPicaRenderBackend::Impl::CreateDeviceObjects() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = 0;
    if (!Check(vk.vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool),
               "vkCreateCommandPool"))
        return false;
    VkCommandBuffer buffers[2] = {};
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 2;
    if (!Check(vk.vkAllocateCommandBuffers(device, &allocate, buffers), "vkAllocateCommandBuffers"))
        return false;
    uploadCb = buffers[0];
    mainCb = buffers[1];
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!Check(vk.vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence")) return false;

    // Garlic (DEVICE_LOCAL | HOST_VISIBLE) suits the ring: the CPU only
    // streams sequential writes into it and the GPU reads it at full speed.
    if (!CreateBuffer(kRingBytes,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ring, &ringMemory, &ringPointer))
        return false;

    // Descriptor set layout of the canonical generated programs.
    const VkShaderStageFlags fragment = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding picaBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, fragment, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, fragment, nullptr},
        {kLightingLutBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = static_cast<uint32_t>(std::size(picaBindings));
    setInfo.pBindings = picaBindings;
    if (!Check(vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &picaSetLayout),
               "vkCreateDescriptorSetLayout(pica)"))
        return false;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &picaSetLayout;
    if (!Check(vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &picaPipelineLayout),
               "vkCreatePipelineLayout(pica)"))
        return false;

    const VkDescriptorSetLayoutBinding presentBinding = {
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr};
    setInfo.bindingCount = 1;
    setInfo.pBindings = &presentBinding;
    if (!Check(vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &presentSetLayout),
               "vkCreateDescriptorSetLayout(present)"))
        return false;
    const VkPushConstantRange pushRange = {fragment, 0, 4};
    layoutInfo.pSetLayouts = &presentSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (!Check(vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &presentPipelineLayout),
               "vkCreatePipelineLayout(present)"))
        return false;

    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxSetsPerFlush * 2U},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSetsPerFlush * 4U},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxSetsPerFlush},
        // The product's Fast3D draws (draw-uniform storage buffer).
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxSetsPerFlush},
    };
    VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptorInfo.maxSets = kMaxSetsPerFlush;
    descriptorInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    descriptorInfo.pPoolSizes = poolSizes;
    if (!Check(vk.vkCreateDescriptorPool(device, &descriptorInfo, nullptr, &descriptorPool),
               "vkCreateDescriptorPool"))
        return false;

    // Render passes: everything lives in GENERAL (v1), LOAD/STORE always.
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachments[1] = attachments[0];
    attachments[1].format = depthFormat;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    const VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_GENERAL};
    const VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    const VkAccessFlags anyAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const VkSubpassDependency dependencies[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, anyAccess, anyAccess, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, anyAccess, anyAccess, 0},
    };
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 2;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = dependencies;
    if (!Check(vk.vkCreateRenderPass(device, &passInfo, nullptr, &picaPass),
               "vkCreateRenderPass(pica)"))
        return false;
    subpass.pDepthStencilAttachment = nullptr;
    passInfo.attachmentCount = 1;
    if (!Check(vk.vkCreateRenderPass(device, &passInfo, nullptr, &scanoutPass),
               "vkCreateRenderPass(scanout)"))
        return false;

    // Pipeline cache from the previous session (ACO compiles cost ~2 ms
    // each on the Jaguar - probe evidence).
    std::vector<uint8_t> cacheBytes;
    const bool cached = ReadFile(kPipelineCachePath, cacheBytes);
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = cached ? cacheBytes.size() : 0U;
    cacheInfo.pInitialData = cached ? cacheBytes.data() : nullptr;
    if (vk.vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache) != VK_SUCCESS) {
        cacheInfo.initialDataSize = 0;
        cacheInfo.pInitialData = nullptr;
        if (!Check(vk.vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache),
                   "vkCreatePipelineCache"))
            return false;
    }
    Log("vk-backend: pipeline cache %s (%zu bytes)", cached ? "loaded" : "empty",
        cached ? cacheBytes.size() : size_t{0});

    // Samplers + dummy images.
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0F;
    if (!Check(vk.vkCreateSampler(device, &samplerInfo, nullptr, &nearestClamp), "vkCreateSampler"))
        return false;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    if (!Check(vk.vkCreateSampler(device, &samplerInfo, nullptr, &linearClamp), "vkCreateSampler"))
        return false;

    if (!CreateImage(whiteImage, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, false) ||
        !CreateImage(uintImage, 1, 1, VK_FORMAT_R32_UINT,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, false))
        return false;
    EnsureRecording();
    ToGeneral(uploadCb, whiteImage);
    ToGeneral(uploadCb, uintImage);
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkClearColorValue white{};
    white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0F;
    vk.vkCmdClearColorImage(uploadCb, whiteImage.image, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &range);
    VkClearColorValue zero{};
    vk.vkCmdClearColorImage(uploadCb, uintImage.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    uploadsRecorded = true;

    // Presentation programs.
    VkShaderModule presentVs = VK_NULL_HANDLE;
    VkShaderModule presentFs = VK_NULL_HANDLE;
    std::string compileLog;
    if (!BuildModule(kPresentVertex, true, &presentVs, &compileLog) ||
        !BuildModule(kPresentFragment, false, &presentFs, &compileLog)) {
        NoteError("present shader: %s", compileLog.c_str());
        return false;
    }
    presentReplace = CreatePresentPipeline(false, presentVs, presentFs);
    presentOverlay = CreatePresentPipeline(true, presentVs, presentFs);
    if (presentReplace == VK_NULL_HANDLE || presentOverlay == VK_NULL_HANDLE) return false;
    if (!BuildModule(kFallbackFragment, false, &fallbackFragment.module, &compileLog)) {
        NoteError("fallback fragment: %s", compileLog.c_str());
        return false;
    }
    Flush();
    return stats.vkErrors == 0;
}

// Linear R8G8B8A8 images in DEVICE_LOCAL | HOST_VISIBLE memory, bound at
// a 2 MiB aligned address (VideoOut's requirement) - exactly the VK probe
// framebuffers that displayed correctly on the console.
bool Ps4VkPicaRenderBackend::Impl::CreateScanouts(uint32_t width, uint32_t height) {
    scanoutWidth = width;
    scanoutHeight = height;
    for (uint32_t index = 0; index < kScanoutCount; ++index) {
        Scanout &scanout = scanouts[index];
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_LINEAR;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vk.vkCreateImage(device, &info, nullptr, &scanout.image), "vkCreateImage(scanout)"))
            return false;
        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(device, scanout.image, &requirements);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const uint32_t type = FindMemoryType(requirements.memoryTypeBits, want, 0);
        if (type == UINT32_MAX) {
            NoteError("no memory type for the scanout images");
            return false;
        }
        constexpr VkDeviceSize kSlack = 2ULL << 20;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size + kSlack;
        allocate.memoryTypeIndex = type;
        if (!Check(vk.vkAllocateMemory(device, &allocate, nullptr, &scanout.memory),
                   "vkAllocateMemory(scanout)"))
            return false;
        uint8_t *base = nullptr;
        if (!Check(vk.vkMapMemory(device, scanout.memory, 0, VK_WHOLE_SIZE, 0,
                                  reinterpret_cast<void **>(&base)),
                   "vkMapMemory(scanout)"))
            return false;
        VkDeviceSize offset =
            ((reinterpret_cast<uintptr_t>(base) + kSlack - 1U) / kSlack) * kSlack -
            reinterpret_cast<uintptr_t>(base);
        offset = (offset + requirements.alignment - 1U) / requirements.alignment *
                 requirements.alignment;
        if (!Check(vk.vkBindImageMemory(device, scanout.image, scanout.memory, offset),
                   "vkBindImageMemory(scanout)"))
            return false;
        const VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout{};
        vk.vkGetImageSubresourceLayout(device, scanout.image, &subresource, &layout);
        scanout.pixels = base + offset + layout.offset;
        scanoutPitchPixels = static_cast<uint32_t>(layout.rowPitch / 4U);
        std::memset(scanout.pixels, 0, layout.size);

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = scanout.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vk.vkCreateImageView(device, &view, nullptr, &scanout.view),
                   "vkCreateImageView(scanout)"))
            return false;
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = scanoutPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &scanout.view;
        framebuffer.width = width;
        framebuffer.height = height;
        framebuffer.layers = 1;
        if (!Check(vk.vkCreateFramebuffer(device, &framebuffer, nullptr, &scanout.framebuffer),
                   "vkCreateFramebuffer(scanout)"))
            return false;
        EnsureRecording();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = scanout.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vk.vkCmdPipelineBarrier(mainCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &barrier);
        workRecorded = true;
        Log("vk-backend: scanout %u at %p pitch %u px (memory type %u)", index,
            static_cast<void *>(scanout.pixels), scanoutPitchPixels, type);
    }
    Flush();
    return stats.vkErrors == 0;
}

void Ps4VkPicaRenderBackend::Impl::Shutdown() {
    if (device == VK_NULL_HANDLE) return;
    vk.vkDeviceWaitIdle(device);
    // The process is exiting; the driver reclaims everything with the
    // device. Persist the pipeline cache, the only state worth keeping.
    owner->SavePipelineCache();
}

// =================================================================== memory ==

uint32_t Ps4VkPicaRenderBackend::Impl::FindMemoryType(uint32_t bits,
                                                      VkMemoryPropertyFlags required,
                                                      VkMemoryPropertyFlags preferred) const {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        if ((bits & (1U << type)) == 0U) continue;
        const VkMemoryPropertyFlags flags = memory.memoryTypes[type].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) return type;
        if (fallback == UINT32_MAX) fallback = type;
    }
    return fallback;
}

bool Ps4VkPicaRenderBackend::Impl::CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                                               VkMemoryPropertyFlags required,
                                               VkMemoryPropertyFlags preferred, VkBuffer *buffer,
                                               VkDeviceMemory *bufferMemory, uint8_t **mapped) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vk.vkCreateBuffer(device, &info, nullptr, buffer), "vkCreateBuffer")) return false;
    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    const uint32_t type = FindMemoryType(requirements.memoryTypeBits, required, preferred);
    if (type == UINT32_MAX) {
        NoteError("no memory type for a %llu byte buffer", (unsigned long long)bytes);
        return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (!Check(vk.vkAllocateMemory(device, &allocate, nullptr, bufferMemory),
               "vkAllocateMemory(buffer)") ||
        !Check(vk.vkBindBufferMemory(device, *buffer, *bufferMemory, 0), "vkBindBufferMemory"))
        return false;
    if (mapped != nullptr &&
        !Check(vk.vkMapMemory(device, *bufferMemory, 0, VK_WHOLE_SIZE, 0,
                              reinterpret_cast<void **>(mapped)),
               "vkMapMemory(buffer)"))
        return false;
    return true;
}

// Optimal-tiling images. Pooled images (textures, LUTs: many, small, never
// freed one by one in v1) bump-allocate from 64 MiB chunks so the winsys sees
// a handful of direct-memory allocations instead of thousands.
bool Ps4VkPicaRenderBackend::Impl::CreateImage(Image &out, uint32_t width, uint32_t height,
                                              VkFormat format, VkImageUsageFlags usage,
                                              VkImageAspectFlags aspect, bool pooled,
                                              uint32_t mipLevels) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = mipLevels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vk.vkCreateImage(device, &info, nullptr, &out.image), "vkCreateImage")) return false;
    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(device, out.image, &requirements);
    const uint32_t type =
        FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == UINT32_MAX) {
        NoteError("no device-local memory type for an image");
        return false;
    }
    VkDeviceMemory boundMemory = VK_NULL_HANDLE;
    VkDeviceSize boundOffset = 0;
    if (pooled && requirements.size <= kPoolChunkBytes) {
        std::vector<MemoryChunk> &chunks = pool[type];
        for (MemoryChunk &chunk : chunks) {
            const VkDeviceSize offset = (chunk.used + requirements.alignment - 1U) /
                                        requirements.alignment * requirements.alignment;
            if (offset + requirements.size <= chunk.size) {
                boundMemory = chunk.memory;
                boundOffset = offset;
                chunk.used = offset + requirements.size;
                break;
            }
        }
        if (boundMemory == VK_NULL_HANDLE) {
            MemoryChunk chunk;
            chunk.size = kPoolChunkBytes;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = chunk.size;
            allocate.memoryTypeIndex = type;
            if (!Check(vk.vkAllocateMemory(device, &allocate, nullptr, &chunk.memory),
                       "vkAllocateMemory(pool)"))
                return false;
            chunk.used = requirements.size;
            boundMemory = chunk.memory;
            chunks.push_back(chunk);
            Log("vk-backend: image pool chunk %zu (type %u)", chunks.size(), type);
        }
    } else {
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (!Check(vk.vkAllocateMemory(device, &allocate, nullptr, &out.dedicated),
                   "vkAllocateMemory(image)"))
            return false;
        boundMemory = out.dedicated;
    }
    if (!Check(vk.vkBindImageMemory(device, out.image, boundMemory, boundOffset),
               "vkBindImageMemory"))
        return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, mipLevels, 0, 1};
    if (!Check(vk.vkCreateImageView(device, &view, nullptr, &out.view), "vkCreateImageView"))
        return false;
    out.width = width;
    out.height = height;
    out.format = format;
    out.aspect = aspect;
    out.mipLevels = mipLevels;
    return true;
}

void Ps4VkPicaRenderBackend::Impl::DestroyImage(Image &image) {
    if (image.view != VK_NULL_HANDLE) vk.vkDestroyImageView(device, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) vk.vkDestroyImage(device, image.image, nullptr);
    if (image.dedicated != VK_NULL_HANDLE) vk.vkFreeMemory(device, image.dedicated, nullptr);
    image = Image{};
}

void Ps4VkPicaRenderBackend::Impl::FreePools() {
    for (auto &chunks : pool) {
        for (MemoryChunk &chunk : chunks) vk.vkFreeMemory(device, chunk.memory, nullptr);
        chunks.clear();
    }
}

// ================================================================ recording ==

void Ps4VkPicaRenderBackend::Impl::EnsureRecording() {
    if (recording) return;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vk.vkBeginCommandBuffer(uploadCb, &begin), "vkBeginCommandBuffer(upload)");
    Check(vk.vkBeginCommandBuffer(mainCb, &begin), "vkBeginCommandBuffer(main)");
    recording = true;
    uploadsRecorded = false;
    workRecorded = false;
}

void Ps4VkPicaRenderBackend::Impl::FullBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                            nullptr);
}

void Ps4VkPicaRenderBackend::Impl::ToGeneral(VkCommandBuffer cb, const Image &image) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {image.aspect, 0, image.mipLevels, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
}

void Ps4VkPicaRenderBackend::Impl::EndRenderPass() {
    if (activeTarget != nullptr || scanoutPassActive) {
        vk.vkCmdEndRenderPass(mainCb);
        activeTarget = nullptr;
        scanoutPassActive = false;
    }
}

// Submits the uploads, then the ordered work, and waits. Every per-flush
// resource (ring space, descriptor sets) is recycled afterwards.
void Ps4VkPicaRenderBackend::Impl::Flush() {
    if (!recording) return;
    EndRenderPass();
    FullBarrier(uploadCb);
    vk.vkEndCommandBuffer(uploadCb);
    vk.vkEndCommandBuffer(mainCb);
    recording = false;
    if (uploadsRecorded || workRecorded) {
        const VkCommandBuffer buffers[2] = {uploadCb, mainCb};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 2;
        submit.pCommandBuffers = buffers;
        const uint64_t start = NowUs();
        if (Check(vk.vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit")) {
            Check(vk.vkWaitForFences(device, 1, &fence, VK_TRUE, 5ULL * 1000000000ULL),
                  "vkWaitForFences");
            vk.vkResetFences(device, 1, &fence);
        }
        stats.gpuWaitUs += NowUs() - start;
        ++stats.submits;
    }
    vk.vkResetCommandPool(device, commandPool, 0);
    vk.vkResetDescriptorPool(device, descriptorPool, 0);
    setsAllocated = 0;
    ringHead = 0;
    EnsureRecording();
}

bool Ps4VkPicaRenderBackend::Impl::RingReserve(VkDeviceSize bytes) {
    // Worst-case alignment slack per allocation is folded into the caller's
    // estimate; 1 KiB more covers the rounding of the reservation itself.
    if (bytes + 1024U > kRingBytes) return false;
    if (ringHead + bytes + 1024U > kRingBytes) Flush();
    return true;
}

VkDeviceSize Ps4VkPicaRenderBackend::Impl::RingAlloc(VkDeviceSize bytes, VkDeviceSize alignment) {
    const VkDeviceSize offset = (ringHead + alignment - 1U) / alignment * alignment;
    ringHead = offset + bytes;
    return offset;
}

VkDescriptorSet Ps4VkPicaRenderBackend::Impl::AllocateSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = descriptorPool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!Check(vk.vkAllocateDescriptorSets(device, &info, &set), "vkAllocateDescriptorSets"))
        return VK_NULL_HANDLE;
    ++setsAllocated;
    return set;
}

// ================================================================= shaders ==

bool Ps4VkPicaRenderBackend::Impl::BuildModule(const std::string &source, bool vertex,
                                              VkShaderModule *module, std::string *compileLog) {
    std::vector<uint32_t> spirv;
    if (!CompileGlsl(vertex ? EShLangVertex : EShLangFragment, source, spirv, *compileLog))
        return false;
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    return Check(vk.vkCreateShaderModule(device, &info, nullptr, module), "vkCreateShaderModule");
}

void Ps4VkPicaRenderBackend::Impl::DumpShaderFailure(const char *stage, std::string_view source,
                                                    const std::string &compileLog) {
    if (shaderDumps >= 8) return;
    char path[96];
    snprintf(path, sizeof path, "/data/triaevum/shader_fail_%d.txt", shaderDumps++);
    FILE *file = fopen(path, "w");
    if (file == nullptr) return;
    fprintf(file, "stage=%s\n%s\n----\n", stage, compileLog.c_str());
    fwrite(source.data(), 1, source.size(), file);
    fclose(file);
    Log("vk-backend: shader failure dumped to %s", path);
}

// Shader modules keyed by source identity. SPIR-V persists under
// /data/triaevum/spv-cache so only the first session pays glslang.
Ps4VkPicaRenderBackend::Impl::Shader *Ps4VkPicaRenderBackend::Impl::GetShader(
    bool vertex, const Fast::Renderer3ds::PicaShaderSourceIdentity &identity,
    std::string_view source) {
    const std::string key = IdentityKey(identity, vertex ? 'v' : 'f');
    auto found = shaders.find(key);
    if (found != shaders.end()) return &found->second;
    Shader shader;
    if (!vertex) {
        shader.integerTexture0 =
            source.find("usampler2D pica_texture0") != std::string_view::npos;
    }
    const std::string path = std::string(kSpirvCacheDir) + "/" + key + ".spv";
    std::vector<uint8_t> cached;
    std::vector<uint32_t> spirv;
    if (ReadFile(path.c_str(), cached) && cached.size() % 4U == 0U && cached.size() >= 20U) {
        spirv.resize(cached.size() / 4U);
        std::memcpy(spirv.data(), cached.data(), cached.size());
        ++stats.shadersFromDisk;
    } else {
        const uint64_t start = NowUs();
        std::string compileLog;
        if (!CompileGlsl(vertex ? EShLangVertex : EShLangFragment, std::string(source), spirv,
                         compileLog)) {
            ++stats.shaderFailures;
            NoteError("%s shader rejected by glslang (%zu bytes)", vertex ? "vertex" : "fragment",
                      source.size());
            DumpShaderFailure(vertex ? "vertex" : "fragment", source, compileLog);
            shader.failed = true;
            return &shaders.emplace(key, shader).first->second;
        }
        const uint64_t elapsed = NowUs() - start;
        stats.shaderCompileUs += elapsed;
        ++stats.shadersCompiled;
        Log("vk-backend: compiled %s %s (%zu bytes GLSL -> %zu words) in %llu ms",
            vertex ? "vertex" : "fragment", key.c_str(), source.size(), spirv.size(),
            (unsigned long long)(elapsed / 1000U));
        WriteFile(path.c_str(), spirv.data(), spirv.size() * sizeof(uint32_t));
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    if (!Check(vk.vkCreateShaderModule(device, &info, nullptr, &shader.module),
               "vkCreateShaderModule")) {
        shader.failed = true;
    }
    return &shaders.emplace(key, shader).first->second;
}

// =============================================================== resources ==

Ps4VkPicaRenderBackend::Impl::Target *Ps4VkPicaRenderBackend::Impl::GetTarget(
    const TargetKey &key) {
    auto found = targets.find(key);
    if (found != targets.end()) {
        found->second.lastUse = ++useSerial;
        ApplyFillsTo(key, found->second);
        return &found->second;
    }
    EndRenderPass();
    Target target;
    const VkImageUsageFlags colorUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageAspectFlags depthAspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (depthHasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U);
    const uint32_t width = uint32_t{key.width} * renderScale;
    // The native top screen (480x400, stored rotated: storage Y is the
    // presented width) widens with the output aspect.
    const bool topScreen = key.width == Fast::Oot3d::kNativeTopScreenPhysicalWidth &&
                           key.height == Fast::Oot3d::kNativeTopScreenPhysicalHeight;
    const uint32_t height = static_cast<uint32_t>(std::lround(
        static_cast<float>(key.height) * static_cast<float>(renderScale) *
        (topScreen ? topScreenExpansion : 1.0F)));
    if (!CreateImage(target.color, width, height, VK_FORMAT_R8G8B8A8_UNORM, colorUsage,
                     VK_IMAGE_ASPECT_COLOR_BIT, false) ||
        !CreateImage(target.depth, width, height, depthFormat,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     depthAspect, false)) {
        DestroyImage(target.color);
        DestroyImage(target.depth);
        return nullptr;
    }
    // The framebuffer image view of a combined depth/stencil format must
    // name both aspects, which CreateImage did.
    const VkImageView views[2] = {target.color.view, target.depth.view};
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = picaPass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = views;
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.layers = 1;
    if (!Check(vk.vkCreateFramebuffer(device, &framebuffer, nullptr, &target.framebuffer),
               "vkCreateFramebuffer(target)")) {
        DestroyImage(target.color);
        DestroyImage(target.depth);
        return nullptr;
    }
    target.scaleX = static_cast<float>(width) / static_cast<float>(key.width);
    target.scaleY = static_cast<float>(height) / static_cast<float>(key.height);
    ToGeneral(mainCb, target.color);
    ToGeneral(mainCb, target.depth);
    const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageSubresourceRange depthRange = {depthAspect, 0, 1, 0, 1};
    const VkClearColorValue black{};
    const VkClearDepthStencilValue far = {1.0F, 0U};
    vk.vkCmdClearColorImage(mainCb, target.color.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1,
                            &colorRange);
    vk.vkCmdClearDepthStencilImage(mainCb, target.depth.image, VK_IMAGE_LAYOUT_GENERAL, &far, 1,
                                   &depthRange);
    FullBarrier(mainCb);
    workRecorded = true;
    ++stats.targets;
    target.lastUse = ++useSerial;
    Log("vk-backend: render target %ux%u -> %ux%u color=0x%08x(f%u) depth=0x%08x(f%u) ns=%llu",
        key.width, key.height, width, height, key.colorAddress, key.colorFormat, key.depthAddress,
        key.depthFormat, (unsigned long long)key.namespace_);
    Target &stored = targets.emplace(key, target).first->second;
    ApplyFillsTo(key, stored);
    return &stored;
}

// Fills that cover a whole colour/depth buffer clear it (the desktop
// coversColor/coversDepth rule); the guest RAM side already ran in GuestFill.
void Ps4VkPicaRenderBackend::Impl::ApplyFillsTo(const TargetKey &key, Target &target) {
    if (pendingFills.empty()) return;
    const uint64_t colorEnd = uint64_t{key.colorAddress} + uint64_t{key.width} * key.height *
                                                              ColorBytesPerPixel(key.colorFormat);
    const uint64_t depthEnd =
        uint64_t{key.depthAddress} +
        uint64_t{key.width} * key.height *
            std::max<uint32_t>(1U, Fast::Renderer3ds::PicaDepthFormatBytes(key.depthFormat));
    for (auto it = pendingFills.begin(); it != pendingFills.end();) {
        if (it->namespace_ != key.namespace_) {
            ++it;
            continue;
        }
        const bool coversColor = it->start <= key.colorAddress && it->end >= colorEnd;
        const bool coversDepth =
            key.depthAddress != 0U && it->start <= key.depthAddress && it->end >= depthEnd;
        if (!coversColor && !coversDepth) {
            ++it;
            continue;
        }
        EndRenderPass();
        FullBarrier(mainCb);
        const Fast::Renderer3ds::PicaMemoryFillView view{it->namespace_, it->start, it->end,
                                                         it->value, it->control};
        if (coversColor) {
            const VkClearColorValue color = DecodeFillColor(view, key.colorFormat);
            const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.vkCmdClearColorImage(mainCb, target.color.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1,
                                    &range);
        }
        if (coversDepth) {
            const VkClearDepthStencilValue depth = DecodeFillDepth(view, key.depthFormat);
            const VkImageSubresourceRange range = {target.depth.aspect, 0, 1, 0, 1};
            vk.vkCmdClearDepthStencilImage(mainCb, target.depth.image, VK_IMAGE_LAYOUT_GENERAL,
                                           &depth, 1, &range);
        }
        FullBarrier(mainCb);
        workRecorded = true;
        ++stats.clears;
        it = pendingFills.erase(it);
    }
}

void Ps4VkPicaRenderBackend::Impl::BeginTargetPass(Target &target) {
    if (activeTarget == &target) return;
    EndRenderPass();
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = picaPass;
    begin.framebuffer = target.framebuffer;
    begin.renderArea = {{0, 0}, {target.color.width, target.color.height}};
    vk.vkCmdBeginRenderPass(mainCb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    activeTarget = &target;
    workRecorded = true;
}

// Textures decode on the CPU into RGBA8 - the whole mip chain, as the
// desktop backend does (base-only sampling aliased and shimmered on the
// console, 0.92) - and upload through the ring on the upload command buffer
// (they are immutable per content identity, so running every upload ahead of
// the frame's ordered work is safe).
Ps4VkPicaRenderBackend::Impl::Image *Ps4VkPicaRenderBackend::Impl::GetTexture(
    const Oot3d::Renderer::PicaTextureView &view) {
    if (view.Width == 0U || view.Height == 0U) return nullptr;
    const uint64_t hash = view.NativeContentHashAvailable ? view.NativeContentHash : 0x7FFFFFFFULL;
    const TextureKey key{view.PhysicalAddress, hash,
                         view.NativeContentHashAvailable ? 0U : view.NativeBytes.size(),
                         view.Width, view.Height, view.NativeFormat, view.MaxMipLevel};
    auto found = textures.find(key);
    if (found != textures.end()) return &found->second;

    // Desktop NativePicaMipLevelCount / NativePicaEncodedMipSize.
    uint32_t wantedLevels = 1U;
    for (uint32_t w = view.Width, h = view.Height; w > 8U && h > 8U; w >>= 1U, h >>= 1U)
        ++wantedLevels;
    wantedLevels = std::min<uint32_t>(wantedLevels, uint32_t{view.MaxMipLevel} + 1U);
    static constexpr uint8_t kNibblesPerPixel[14] = {8, 6, 4, 4, 4, 4, 4, 2, 2, 2, 1, 1, 1, 2};
    decoded.clear();
    std::vector<VkBufferImageCopy> regions;
    std::vector<uint8_t> level;
    size_t nativeOffset = 0;
    std::string decodeError;
    for (uint32_t index = 0; index < wantedLevels; ++index) {
        const uint32_t w = std::max<uint32_t>(1U, view.Width >> index);
        const uint32_t h = std::max<uint32_t>(1U, view.Height >> index);
        if (view.NativeFormat >= std::size(kNibblesPerPixel)) break;
        const size_t tiledW = (std::max<uint32_t>(8U, w) + 7U) & ~7U;
        const size_t tiledH = (std::max<uint32_t>(8U, h) + 7U) & ~7U;
        const size_t bytes = (tiledW * tiledH * kNibblesPerPixel[view.NativeFormat] + 1U) / 2U;
        if (nativeOffset + bytes > view.NativeBytes.size()) break;
        level.clear();
        if (!Oot3d::Renderer::DecodePicaTextureRgba8(
                view.NativeFormat, static_cast<uint16_t>(w), static_cast<uint16_t>(h),
                view.NativeBytes.subspan(nativeOffset, bytes), level, &decodeError) ||
            level.size() != size_t{w} * h * 4U)
            break;
        VkBufferImageCopy copy{};
        copy.bufferOffset = decoded.size();
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, index, 0, 1};
        copy.imageExtent = {w, h, 1};
        regions.push_back(copy);
        decoded.insert(decoded.end(), level.begin(), level.end());
        nativeOffset += bytes;
    }
    if (regions.empty()) {
        if (!decodeError.empty()) NoteError("texture decode failed: %s", decodeError.c_str());
        decoded.assign(static_cast<size_t>(view.Width) * view.Height * 4U, 0xFF);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {view.Width, view.Height, 1};
        regions.push_back(copy);
    }
    const uint32_t levels = static_cast<uint32_t>(regions.size());
    const VkDeviceSize bytes = decoded.size();
    if (!RingReserve(bytes + 16U)) {
        NoteError("texture %ux%u exceeds the upload ring", view.Width, view.Height);
        return nullptr;
    }
    Image image;
    if (!CreateImage(image, view.Width, view.Height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, true, levels)) {
        return nullptr;
    }
    const VkDeviceSize offset = RingAlloc(bytes, 16U);
    std::memcpy(ringPointer + offset, decoded.data(), bytes);
    for (auto &region : regions) region.bufferOffset += offset;
    ToGeneral(uploadCb, image);
    vk.vkCmdCopyBufferToImage(uploadCb, ring, image.image, VK_IMAGE_LAYOUT_GENERAL,
                              levels, regions.data());
    uploadsRecorded = true;
    ++stats.textures;
    stats.textureUploadBytes += bytes;
    return &textures.emplace(key, image).first->second;
}

// The 24 lighting LUTs x 256 packed words as an R32_UINT image the
// generated fragment programs texelFetch (binding 13).
Ps4VkPicaRenderBackend::Impl::Image *Ps4VkPicaRenderBackend::Impl::GetLightingLut(
    const Oot3d::Renderer::PicaLightingLutView &view) {
    if (!view.Valid()) return nullptr;
    auto found = lightingLuts.find(view.ContentHash);
    if (found != lightingLuts.end()) return &found->second;
    const VkDeviceSize bytes = view.PackedEntries.size() * sizeof(uint32_t);
    if (!RingReserve(bytes + 16U)) return nullptr;
    Image image;
    if (!CreateImage(image, 256, 24, VK_FORMAT_R32_UINT,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, true)) {
        return nullptr;
    }
    const VkDeviceSize offset = RingAlloc(bytes, 16U);
    std::memcpy(ringPointer + offset, view.PackedEntries.data(), bytes);
    ToGeneral(uploadCb, image);
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {256, 24, 1};
    vk.vkCmdCopyBufferToImage(uploadCb, ring, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    uploadsRecorded = true;
    return &lightingLuts.emplace(view.ContentHash, image).first->second;
}

VkSampler Ps4VkPicaRenderBackend::Impl::GetSampler(const Oot3d::Renderer::PicaTextureView &view,
                                                   uint32_t levels) {
    const uint32_t minLevel = std::min<uint32_t>(view.MinMipLevel, levels - 1U);
    const uint32_t key = uint32_t{view.NativeWrapS} | (uint32_t{view.NativeWrapT} << 4U) |
                         (view.MagLinear ? 1U << 8U : 0U) | (view.MinLinear ? 1U << 9U : 0U) |
                         (view.MipLinear ? 1U << 10U : 0U) | (minLevel << 12U) |
                         ((levels - 1U) << 18U);
    auto found = samplers.find(key);
    if (found != samplers.end()) return found->second;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = view.MagLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = view.MinLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode =
        view.MipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = ToVkAddressMode(view.NativeWrapS);
    info.addressModeV = ToVkAddressMode(view.NativeWrapT);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // The generated programs pick the LOD themselves (textureLod with the
    // PICA bias); the sampler only clamps it to the guest's level range.
    info.minLod = static_cast<float>(minLevel);
    info.maxLod = static_cast<float>(levels - 1U);
    VkSampler sampler = VK_NULL_HANDLE;
    if (!Check(vk.vkCreateSampler(device, &info, nullptr, &sampler), "vkCreateSampler"))
        return nearestClamp;
    samplers.emplace(key, sampler);
    return sampler;
}

bool Ps4VkPicaRenderBackend::Impl::VertexFormatSupported(VkFormat format) {
    auto found = vertexFormatSupport.find(static_cast<uint32_t>(format));
    if (found != vertexFormatSupport.end()) return found->second;
    // Integer attributes (8/16-bit, scaled) go through the CPU conversion:
    // RADV's fetch of them on the PS4's GFX7 deforms skinned models (Link's
    // tunic and cap, 1.05 console test), the converted floats do not. The
    // conversion costs ~1% of the render thread. This file restores the
    // driver's fetch, for testing a fixed driver.
    static const bool forceConversion = [] {
        FILE *flag = std::fopen("/data/triaevum/native_vertex_fetch", "rb");
        if (flag != nullptr) std::fclose(flag);
        return flag == nullptr;
    }();
    VkFormatProperties properties{};
    vk.vkGetPhysicalDeviceFormatProperties(gpu, format, &properties);
    const bool isFloat = format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT ||
                         format == VK_FORMAT_R32G32B32_SFLOAT ||
                         format == VK_FORMAT_R32G32B32A32_SFLOAT;
    const bool supported =
        (properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0U &&
        (isFloat || !forceConversion);
    if (!supported) Log("vk-backend: vertex format %d - CPU conversion", format);
    vertexFormatSupport.emplace(static_cast<uint32_t>(format), supported);
    return supported;
}

// The desktop's per-draw raster canvas (gfx_vulkan_pica_draw.cpp): which
// guest-to-host mapping a draw rasterises with, from its composition domain
// and what a UI draw covering the frame shows.
Fast::Renderer3ds::PicaRasterCanvas Ps4VkPicaRenderBackend::Impl::RasterCanvas(
    const Oot3d::Renderer::PicaDrawView &draw, const Target &target) {
    using Fast::Renderer3ds::PicaCompositionDomain;
    const auto contains = [&](uint32_t address) {
        return std::find(sceneColorTargets.begin(), sceneColorTargets.end(), address) !=
               sceneColorTargets.end();
    };
    if (draw.CompositionDomain == PicaCompositionDomain::Scene &&
        draw.FramebufferColorPhysicalAddress != 0U &&
        !contains(draw.FramebufferColorPhysicalAddress)) {
        sceneColorTargets.push_back(draw.FramebufferColorPhysicalAddress);
    }
    Fast::Renderer3ds::PicaUiFrameContentEvidence evidence;
    evidence.CoversWholeTarget =
        draw.ViewportX <= 0.0F && draw.ViewportY <= 0.0F &&
        draw.ViewportX + draw.ViewportWidth >= static_cast<float>(draw.FramebufferWidth) &&
        draw.ViewportY + draw.ViewportHeight >= static_cast<float>(draw.FramebufferHeight);
    if (draw.CompositionDomain == PicaCompositionDomain::Ui && evidence.CoversWholeTarget &&
        contains(draw.FramebufferColorPhysicalAddress)) {
        evidence.SceneDrewOnTarget = true;
        evidence.Untextured = draw.Textures.empty();
        evidence.SamplesOnlyRenderedCopies =
            !draw.Textures.empty() &&
            std::all_of(draw.Textures.begin(), draw.Textures.end(),
                        [](const auto &texture) { return !texture.CopyReferences.empty(); });
        evidence.SamplesOnlyAlphaFreeTextures =
            !draw.Textures.empty() &&
            std::none_of(draw.Textures.begin(), draw.Textures.end(), [](const auto &texture) {
                return Fast::Renderer3ds::PicaTextureFormatCarriesAlpha(texture.NativeFormat);
            });
        if (draw.VertexUniformBytes.size() >= Fast::Oot3d::kPicaPackedVertexFloatOffset) {
            uint32_t booleanUniforms = 0;
            std::memcpy(&booleanUniforms,
                        draw.VertexUniformBytes.data() +
                            Fast::Oot3d::kPicaPackedVertexBooleanMaskOffset,
                        sizeof(booleanUniforms));
            evidence.GeometryCoversViewport = Fast::Renderer3ds::PicaDrawGeometryCoversViewport(
                draw, booleanUniforms,
                draw.VertexUniformBytes.subspan(Fast::Oot3d::kPicaPackedVertexFloatOffset));
        }
    }
    const auto content = Fast::Renderer3ds::ResolvePicaUiFrameContent(draw.CompositionDomain,
                                                                      evidence);
    // This target keeps the guest's own sampling along storage X.
    return Oot3d::Renderer::ResolvePicaRasterCanvas(
        content.Domain, content.Content, draw.FramebufferWidth, draw.FramebufferHeight,
        target.color.width, target.color.height, 1.0F);
}

// ================================================================ pipelines ==

namespace {

// Everything a PICA pipeline depends on, as POD so its bytes are the map key.
struct PipelineKey {
    VkShaderModule vertex;
    VkShaderModule fragment;
    uint8_t topology;
    uint8_t cullMode;
    uint8_t frontFace;
    uint8_t depthTest;
    uint8_t depthWrite;
    uint8_t depthCompare;
    uint8_t stencilEnable;
    uint8_t stencilFail;
    uint8_t stencilPass;
    uint8_t stencilDepthFail;
    uint8_t stencilCompare;
    uint8_t blendEnable;
    uint8_t blendOpRgb;
    uint8_t blendOpAlpha;
    uint8_t srcRgb;
    uint8_t dstRgb;
    uint8_t srcAlpha;
    uint8_t dstAlpha;
    uint8_t writeMask;
    uint8_t logicEnable;
    uint8_t logicOp;
    uint8_t bindingCount;
    uint8_t attributeCount;
    struct {
        uint32_t binding;
        uint32_t stride;
        uint32_t rate;
    } bindings[32];
    struct {
        uint32_t location;
        uint32_t binding;
        uint32_t format;
        uint32_t offset;
    } attributes[16];
};

}  // namespace

VkPipeline Ps4VkPicaRenderBackend::Impl::GetPipeline(
    const Oot3d::Renderer::PicaDrawView &draw, VkShaderModule vs, VkShaderModule fs,
    const std::vector<VkVertexInputBindingDescription> &bindings,
    const std::vector<VkVertexInputAttributeDescription> &attributes) {
    PipelineKey key;
    std::memset(&key, 0, sizeof key);
    key.vertex = vs;
    key.fragment = fs;
    key.topology = static_cast<uint8_t>(ToVkTopology(draw.Topology));
    // Desktop ToPicaSceneCullMode / ToPicaSceneFrontFace.
    key.cullMode = static_cast<uint8_t>(
        draw.CullMode == NativeCullMode::KeepAll
            ? VK_CULL_MODE_NONE
            : (draw.FramebufferFlipped ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT));
    key.frontFace = static_cast<uint8_t>(draw.CullMode == NativeCullMode::KeepCounterClockwise
                                             ? VK_FRONT_FACE_CLOCKWISE
                                             : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    key.depthTest = draw.DepthTestEnabled || draw.DepthWriteEnabled;
    key.depthWrite = draw.DepthWriteEnabled;
    key.depthCompare = static_cast<uint8_t>(
        draw.DepthTestEnabled ? ToVkCompare(draw.DepthCompare) : VK_COMPARE_OP_ALWAYS);
    key.stencilEnable = draw.Stencil.Enabled && depthHasStencil;
    if (key.stencilEnable) {
        key.stencilFail = static_cast<uint8_t>(ToVkStencil(draw.Stencil.Fail));
        key.stencilPass = static_cast<uint8_t>(ToVkStencil(draw.Stencil.Pass));
        key.stencilDepthFail = static_cast<uint8_t>(ToVkStencil(draw.Stencil.DepthFail));
        key.stencilCompare = static_cast<uint8_t>(ToVkCompare(draw.Stencil.Compare));
    }
    key.blendEnable = draw.Blend.Enabled;
    if (key.blendEnable) {
        key.blendOpRgb = static_cast<uint8_t>(ToVkBlendOp(draw.Blend.EquationRgb));
        key.blendOpAlpha = static_cast<uint8_t>(ToVkBlendOp(draw.Blend.EquationAlpha));
        key.srcRgb = static_cast<uint8_t>(ToVkBlendFactor(draw.Blend.SourceRgb));
        key.dstRgb = static_cast<uint8_t>(ToVkBlendFactor(draw.Blend.DestRgb));
        key.srcAlpha = static_cast<uint8_t>(ToVkBlendFactor(draw.Blend.SourceAlpha));
        key.dstAlpha = static_cast<uint8_t>(ToVkBlendFactor(draw.Blend.DestAlpha));
    } else if (logicOpSupported && draw.LogicOperation != PicaLogicOperation::Copy) {
        key.logicEnable = 1;
        key.logicOp = static_cast<uint8_t>(ToVkLogicOp(draw.LogicOperation));
    }
    key.writeMask = draw.ColorWriteMask & 0xFU;
    key.bindingCount = static_cast<uint8_t>(bindings.size());
    for (size_t index = 0; index < bindings.size() && index < 32U; ++index) {
        key.bindings[index] = {bindings[index].binding, bindings[index].stride,
                               static_cast<uint32_t>(bindings[index].inputRate)};
    }
    key.attributeCount = static_cast<uint8_t>(attributes.size());
    for (size_t index = 0; index < attributes.size() && index < 16U; ++index) {
        key.attributes[index] = {attributes[index].location, attributes[index].binding,
                                 static_cast<uint32_t>(attributes[index].format),
                                 attributes[index].offset};
    }
    std::string mapKey(reinterpret_cast<const char *>(&key), sizeof key);
    auto found = pipelines.find(mapKey);
    if (found != pipelines.end()) return found->second;

    const uint64_t start = NowUs();
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = static_cast<VkPrimitiveTopology>(key.topology);
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = key.cullMode;
    raster.frontFace = static_cast<VkFrontFace>(key.frontFace);
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = key.depthTest;
    depth.depthWriteEnable = key.depthWrite;
    depth.depthCompareOp = static_cast<VkCompareOp>(key.depthCompare);
    depth.stencilTestEnable = key.stencilEnable;
    depth.front.failOp = static_cast<VkStencilOp>(key.stencilFail);
    depth.front.passOp = static_cast<VkStencilOp>(key.stencilPass);
    depth.front.depthFailOp = static_cast<VkStencilOp>(key.stencilDepthFail);
    depth.front.compareOp = static_cast<VkCompareOp>(key.stencilCompare);
    depth.back = depth.front;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = key.blendEnable;
    blendAttachment.colorBlendOp = static_cast<VkBlendOp>(key.blendOpRgb);
    blendAttachment.alphaBlendOp = static_cast<VkBlendOp>(key.blendOpAlpha);
    blendAttachment.srcColorBlendFactor = static_cast<VkBlendFactor>(key.srcRgb);
    blendAttachment.dstColorBlendFactor = static_cast<VkBlendFactor>(key.dstRgb);
    blendAttachment.srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.srcAlpha);
    blendAttachment.dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.dstAlpha);
    blendAttachment.colorWriteMask = key.writeMask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.logicOpEnable = key.logicEnable;
    blend.logicOp = static_cast<VkLogicOp>(key.logicOp);
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
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
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = picaPipelineLayout;
    info.renderPass = picaPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!Check(vk.vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &pipeline),
               "vkCreateGraphicsPipelines")) {
        pipeline = VK_NULL_HANDLE;
    }
    const uint64_t elapsed = NowUs() - start;
    stats.pipelineCompileUs += elapsed;
    if (pipeline != VK_NULL_HANDLE) {
        ++stats.pipelines;
        pipelineCacheDirty = true;
        if (stats.pipelines <= 16U || elapsed > 20000U) {
            Log("vk-backend: pipeline #%llu in %llu us", (unsigned long long)stats.pipelines,
                (unsigned long long)elapsed);
        }
    }
    pipelines.emplace(std::move(mapKey), pipeline);
    return pipeline;
}

VkPipeline Ps4VkPicaRenderBackend::Impl::CreatePresentPipeline(bool overlay, VkShaderModule vs,
                                                              VkShaderModule fs) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = overlay ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = 0xFU;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
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
    info.layout = presentPipelineLayout;
    info.renderPass = scanoutPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!Check(vk.vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &pipeline),
               "vkCreateGraphicsPipelines(present)"))
        return VK_NULL_HANDLE;
    return pipeline;
}

// ==================================================================== draw ==

bool Ps4VkPicaRenderBackend::SubmitPicaDraw(const Oot3d::Renderer::PicaDrawView &draw,
                                            std::string *error) {
    (void)error;
    auto &impl = *mImpl;
    ++impl.stats.draws;
    const auto skip = [&](uint64_t *counter, const char *reason) {
        ++*counter;
        ++impl.stats.drawsSkipped;
        if (impl.skipLogs < 8) {
            ++impl.skipLogs;
            impl.Log("vk-backend: draw skipped (%s): src=%zu/%zu verts=%u bind=%zu attr=%zu "
                     "fragop=%u scissor=%u fb=%ux%u color=0x%08x",
                     reason, draw.VertexShaderSource.size(), draw.FragmentShaderSource.size(),
                     draw.VertexCount, draw.VertexBindings.size(), draw.VertexAttributes.size(),
                     draw.FragmentOperationMode, draw.ScissorMode, draw.FramebufferWidth,
                     draw.FramebufferHeight, draw.FramebufferColorPhysicalAddress);
        }
        return true;
    };
    if (draw.VertexShaderSource.empty() || draw.FragmentShaderSource.empty())
        return skip(&impl.stats.skipNoSource, "no-shader-source");
    if (draw.VertexCount == 0U || draw.VertexBindings.empty() || draw.VertexAttributes.empty())
        return skip(&impl.stats.skipNoGeometry, "no-geometry");
    if (draw.FragmentOperationMode != 0U)
        return skip(&impl.stats.skipFragOp, "fragment-operation-mode");
    if (draw.ScissorMode == 1U || draw.ScissorMode == 2U)
        return skip(&impl.stats.skipScissor, "scissor-mode");
    if (draw.FramebufferWidth == 0U || draw.FramebufferHeight == 0U ||
        draw.ViewportWidth <= 0.0F || draw.ViewportHeight <= 0.0F)
        return skip(&impl.stats.skipExtent, "zero-extent");

    impl.EnsureRecording();

    // Programs.
    Impl::Shader *vertex =
        impl.GetShader(true, draw.VertexShaderSourceIdentity, draw.VertexShaderSource);
    Impl::Shader *fragment =
        impl.GetShader(false, draw.FragmentShaderSourceIdentity, draw.FragmentShaderSource);
    if (vertex == nullptr || vertex->failed) return skip(&impl.stats.skipShader, "vertex-shader");
    const bool fallbackFragment = fragment == nullptr || fragment->failed;
    const VkShaderModule fragmentModule =
        fallbackFragment ? impl.fallbackFragment.module : fragment->module;
    const bool integerTexture0 = !fallbackFragment && fragment->integerTexture0;

    // Target (applies pending fills; may end the open render pass).
    const Impl::TargetKey targetKey{draw.RenderTargetNamespace,
                                    draw.FramebufferColorPhysicalAddress,
                                    draw.FramebufferDepthPhysicalAddress,
                                    draw.FramebufferWidth,
                                    draw.FramebufferHeight,
                                    draw.FramebufferColorFormat,
                                    draw.FramebufferDepthFormat};
    Impl::Target *target = impl.GetTarget(targetKey);
    if (target == nullptr) {
        ++impl.stats.drawFailures;
        return true;
    }

    // Textures (uploads land on the upload command buffer).
    VkImageView textureViews[3] = {impl.whiteImage.view, impl.whiteImage.view,
                                   impl.whiteImage.view};
    VkSampler textureSamplers[3] = {impl.nearestClamp, impl.nearestClamp, impl.nearestClamp};
    if (integerTexture0) textureViews[0] = impl.uintImage.view;
    for (const auto &texture : draw.Textures) {
        if (texture.Slot >= 3U || texture.NativeType != 0U) continue;
        if (texture.Slot == 0U && integerTexture0) continue;
        Impl::Image *image = impl.GetTexture(texture);
        if (image == nullptr) continue;
        textureViews[texture.Slot] = image->view;
        textureSamplers[texture.Slot] = impl.GetSampler(texture, image->mipLevels);
    }
    Impl::Image *lut = impl.GetLightingLut(draw.LightingLut);
    const VkImageView lutView = lut != nullptr ? lut->view : impl.uintImage.view;

    // Vertex input: draw bindings as they are; attributes whose format the
    // GPU cannot fetch get a float copy in a binding of their own (16+loc).
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    struct Conversion {
        const Oot3d::Renderer::PicaVertexAttributeView *attribute;
        const Oot3d::Renderer::PicaVertexBindingView *binding;
        uint32_t elements;
    };
    std::vector<Conversion> conversions;
    uint32_t usedBindings = 0;
    VkDeviceSize reserve = 0;
    for (const auto &attribute : draw.VertexAttributes) {
        if (attribute.Location >= 16U || attribute.Binding >= 16U ||
            attribute.ComponentCount == 0U || attribute.ComponentCount > 4U) {
            ++impl.stats.drawFailures;
            return true;
        }
        const auto bindingIt =
            std::find_if(draw.VertexBindings.begin(), draw.VertexBindings.end(),
                         [&](const auto &candidate) { return candidate.Binding == attribute.Binding; });
        if (bindingIt == draw.VertexBindings.end() || bindingIt->Bytes.empty() ||
            bindingIt->ByteStride == 0U) {
            ++impl.stats.drawFailures;
            return true;
        }
        const VkFormat format = ToVkVertexFormat(attribute.Format, attribute.ComponentCount);
        if (impl.VertexFormatSupported(format)) {
            attributes.push_back({attribute.Location, attribute.Binding, format,
                                  attribute.ByteOffset});
            usedBindings |= 1U << attribute.Binding;
            continue;
        }
        const uint32_t elementBytes =
            attribute.ByteOffset + VertexComponentBytes(attribute.Format) * attribute.ComponentCount;
        const uint32_t elements =
            bindingIt->Bytes.size() < elementBytes
                ? 0U
                : static_cast<uint32_t>((bindingIt->Bytes.size() - elementBytes) /
                                            bindingIt->ByteStride +
                                        1U);
        if (elements == 0U) {
            ++impl.stats.drawFailures;
            return true;
        }
        conversions.push_back({&attribute, &*bindingIt, elements});
        reserve += elements * 16U + 16U;
    }
    for (const auto &binding : draw.VertexBindings) {
        if ((usedBindings & (1U << binding.Binding)) == 0U) continue;
        bindings.push_back({binding.Binding, binding.ByteStride,
                            binding.PerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                : VK_VERTEX_INPUT_RATE_VERTEX});
        reserve += binding.Bytes.size() + 16U;
    }
    uint32_t usedLocations = 0;
    uint32_t usedBindingSlots = usedBindings;
    for (const auto &attribute : attributes) usedLocations |= 1U << attribute.location;
    for (const auto &conversion : conversions) {
        const uint32_t binding = 16U + conversion.attribute->Location;
        bindings.push_back({binding, 16U,
                            conversion.binding->PerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                            : VK_VERTEX_INPUT_RATE_VERTEX});
        attributes.push_back({conversion.attribute->Location, binding,
                              VK_FORMAT_R32G32B32A32_SFLOAT, 0U});
        usedLocations |= 1U << conversion.attribute->Location;
        usedBindingSlots |= 1U << binding;
    }
    // The generated vertex programs declare all sixteen input registers;
    // Vulkan needs an attribute behind each (validation: Input-07904 - the
    // desktop leans on driver tolerance). Unconfigured registers read the
    // PICA default (0,0,0,1) from one per-instance element.
    uint32_t defaultBinding = UINT32_MAX;
    if (usedLocations != 0xFFFFU) {
        for (uint32_t candidate = 31; candidate > 0; --candidate) {
            if ((usedBindingSlots & (1U << candidate)) == 0U) {
                defaultBinding = candidate;
                break;
            }
        }
        if (defaultBinding == UINT32_MAX) {
            ++impl.stats.drawFailures;
            return true;
        }
        bindings.push_back({defaultBinding, 16U, VK_VERTEX_INPUT_RATE_INSTANCE});
        for (uint32_t location = 0; location < 16U; ++location) {
            if ((usedLocations & (1U << location)) == 0U) {
                attributes.push_back(
                    {location, defaultBinding, VK_FORMAT_R32G32B32A32_SFLOAT, 0U});
            }
        }
        reserve += 32U;
    }
    const VkDeviceSize uboAlign =
        std::max<VkDeviceSize>(16U, impl.properties.limits.minUniformBufferOffsetAlignment);
    const VkDeviceSize vertexUniformBytes =
        std::max<VkDeviceSize>(16U, (draw.VertexUniformBytes.size() + 15U) / 16U * 16U);
    const VkDeviceSize fragmentUniformBytes =
        std::max<VkDeviceSize>(16U, (draw.FragmentUniformBytes.size() + 15U) / 16U * 16U);
    reserve += vertexUniformBytes + fragmentUniformBytes + 2U * uboAlign;
    const size_t indexSize = draw.IndicesAre16Bit ? 2U : 1U;
    size_t indexCount = 0;
    if (draw.Indexed) {
        if (draw.IndexBytes.empty() || draw.IndexBytes.size() % indexSize != 0U) {
            ++impl.stats.drawFailures;
            return true;
        }
        indexCount = draw.IndexBytes.size() / indexSize;
        reserve += indexCount * 2U + 16U;
    }

    VkPipeline pipeline =
        impl.GetPipeline(draw, vertex->module, fragmentModule, bindings, attributes);
    if (pipeline == VK_NULL_HANDLE) return skip(&impl.stats.skipPipeline, "pipeline");

    // Per-draw data: one reservation (a flush recycles the ring, so nothing
    // allocated before it may survive into this draw's recording).
    if (impl.setsAllocated + 1U >= kMaxSetsPerFlush) impl.Flush();
    if (!impl.RingReserve(reserve)) {
        ++impl.stats.drawFailures;
        impl.NoteError("draw data (%llu bytes) exceeds the ring", (unsigned long long)reserve);
        return true;
    }

    uint32_t bindingOffsets[48] = {};
    for (const auto &binding : draw.VertexBindings) {
        if ((usedBindings & (1U << binding.Binding)) == 0U) continue;
        const VkDeviceSize offset = impl.RingAlloc(binding.Bytes.size(), 16U);
        std::memcpy(impl.ringPointer + offset, binding.Bytes.data(), binding.Bytes.size());
        bindingOffsets[binding.Binding] = static_cast<uint32_t>(offset);
    }
    for (const auto &conversion : conversions) {
        const auto &attribute = *conversion.attribute;
        const auto &binding = *conversion.binding;
        const VkDeviceSize offset = impl.RingAlloc(conversion.elements * 16U, 16U);
        float *out = reinterpret_cast<float *>(impl.ringPointer + offset);
        const uint32_t componentBytes = VertexComponentBytes(attribute.Format);
        for (uint32_t element = 0; element < conversion.elements; ++element) {
            const uint8_t *source =
                binding.Bytes.data() + size_t{element} * binding.ByteStride + attribute.ByteOffset;
            float values[4] = {0.0F, 0.0F, 0.0F, 1.0F};
            for (uint32_t component = 0; component < attribute.ComponentCount; ++component) {
                values[component] =
                    ReadVertexComponent(attribute.Format, source + component * componentBytes);
            }
            std::memcpy(out + size_t{element} * 4U, values, sizeof values);
        }
        bindingOffsets[16U + attribute.Location] = static_cast<uint32_t>(offset);
    }
    if (defaultBinding != UINT32_MAX) {
        const VkDeviceSize offset = impl.RingAlloc(16U, 16U);
        const float defaults[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        std::memcpy(impl.ringPointer + offset, defaults, sizeof defaults);
        bindingOffsets[defaultBinding] = static_cast<uint32_t>(offset);
    }
    const VkDeviceSize vertexUbo = impl.RingAlloc(vertexUniformBytes, uboAlign);
    std::memset(impl.ringPointer + vertexUbo, 0, vertexUniformBytes);
    std::memcpy(impl.ringPointer + vertexUbo, draw.VertexUniformBytes.data(),
                draw.VertexUniformBytes.size());
    const VkDeviceSize fragmentUbo = impl.RingAlloc(fragmentUniformBytes, uboAlign);
    std::memset(impl.ringPointer + fragmentUbo, 0, fragmentUniformBytes);
    std::memcpy(impl.ringPointer + fragmentUbo, draw.FragmentUniformBytes.data(),
                draw.FragmentUniformBytes.size());
    VkDeviceSize indexOffset = 0;
    if (draw.Indexed) {
        indexOffset = impl.RingAlloc(indexCount * 2U, 16U);
        if (draw.IndicesAre16Bit) {
            std::memcpy(impl.ringPointer + indexOffset, draw.IndexBytes.data(), indexCount * 2U);
        } else {
            // No VK_EXT_index_type_uint8 on this path: widen 8-bit indices.
            uint16_t *out = reinterpret_cast<uint16_t *>(impl.ringPointer + indexOffset);
            for (size_t index = 0; index < indexCount; ++index) out[index] = draw.IndexBytes[index];
        }
    }

    // Descriptors.
    VkDescriptorSet set = impl.AllocateSet(impl.picaSetLayout);
    if (set == VK_NULL_HANDLE) {
        ++impl.stats.drawFailures;
        return true;
    }
    const VkDescriptorBufferInfo uboInfos[2] = {{impl.ring, vertexUbo, vertexUniformBytes},
                                                {impl.ring, fragmentUbo, fragmentUniformBytes}};
    VkDescriptorImageInfo imageInfos[5] = {};
    for (size_t slot = 0; slot < 3; ++slot) {
        imageInfos[slot] = {textureSamplers[slot], textureViews[slot], VK_IMAGE_LAYOUT_GENERAL};
    }
    if (integerTexture0) imageInfos[0].sampler = impl.nearestClamp;
    imageInfos[3] = {VK_NULL_HANDLE, impl.uintImage.view, VK_IMAGE_LAYOUT_GENERAL};
    imageInfos[4] = {impl.nearestClamp, lutView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[7] = {};
    for (auto &write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.descriptorCount = 1;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &uboInfos[0];
    for (uint32_t slot = 0; slot < 3; ++slot) {
        writes[1 + slot].dstBinding = 1 + slot;
        writes[1 + slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1 + slot].pImageInfo = &imageInfos[slot];
    }
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[4].pBufferInfo = &uboInfos[1];
    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[5].pImageInfo = &imageInfos[3];
    writes[6].dstBinding = kLightingLutBinding;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = &imageInfos[4];
    impl.vk.vkUpdateDescriptorSets(impl.device, 7, writes, 0, nullptr);

    // Record.
    impl.BeginTargetPass(*target);
    const VkCommandBuffer cb = impl.mainCb;
    impl.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    impl.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.picaPipelineLayout,
                                    0, 1, &set, 0, nullptr);
    for (const auto &binding : bindings) {
        const VkDeviceSize offset = bindingOffsets[binding.binding];
        impl.vk.vkCmdBindVertexBuffers(cb, binding.binding, 1, &impl.ring, &offset);
    }
    // Desktop viewport math (gfx_vulkan_pica_draw.cpp): the guest numbers
    // through the draw's raster canvas. The scene fills the host target
    // (wider than native in widescreen); placed UI keeps the native canvas,
    // centred; UI that covers the frame takes the scene's mapping, or the
    // native canvas when it is a picture of its own.
    const auto canvas = impl.RasterCanvas(draw, *target);
    const VkViewport viewport = {canvas.X + draw.ViewportX * canvas.ScaleX,
                                 canvas.Y + draw.ViewportY * canvas.ScaleY,
                                 draw.ViewportWidth * canvas.ScaleX,
                                 draw.ViewportHeight * canvas.ScaleY, 0.0F, 1.0F};
    impl.vk.vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {target->color.width, target->color.height}};
    if (draw.ScissorMode == 3U) {
        const uint32_t x1 = std::min<uint32_t>(draw.ScissorX1, draw.FramebufferWidth);
        const uint32_t y1 = std::min<uint32_t>(draw.ScissorY1, draw.FramebufferHeight);
        const uint32_t x2 = std::min<uint32_t>(uint32_t{draw.ScissorX2} + 1U, draw.FramebufferWidth);
        const uint32_t y2 =
            std::min<uint32_t>(uint32_t{draw.ScissorY2} + 1U, draw.FramebufferHeight);
        const int32_t left = std::max(0, static_cast<int32_t>(canvas.X + x1 * canvas.ScaleX));
        const int32_t top = std::max(0, static_cast<int32_t>(canvas.Y + y1 * canvas.ScaleY));
        scissor = {{left, top},
                   {x2 > x1 ? static_cast<uint32_t>((x2 - x1) * canvas.ScaleX) : 0U,
                    y2 > y1 ? static_cast<uint32_t>((y2 - y1) * canvas.ScaleY) : 0U}};
    }
    impl.vk.vkCmdSetScissor(cb, 0, 1, &scissor);
    impl.vk.vkCmdSetBlendConstants(cb, draw.Blend.ConstantColor);
    impl.vk.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, draw.Stencil.CompareMask);
    impl.vk.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, draw.Stencil.WriteMask);
    impl.vk.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, draw.Stencil.Reference);
    if (draw.Indexed) {
        impl.vk.vkCmdBindIndexBuffer(cb, impl.ring, indexOffset, VK_INDEX_TYPE_UINT16);
        impl.vk.vkCmdDrawIndexed(cb, static_cast<uint32_t>(indexCount), 1, 0, draw.BaseVertex, 0);
    } else {
        impl.vk.vkCmdDraw(cb, draw.VertexCount, 1, 0, 0);
    }
    impl.workRecorded = true;
    return true;
}

// ======================================================== display transfer ==

bool Ps4VkPicaRenderBackend::SubmitPicaDisplayTransfer(
    const Oot3d::Renderer::PicaDisplayTransferView &transfer, std::string *error) {
    (void)error;
    auto &impl = *mImpl;
    ++impl.stats.transfers;
    if (transfer.InputPhysicalAddress == 0U || transfer.OutputPhysicalAddress == 0U ||
        transfer.InputWidth == 0U || transfer.InputHeight == 0U || transfer.OutputWidth == 0U ||
        transfer.OutputHeight == 0U) {
        ++impl.stats.transferFailures;
        return true;
    }
    const uint32_t scaling = (transfer.Flags >> 24U) & 3U;
    if ((transfer.Flags & 8U) != 0U || (transfer.Flags & 0x10000U) != 0U || scaling > 2U) {
        ++impl.stats.transferFailures;
        return true;
    }
    impl.EnsureRecording();

    // Source: the most recently used target at the input address.
    Impl::Target *source = nullptr;
    const Impl::TargetKey *sourceKey = nullptr;
    for (auto &entry : impl.targets) {
        if (entry.first.namespace_ != transfer.RenderTargetNamespace ||
            entry.first.colorAddress != transfer.InputPhysicalAddress)
            continue;
        if (source == nullptr || entry.second.lastUse > source->lastUse) {
            source = &entry.second;
            sourceKey = &entry.first;
        }
    }
    if (source == nullptr) {
        ++impl.stats.transferFailures;
        ++impl.missingSourceLogs;
        if (impl.missingSourceLogs <= 5 || impl.missingSourceLogs % 500 == 0) {
            impl.NoteError("display transfer source missing (#%d): ns=%llu addr=0x%08x",
                           impl.missingSourceLogs,
                           (unsigned long long)transfer.RenderTargetNamespace,
                           transfer.InputPhysicalAddress);
        }
        return true;
    }
    impl.ApplyFillsTo(*sourceKey, *source);
    const uint32_t sourceWidth = sourceKey->width;
    const uint32_t sourceHeight = sourceKey->height;
    if (transfer.InputWidth > sourceWidth || transfer.InputHeight > sourceHeight) {
        ++impl.stats.transferFailures;
        return true;
    }
    const uint32_t outputWidth =
        transfer.TextureCopyBytes != 0U
            ? transfer.OutputWidth
            : std::max<uint32_t>(1U, static_cast<uint32_t>(
                                         (uint64_t{transfer.OutputWidth} * sourceWidth +
                                          transfer.InputWidth / 2U) /
                                         transfer.InputWidth));
    const uint32_t outputHeight =
        transfer.TextureCopyBytes != 0U
            ? transfer.OutputHeight
            : std::max<uint32_t>(1U, static_cast<uint32_t>(
                                         (uint64_t{transfer.OutputHeight} * sourceHeight +
                                          transfer.InputHeight / 2U) /
                                         transfer.InputHeight));
    const uint32_t horizontalSamples = scaling != 0U ? 2U : 1U;
    const uint32_t verticalSamples = scaling == 2U ? 2U : 1U;
    if (uint64_t{outputWidth} * horizontalSamples > sourceWidth ||
        uint64_t{outputHeight} * verticalSamples > sourceHeight) {
        ++impl.stats.transferFailures;
        return true;
    }

    // Destination display image (output-address keyed, recreated when the
    // extent changes - the desktop cache).
    impl.EndRenderPass();
    // The display image keeps its source's host density (the copy reads
    // horizontalSamples x verticalSamples guest pixels per output pixel).
    const uint32_t displayWidth =
        std::max<uint32_t>(1U, static_cast<uint32_t>(std::lround(outputWidth * source->scaleX)));
    const uint32_t displayHeight =
        std::max<uint32_t>(1U, static_cast<uint32_t>(std::lround(outputHeight * source->scaleY)));
    const auto displayKey =
        std::make_pair(transfer.RenderTargetNamespace, transfer.OutputPhysicalAddress);
    auto found = impl.displays.find(displayKey);
    if (found != impl.displays.end() && (found->second.logicalWidth != outputWidth ||
                                         found->second.logicalHeight != outputHeight ||
                                         found->second.color.width != displayWidth ||
                                         found->second.color.height != displayHeight)) {
        // The old image may still be referenced by recorded work and by the
        // per-screen presentation records.
        impl.Flush();
        for (auto &screen : impl.screens) {
            if (screen.display == &found->second) screen = Impl::ScreenRecord{};
        }
        impl.DestroyImage(found->second.color);
        impl.displays.erase(found);
        found = impl.displays.end();
    }
    if (found == impl.displays.end()) {
        Impl::Display display;
        display.logicalWidth = outputWidth;
        display.logicalHeight = outputHeight;
        if (!impl.CreateImage(display.color, displayWidth, displayHeight,
                              VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, false)) {
            ++impl.stats.transferFailures;
            return true;
        }
        impl.ToGeneral(impl.mainCb, display.color);
        found = impl.displays.emplace(displayKey, display).first;
        impl.Log("vk-backend: display image %ux%u at 0x%08x (from %ux%u, scaling %u)",
                 outputWidth, outputHeight, transfer.OutputPhysicalAddress, sourceWidth,
                 sourceHeight, scaling);
    }
    Impl::Display &display = found->second;

    impl.FullBarrier(impl.mainCb);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {
        static_cast<int32_t>(std::lround(outputWidth * horizontalSamples * source->scaleX)),
        static_cast<int32_t>(std::lround(outputHeight * verticalSamples * source->scaleY)), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(displayWidth), static_cast<int32_t>(displayHeight),
                          1};
    impl.vk.vkCmdBlitImage(impl.mainCb, source->color.image, VK_IMAGE_LAYOUT_GENERAL,
                           display.color.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                           scaling != 0U ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    impl.FullBarrier(impl.mainCb);
    impl.workRecorded = true;

    // Raw copies owe the guest its bytes back (guest-side sampling of the
    // copy and CPU readers must see the GPU result).
    if (transfer.MaterializeGuest && transfer.TextureCopyBytes != 0U) {
        impl.MaterializeRawCopy(*sourceKey, *source, transfer);
    }
    if (transfer.Present) impl.PresentDisplay(display, transfer);
    return true;
}

void Ps4VkPicaRenderBackend::Impl::MaterializeRawCopy(
    const TargetKey &key, Target &source,
    const Fast::Renderer3ds::PicaDisplayTransferView &transfer) {
    const uint32_t imageWidth = source.color.width;
    const uint32_t imageHeight = source.color.height;
    const VkDeviceSize bytes = VkDeviceSize{imageWidth} * imageHeight * 4U;
    if (readbackBytes < bytes) {
        // Grow (the old buffer is idle: every readback waits for its flush).
        readbackBytes = std::max<VkDeviceSize>(bytes, 4ULL << 20);
        if (!CreateBuffer(readbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &readback, &readbackMemory,
                          &readbackPointer)) {
            readbackBytes = 0;
            return;
        }
    }
    EndRenderPass();
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {imageWidth, imageHeight, 1};
    vk.vkCmdCopyImageToBuffer(mainCb, source.color.image, VK_IMAGE_LAYOUT_GENERAL, readback, 1,
                              &copy);
    FullBarrier(mainCb);
    workRecorded = true;
    Flush();
    if (owner->mPhysicalMemoryCommit == nullptr) return;
    try {
        const auto encoded = Fast::Renderer3ds::EncodePicaFramebuffer(
            std::span<const uint8_t>(readbackPointer, bytes), imageWidth, imageHeight, key.width,
            key.height, key.colorFormat);
        Fast::Renderer3ds::PicaPhysicalMemoryWrite write;
        write.Address = transfer.OutputPhysicalAddress;
        const size_t count = std::min<size_t>(transfer.TextureCopyBytes, encoded.size());
        write.Bytes.assign(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(count));
        ++stats.materializations;
        if (!owner->mPhysicalMemoryCommit(
                std::span<const Fast::Renderer3ds::PicaPhysicalMemoryWrite>(&write, 1))) {
            NoteError("guest materialization commit failed at 0x%08x", write.Address);
        }
    } catch (const std::exception &exception) {
        NoteError("raw copy encode failed: %s", exception.what());
    }
}

// A presented transfer only records which display image its CTR screen
// shows (the top screen is the 400-line portrait image, the bottom one the
// 320-line image); EndFrame composes the screens into the scanout with the
// chosen layout, so a frame that refreshes one screen still shows both.
void Ps4VkPicaRenderBackend::Impl::PresentDisplay(
    Display &display, const Fast::Renderer3ds::PicaDisplayTransferView &view) {
    const uint32_t longSide = std::max(display.logicalWidth, display.logicalHeight);
    const uint32_t screen = longSide > 320U ? 0U : 1U;
    screens[screen].display = &display;
    screens[screen].flags = view.Flags;
    screens[screen].overlay =
        view.PresentationMode == Fast::Renderer3ds::PicaPresentationMode::AlphaOverlay;
    if (screen == 0U || options.layout != ScreenLayout::TopOnly) presentedThisFrame = true;
    ++stats.presents;
    if (immediatePresent && screen == 0U && frameActive) {
        // The landscape top screen, fitted at its host proportions (16:9 in
        // widescreen, the native 5:3 otherwise).
        const auto viewport = Oot3d::Renderer::FitUiPresentationViewport(
            scanoutWidth, scanoutHeight, static_cast<float>(display.color.height),
            static_cast<float>(display.color.width));
        OpenScanoutPass();
        DrawScreen(screens[0], static_cast<int>(viewport.X), static_cast<int>(viewport.Y),
                   static_cast<int>(viewport.Width), static_cast<int>(viewport.Height));
        workRecorded = true;
    }
}

void Ps4VkPicaRenderBackend::Impl::OpenScanoutPass() {
    if (scanoutPassActive) return;
    EndRenderPass();
    FullBarrier(mainCb);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = scanoutPass;
    begin.framebuffer = scanouts[frameScanout].framebuffer;
    begin.renderArea = {{0, 0}, {scanoutWidth, scanoutHeight}};
    vk.vkCmdBeginRenderPass(mainCb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    scanoutPassActive = true;
    workRecorded = true;
}

// Desktop scanout math (gfx_opengl_pica.cpp, kept by the GLES2 backend): the
// display image is in the CTR portrait layout; the fragment transposes it and
// applies the transfer's flip.
void Ps4VkPicaRenderBackend::Impl::DrawScreen(const ScreenRecord &record, int x, int y,
                                              int width, int height) {
    VkDescriptorSet set = AllocateSet(presentSetLayout);
    if (set == VK_NULL_HANDLE || record.display == nullptr) return;
    const VkDescriptorImageInfo imageInfo = {options.linearFilter ? linearClamp : nearestClamp,
                                             record.display->color.view,
                                             VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vk.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    const VkViewport viewport = {static_cast<float>(x),     static_cast<float>(y),
                                 static_cast<float>(width), static_cast<float>(height),
                                 0.0F,                      1.0F};
    const int32_t x0 = std::max(0, x);
    const int32_t y0 = std::max(0, y);
    const int32_t x1 = std::min(static_cast<int>(scanoutWidth), x + width);
    const int32_t y1 = std::min(static_cast<int>(scanoutHeight), y + height);
    if (x1 <= x0 || y1 <= y0) return;
    const VkRect2D scissor = {{x0, y0},
                              {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
    const bool transferInputLinear = (record.flags & 2U) != 0U;
    const bool dontSwizzle = (record.flags & 0x20U) != 0U;
    const int32_t flip =
        ((!transferInputLinear != (transferInputLinear != dontSwizzle)) ^ ((record.flags & 1U) != 0U))
            ? 1
            : 0;
    vk.vkCmdBindPipeline(mainCb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         record.overlay ? presentOverlay : presentReplace);
    vk.vkCmdBindDescriptorSets(mainCb, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipelineLayout, 0,
                               1, &set, 0, nullptr);
    vk.vkCmdPushConstants(mainCb, presentPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                          sizeof flip, &flip);
    vk.vkCmdSetViewport(mainCb, 0, 1, &viewport);
    vk.vkCmdSetScissor(mainCb, 0, 1, &scissor);
    vk.vkCmdDraw(mainCb, 3, 1, 0, 0);
}

void Ps4VkPicaRenderBackend::Impl::ComposeScanout() {
    EndRenderPass();
    FullBarrier(mainCb);
    Scanout &scanout = scanouts[frameScanout];
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = scanoutPass;
    begin.framebuffer = scanout.framebuffer;
    begin.renderArea = {{0, 0}, {scanoutWidth, scanoutHeight}};
    vk.vkCmdBeginRenderPass(mainCb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    scanoutPassActive = true;
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear.clearValue.color.float32[0] = 0.0F;
    clear.clearValue.color.float32[1] = 0.0F;
    clear.clearValue.color.float32[2] = 0.0F;
    clear.clearValue.color.float32[3] = 1.0F;
    const VkClearRect clearRect = {{{0, 0}, {scanoutWidth, scanoutHeight}}, 0, 1};
    vk.vkCmdClearAttachments(mainCb, 1, &clear, 1, &clearRect);

    // Landscape (on-screen) extent of a screen: the transposed portrait image.
    const auto landscape = [](const ScreenRecord &record, float *w, float *h) {
        *w = static_cast<float>(record.display->logicalHeight);
        *h = static_cast<float>(record.display->logicalWidth);
    };
    const float outW = static_cast<float>(scanoutWidth);
    const float outH = static_cast<float>(scanoutHeight);
    const auto fit = [&](float w, float h, float boxX, float boxY, float boxW, float boxH,
                         bool stretch, int *x, int *y, int *width, int *height) {
        float drawW = boxW;
        float drawH = boxH;
        if (!stretch) {
            const float scale = std::min(boxW / w, boxH / h);
            drawW = w * scale;
            drawH = h * scale;
        }
        *width = std::max(1, static_cast<int>(std::lround(drawW)));
        *height = std::max(1, static_cast<int>(std::lround(drawH)));
        *x = static_cast<int>(std::lround(boxX + (boxW - drawW) / 2.0F));
        *y = static_cast<int>(std::lround(boxY + (boxH - drawH) / 2.0F));
    };
    const ScreenRecord &top = screens[0];
    const ScreenRecord &bottom = screens[1];
    int x = 0, y = 0, w = 0, h = 0;
    float topW = 400.0F, topH = 240.0F, bottomW = 320.0F, bottomH = 240.0F;
    if (top.display != nullptr) landscape(top, &topW, &topH);
    if (bottom.display != nullptr) landscape(bottom, &bottomW, &bottomH);
    switch (options.layout) {
        case ScreenLayout::TopOnly:
            if (top.display != nullptr) {
                fit(topW, topH, 0.0F, 0.0F, outW, outH, options.stretch, &x, &y, &w, &h);
                DrawScreen(top, x, y, w, h);
            }
            break;
        case ScreenLayout::SideBySide: {
            // Both screens at one common scale, side by side, centred.
            const float scale = std::min(outW / (topW + bottomW), outH / std::max(topH, bottomH));
            const float left = (outW - (topW + bottomW) * scale) / 2.0F;
            if (top.display != nullptr) {
                fit(topW, topH, left, 0.0F, topW * scale, outH, false, &x, &y, &w, &h);
                DrawScreen(top, x, y, w, h);
            }
            if (bottom.display != nullptr) {
                fit(bottomW, bottomH, left + topW * scale, 0.0F, bottomW * scale, outH, false, &x,
                    &y, &w, &h);
                DrawScreen(bottom, x, y, w, h);
            }
            break;
        }
        case ScreenLayout::BottomInCorner:
            if (top.display != nullptr) {
                fit(topW, topH, 0.0F, 0.0F, outW, outH, options.stretch, &x, &y, &w, &h);
                DrawScreen(top, x, y, w, h);
            }
            if (bottom.display != nullptr) {
                // A third of the output height, bottom-right corner.
                const float cornerH = outH / 3.0F;
                const float cornerW = cornerH * bottomW / bottomH;
                fit(bottomW, bottomH, outW - cornerW - 16.0F, outH - cornerH - 16.0F, cornerW,
                    cornerH, false, &x, &y, &w, &h);
                DrawScreen(bottom, x, y, w, h);
            }
            break;
    }
    EndRenderPass();
    FullBarrier(mainCb);
    workRecorded = true;
}

// ============================================ fills / clears / completions ==

bool Ps4VkPicaRenderBackend::ClearPicaRenderTarget(uint64_t renderTargetNamespace,
                                                   uint32_t colorPhysicalAddress,
                                                   std::string *error) {
    (void)error;
    auto &impl = *mImpl;
    impl.EnsureRecording();
    for (auto &entry : impl.targets) {
        if (entry.first.namespace_ != renderTargetNamespace ||
            entry.first.colorAddress != colorPhysicalAddress)
            continue;
        impl.EndRenderPass();
        impl.FullBarrier(impl.mainCb);
        const VkClearColorValue black{};
        const VkClearDepthStencilValue far = {1.0F, 0U};
        const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageSubresourceRange depthRange = {entry.second.depth.aspect, 0, 1, 0, 1};
        impl.vk.vkCmdClearColorImage(impl.mainCb, entry.second.color.image,
                                     VK_IMAGE_LAYOUT_GENERAL, &black, 1, &colorRange);
        impl.vk.vkCmdClearDepthStencilImage(impl.mainCb, entry.second.depth.image,
                                            VK_IMAGE_LAYOUT_GENERAL, &far, 1, &depthRange);
        impl.FullBarrier(impl.mainCb);
        impl.workRecorded = true;
        ++impl.stats.clears;
    }
    return true;
}

bool Ps4VkPicaRenderBackend::SubmitPicaMemoryFill(const Oot3d::Renderer::PicaMemoryFillView &fill,
                                                  std::string *error) {
    (void)error;
    auto &impl = *mImpl;
    ++impl.stats.fills;
    if (fill.StartPhysicalAddress == 0U || fill.EndPhysicalAddress <= fill.StartPhysicalAddress ||
        (fill.Control & 1U) == 0U) {
        return true;
    }
    // Recorded against the targets (applied before their next use) ...
    const Impl::PendingFill pending{fill.RenderTargetNamespace, fill.StartPhysicalAddress,
                                    fill.EndPhysicalAddress, fill.Value, fill.Control};
    auto previous = std::find_if(impl.pendingFills.begin(), impl.pendingFills.end(),
                                 [&](const Impl::PendingFill &entry) {
                                     return entry.namespace_ == pending.namespace_ &&
                                            entry.start == pending.start &&
                                            entry.end == pending.end;
                                 });
    if (previous != impl.pendingFills.end()) {
        *previous = pending;
    } else {
        impl.pendingFills.push_back(pending);
    }
    // ... and written to guest RAM straight away so CPU readers (LCD force
    // black, guest sampling of cleared ranges) agree with the GPU view.
    impl.GuestFill(fill);
    return true;
}

void Ps4VkPicaRenderBackend::Impl::GuestFill(const Fast::Renderer3ds::PicaMemoryFillView &fill) {
    if (owner->mPhysicalMemoryCommit == nullptr) return;
    const uint32_t elementSize = FillElementSize(fill.Control);
    uint8_t pattern[4] = {};
    for (uint32_t i = 0; i < elementSize; ++i) pattern[i] = static_cast<uint8_t>(fill.Value >> (i * 8U));
    uint32_t address = fill.StartPhysicalAddress;
    constexpr uint32_t kChunk = 256U * 1024U;
    while (address < fill.EndPhysicalAddress) {
        const uint32_t span = std::min<uint32_t>(kChunk, fill.EndPhysicalAddress - address);
        Fast::Renderer3ds::PicaPhysicalMemoryWrite write;
        write.Address = address;
        write.Bytes.resize(span);
        const uint32_t phase = (address - fill.StartPhysicalAddress) % elementSize;
        for (uint32_t i = 0; i < span; ++i) write.Bytes[i] = pattern[(phase + i) % elementSize];
        if (!owner->mPhysicalMemoryCommit(
                std::span<const Fast::Renderer3ds::PicaPhysicalMemoryWrite>(&write, 1))) {
            NoteError("guest fill commit failed at 0x%08x", address);
            return;
        }
        address += span;
    }
}

bool Ps4VkPicaRenderBackend::QueuePicaCompletion(uint64_t completionId, std::string *error) {
    if (completionId == 0U) {
        if (error != nullptr) *error = "PICA completion identity is invalid";
        return false;
    }
    mImpl->completions.push_back(completionId);
    return true;
}

std::vector<uint64_t> Ps4VkPicaRenderBackend::TakePicaCompletions() {
    auto result = std::move(mImpl->completions);
    mImpl->completions.clear();
    mImpl->stats.completions += result.size();
    return result;
}

void Ps4VkPicaRenderBackend::Impl::DestroyTargetsAndDisplays() {
    for (auto &entry : targets) {
        vk.vkDestroyFramebuffer(device, entry.second.framebuffer, nullptr);
        DestroyImage(entry.second.color);
        DestroyImage(entry.second.depth);
    }
    targets.clear();
    activeTarget = nullptr;
    for (auto &entry : displays) DestroyImage(entry.second.color);
    displays.clear();
    screens[0] = ScreenRecord{};
    screens[1] = ScreenRecord{};
}

void Ps4VkPicaRenderBackend::Impl::DestroyResources() {
    DestroyTargetsAndDisplays();
    for (auto &entry : textures) DestroyImage(entry.second);
    textures.clear();
    for (auto &entry : lightingLuts) DestroyImage(entry.second);
    lightingLuts.clear();
    FreePools();
}

bool Ps4VkPicaRenderBackend::ResetPicaState(std::string *error) {
    (void)error;
    auto &impl = *mImpl;
    impl.Flush();
    impl.vk.vkDeviceWaitIdle(impl.device);
    impl.DestroyResources();
    impl.pendingFills.clear();
    impl.completions.clear();
    impl.Log("vk-backend: PICA state reset");
    return true;
}

// ==================================================================== frame ==

void Ps4VkPicaRenderBackend::BeginFrame(uint32_t scanoutIndex) {
    auto &impl = *mImpl;
    impl.EnsureRecording();
    const float expansion =
        impl.options.outputAspect > Fast::Oot3d::kNativeTopScreenAspect
            ? impl.options.outputAspect / Fast::Oot3d::kNativeTopScreenAspect
            : 1.0F;
    if (impl.options.renderScale != impl.renderScale ||
        std::abs(expansion - impl.topScreenExpansion) > 0.0005F) {
        // Targets are redrawn every frame; dropping them costs one frame of
        // retained content (incrementally drawn UI surfaces repaint soon).
        impl.Flush();
        impl.DestroyTargetsAndDisplays();
        impl.Log("vk-backend: render scale %u -> %u, top screen width x%.3f", impl.renderScale,
                 impl.options.renderScale, expansion);
        impl.renderScale = impl.options.renderScale;
        impl.topScreenExpansion = expansion;
    }
    impl.sceneColorTargets.clear();
    impl.frameActive = true;
    impl.frameScanout = scanoutIndex % kScanoutCount;
    impl.presentedThisFrame = false;
}

bool Ps4VkPicaRenderBackend::EndFrame() {
    auto &impl = *mImpl;
    if (impl.presentedThisFrame && !impl.immediatePresent) impl.ComposeScanout();
    impl.Flush();
    impl.frameActive = false;
    return impl.presentedThisFrame;
}

void Ps4VkPicaRenderBackend::SetPresentOptions(const PresentOptions &options) {
    auto &impl = *mImpl;
    impl.options = options;
    impl.options.renderScale = std::clamp<uint32_t>(options.renderScale, 1U, 6U);
}

const Ps4VkPicaRenderBackend::PresentOptions &Ps4VkPicaRenderBackend::CurrentPresentOptions()
    const {
    return mImpl->options;
}

void Ps4VkPicaRenderBackend::SetImmediatePresentation(bool immediate) {
    mImpl->immediatePresent = immediate;
}

void Ps4VkPicaRenderBackend::SavePipelineCache() {
    auto &impl = *mImpl;
    if (!impl.pipelineCacheDirty || impl.pipelineCache == VK_NULL_HANDLE) return;
    size_t size = 0;
    if (impl.vk.vkGetPipelineCacheData(impl.device, impl.pipelineCache, &size, nullptr) !=
            VK_SUCCESS ||
        size == 0U)
        return;
    std::vector<uint8_t> data(size);
    if (impl.vk.vkGetPipelineCacheData(impl.device, impl.pipelineCache, &size, data.data()) !=
        VK_SUCCESS)
        return;
    if (WriteFile(kPipelineCachePath, data.data(), size)) {
        impl.pipelineCacheDirty = false;
        impl.Log("vk-backend: pipeline cache saved (%zu bytes, %llu pipelines)", size,
                 (unsigned long long)impl.stats.pipelines);
    }
}

}  // namespace triaevum::ps4

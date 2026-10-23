// TriAevum PS4 - internal state of the Vulkan PICA backend
// (ps4_vk_pica_backend.cpp), shared with the product's rendering API
// (ports/ps4-product: Fast3D UI draws, ImGui and the presentation pass record
// into the same command buffer and scanout render pass). Not a public API.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "ps4_vk_pica_backend.h"
#include "fast/renderer3ds/pica_ui_canvas.h"

namespace triaevum::ps4 {
namespace detail {

#define PS4VK_INSTANCE_FUNCS(X)                     \
    X(vkDestroyInstance)                            \
    X(vkEnumeratePhysicalDevices)                   \
    X(vkGetPhysicalDeviceProperties)                \
    X(vkGetPhysicalDeviceFeatures)                  \
    X(vkGetPhysicalDeviceMemoryProperties)          \
    X(vkGetPhysicalDeviceFormatProperties)          \
    X(vkGetPhysicalDeviceQueueFamilyProperties)     \
    X(vkCreateDevice)                               \
    X(vkGetDeviceProcAddr)

#define PS4VK_DEVICE_FUNCS(X)          \
    X(vkGetDeviceQueue)                \
    X(vkDeviceWaitIdle)                \
    X(vkCreateCommandPool)             \
    X(vkResetCommandPool)              \
    X(vkAllocateCommandBuffers)        \
    X(vkBeginCommandBuffer)            \
    X(vkEndCommandBuffer)              \
    X(vkQueueSubmit)                   \
    X(vkCreateFence)                   \
    X(vkWaitForFences)                 \
    X(vkResetFences)                   \
    X(vkCreateBuffer)                  \
    X(vkGetBufferMemoryRequirements)   \
    X(vkBindBufferMemory)              \
    X(vkAllocateMemory)                \
    X(vkFreeMemory)                    \
    X(vkMapMemory)                     \
    X(vkCreateImage)                   \
    X(vkDestroyImage)                  \
    X(vkGetImageMemoryRequirements)    \
    X(vkBindImageMemory)               \
    X(vkGetImageSubresourceLayout)     \
    X(vkCreateImageView)               \
    X(vkDestroyImageView)              \
    X(vkCreateSampler)                 \
    X(vkCreateRenderPass)              \
    X(vkCreateFramebuffer)             \
    X(vkDestroyFramebuffer)            \
    X(vkCreateShaderModule)            \
    X(vkCreateDescriptorSetLayout)     \
    X(vkCreatePipelineLayout)          \
    X(vkCreateDescriptorPool)          \
    X(vkResetDescriptorPool)           \
    X(vkAllocateDescriptorSets)        \
    X(vkUpdateDescriptorSets)          \
    X(vkCreatePipelineCache)           \
    X(vkGetPipelineCacheData)          \
    X(vkCreateGraphicsPipelines)       \
    X(vkCmdPipelineBarrier)            \
    X(vkCmdBeginRenderPass)            \
    X(vkCmdEndRenderPass)              \
    X(vkCmdBindPipeline)               \
    X(vkCmdBindDescriptorSets)         \
    X(vkCmdBindVertexBuffers)          \
    X(vkCmdBindIndexBuffer)            \
    X(vkCmdDraw)                       \
    X(vkCmdDrawIndexed)                \
    X(vkCmdSetViewport)                \
    X(vkCmdSetScissor)                 \
    X(vkCmdSetBlendConstants)          \
    X(vkCmdSetStencilCompareMask)      \
    X(vkCmdSetStencilWriteMask)        \
    X(vkCmdSetStencilReference)        \
    X(vkCmdCopyBufferToImage)          \
    X(vkCmdCopyImageToBuffer)          \
    X(vkCmdBlitImage)                  \
    X(vkCmdClearColorImage)            \
    X(vkCmdClearDepthStencilImage)     \
    X(vkCmdClearAttachments)           \
    X(vkCmdPushConstants)

struct Vk {
#define PS4VK_DECLARE(name) PFN_##name name = nullptr;
    PS4VK_INSTANCE_FUNCS(PS4VK_DECLARE)
    PS4VK_DEVICE_FUNCS(PS4VK_DECLARE)
#undef PS4VK_DECLARE
};

}  // namespace detail

// ===================================================================== Impl ==

struct Ps4VkPicaRenderBackend::Impl {
    Ps4VkPicaRenderBackend *owner = nullptr;
    LogFn log = nullptr;
    VkBackendStats stats{};
    detail::Vk vk{};

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    bool logicOpSupported = false;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    bool depthHasStencil = false;

    // -- recording --
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer uploadCb = VK_NULL_HANDLE;  // texture/LUT uploads, run first
    VkCommandBuffer mainCb = VK_NULL_HANDLE;    // ordered PICA work
    VkFence fence = VK_NULL_HANDLE;
    bool recording = false;
    bool uploadsRecorded = false;
    bool workRecorded = false;

    // -- streaming ring (vertex/index/uniform data and texture staging) --
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ringMemory = VK_NULL_HANDLE;
    uint8_t *ringPointer = nullptr;
    VkDeviceSize ringHead = 0;

    // -- readback --
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    uint8_t *readbackPointer = nullptr;
    VkDeviceSize readbackBytes = 0;

    // -- descriptors / layouts --
    VkDescriptorSetLayout picaSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout picaPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout presentSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout presentPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    uint32_t setsAllocated = 0;

    VkRenderPass picaPass = VK_NULL_HANDLE;
    VkRenderPass scanoutPass = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    bool pipelineCacheDirty = false;
    VkPipeline presentReplace = VK_NULL_HANDLE;
    VkPipeline presentOverlay = VK_NULL_HANDLE;

    // -- images --
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory dedicated = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        uint32_t mipLevels = 1;
    };
    struct MemoryChunk {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
    };
    std::vector<MemoryChunk> pool[VK_MAX_MEMORY_TYPES];

    Image whiteImage;   // RGBA8 1x1 white: unbound float samplers
    Image uintImage;    // R32_UINT 1x1 zero: unbound integer samplers + storage
    VkSampler nearestClamp = VK_NULL_HANDLE;
    VkSampler linearClamp = VK_NULL_HANDLE;
    std::map<uint32_t, VkSampler> samplers;

    struct TargetKey {
        uint64_t namespace_;
        uint32_t colorAddress;
        uint32_t depthAddress;
        uint16_t width;
        uint16_t height;
        uint8_t colorFormat;
        uint8_t depthFormat;
        bool operator<(const TargetKey &o) const {
            return std::tie(namespace_, colorAddress, depthAddress, width, height, colorFormat,
                            depthFormat) < std::tie(o.namespace_, o.colorAddress, o.depthAddress,
                                                    o.width, o.height, o.colorFormat,
                                                    o.depthFormat);
        }
    };
    struct Target {
        Image color;
        Image depth;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint64_t lastUse = 0;
        // Host pixels per guest pixel along each storage axis (the top
        // screen is wider than renderScale along Y in widescreen).
        float scaleX = 1.0F;
        float scaleY = 1.0F;
    };
    std::map<TargetKey, Target> targets;
    Target *activeTarget = nullptr;  // render pass open on this target
    bool scanoutPassActive = false;
    uint64_t useSerial = 0;

    struct Display {
        Image color;               // at its source target's scale x the guest extent
        uint32_t logicalWidth = 0;  // guest extent (CTR portrait layout)
        uint32_t logicalHeight = 0;
    };
    std::map<std::pair<uint64_t, uint32_t>, Display> displays;

    struct TextureKey {
        uint32_t address;
        uint64_t hash;
        uint64_t size;
        uint16_t width;
        uint16_t height;
        uint8_t format;
        uint8_t maxMipLevel;
        bool operator==(const TextureKey &o) const {
            return address == o.address && hash == o.hash && size == o.size &&
                   width == o.width && height == o.height && format == o.format &&
                   maxMipLevel == o.maxMipLevel;
        }
    };
    struct TextureKeyHash {
        size_t operator()(const TextureKey &k) const noexcept {
            return static_cast<size_t>(k.hash ^ (uint64_t{k.address} << 8) ^ k.size ^
                                       (uint64_t{k.width} << 40) ^ (uint64_t{k.height} << 52));
        }
    };
    std::unordered_map<TextureKey, Image, TextureKeyHash> textures;
    std::unordered_map<uint64_t, Image> lightingLuts;

    struct Shader {
        VkShaderModule module = VK_NULL_HANDLE;
        bool integerTexture0 = false;
        bool failed = false;
    };
    std::unordered_map<std::string, Shader> shaders;
    Shader fallbackFragment;
    int shaderDumps = 0;
    std::map<std::string, VkPipeline> pipelines;
    std::unordered_map<uint32_t, bool> vertexFormatSupport;

    struct PendingFill {
        uint64_t namespace_;
        uint32_t start;
        uint32_t end;
        uint32_t value;
        uint16_t control;
    };
    std::vector<PendingFill> pendingFills;
    std::vector<uint64_t> completions;

    // -- scanout / frame --
    struct Scanout {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint8_t *pixels = nullptr;
    };
    Scanout scanouts[kScanoutCount];
    uint32_t scanoutWidth = 0;
    uint32_t scanoutHeight = 0;
    uint32_t scanoutPitchPixels = 0;
    bool frameActive = false;
    uint32_t frameScanout = 0;
    bool presentedThisFrame = false;

    // Settings + per-screen presentation (the last display image each CTR
    // screen presented; composed into the scanout at EndFrame).
    PresentOptions options;
    uint32_t renderScale = 1;
    // Widening of the top screen's presented width (storage Y), from the
    // output aspect; 1 keeps the native 5:3.
    float topScreenExpansion = 1.0F;
    // Colour targets the scene (CompositionDomain::Scene) drew into this
    // frame: UI covering one of them may be frame content (desktop
    // PicaSceneFrame::HasSceneDrawOnColorTarget).
    std::vector<uint32_t> sceneColorTargets;
    struct ScreenRecord {
        Display *display = nullptr;
        uint32_t flags = 0;
        bool overlay = false;
    };
    ScreenRecord screens[2];
    bool immediatePresent = false;

    std::vector<uint8_t> decoded;
    std::vector<uint16_t> expandedIndices;
    int missingSourceLogs = 0;
    int skipLogs = 0;

    // ------------------------------------------------------------ logging --
    void Log(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (log == nullptr) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log(buf);
    }
    void NoteError(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(stats.lastError, sizeof stats.lastError, fmt, ap);
        va_end(ap);
        Log("vk-backend: %s", stats.lastError);
    }
    bool Check(VkResult result, const char *what) {
        if (result == VK_SUCCESS) return true;
        ++stats.vkErrors;
        NoteError("%s = %d", what, static_cast<int>(result));
        return false;
    }

    bool Initialize(uint32_t width, uint32_t height);
    bool CreateDeviceObjects();
    bool CreateScanouts(uint32_t width, uint32_t height);
    void Shutdown();

    // memory
    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred) const;
    bool CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred, VkBuffer *buffer, VkDeviceMemory *memory,
                      uint8_t **mapped);
    bool CreateImage(Image &out, uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VkImageAspectFlags aspect, bool pooled,
                     uint32_t mipLevels = 1);
    void DestroyImage(Image &image);
    void FreePools();

    // recording
    void EnsureRecording();
    void Flush();
    void EndRenderPass();
    void FullBarrier(VkCommandBuffer cb);
    void ToGeneral(VkCommandBuffer cb, const Image &image);
    bool RingReserve(VkDeviceSize bytes);
    VkDeviceSize RingAlloc(VkDeviceSize bytes, VkDeviceSize alignment);
    VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);

    // resources
    Shader *GetShader(bool vertex, const Fast::Renderer3ds::PicaShaderSourceIdentity &identity,
                      std::string_view source);
    bool BuildModule(const std::string &source, bool vertex, VkShaderModule *module,
                     std::string *log);
    void DumpShaderFailure(const char *stage, std::string_view source, const std::string &log);
    Target *GetTarget(const TargetKey &key);
    void ApplyFillsTo(const TargetKey &key, Target &target);
    void BeginTargetPass(Target &target);
    Image *GetTexture(const Oot3d::Renderer::PicaTextureView &view);
    Image *GetLightingLut(const Oot3d::Renderer::PicaLightingLutView &view);
    VkSampler GetSampler(const Oot3d::Renderer::PicaTextureView &view, uint32_t levels);
    bool VertexFormatSupported(VkFormat format);
    Fast::Renderer3ds::PicaRasterCanvas RasterCanvas(const Oot3d::Renderer::PicaDrawView &draw,
                                                     const Target &target);
    VkPipeline GetPipeline(const Oot3d::Renderer::PicaDrawView &draw, VkShaderModule vs,
                           VkShaderModule fs,
                           const std::vector<VkVertexInputBindingDescription> &bindings,
                           const std::vector<VkVertexInputAttributeDescription> &attributes);
    VkPipeline CreatePresentPipeline(bool overlay, VkShaderModule vs, VkShaderModule fs);

    // transfers
    void GuestFill(const Fast::Renderer3ds::PicaMemoryFillView &fill);
    void MaterializeRawCopy(const TargetKey &key, Target &source,
                            const Fast::Renderer3ds::PicaDisplayTransferView &transfer);
    void PresentDisplay(Display &display, const Fast::Renderer3ds::PicaDisplayTransferView &view);
    void ComposeScanout();
    // Begins the scanout render pass on this frame's scanout image (LOAD)
    // unless it is already open; ends any open PICA target pass first.
    void OpenScanoutPass();
    void DrawScreen(const ScreenRecord &record, int x, int y, int width, int height);
    void DestroyTargetsAndDisplays();
    void DestroyResources();
};

}  // namespace triaevum::ps4

// TriAevum PS4 - VK port, Phase 2: whole-AOT game host + Vulkan renderer.
//
// Runs the statically linked OoT3D game module (oot3d_game_module +
// triaevum_title_whole_aot_plugin + 256 generated whole-AOT shards) on the
// new Vulkan-stack runtime (DolphinPS4 port/: dlmalloc heap with the
// malloc-replace crt1, crash.log + boot-trace.log; libc++ 21 sysroot):
//   * content is read from /data/triaevum/game/content.tap staged by the user
//     (Forge-prepared directory pushed over GoldHEN FTP - never inside the
//     PKG, it is private user data derived from the user's own ROM);
//   * PICA is the real host (frontend + submission queue + scanout). With
//     the Vulkan backend up (ps4_vk_pica_backend.*: RADV on GNM, canonical
//     GLSL compiled on the console by glslang) the desktop drain
//     TriAevumOot3dPicaRenderer -> plan -> bridge -> backend rasterizes every
//     draw and presents into the VideoOut framebuffers (GPU-backed display
//     transfers deferred, like the desktop product). Without it, draws are
//     counted by the batch sink and the CPU paths of phase 1 stay in charge;
//     either way completions publish their GSP interrupts;
//   * audio submissions are validated, counted and discarded (real
//     sceAudioOut output is the audio phase);
//   * input is the DualShock 4 through scePad (Orbis direct, no SDL);
//   * the status page (until the game presents) and a HUD strip (after)
//     are CPU-drawn into the same two VideoOut framebuffers, flipped at the
//     30 Hz cadence;
//   * everything is mirrored to /data/triaevum_boot.log (stdout/stderr),
//     the kernel TTY log (sceKernelDebugOutText survives the redirect) and
//     the on-screen status page.
//
// This host contains no Nintendo data.

#include "ps4_font8x8.h"
#include "ps4_vk_pica_backend.h"

#include <cpuid.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "triaevum/module_abi.h"
#include "triaevum/service_abi.h"
#include "triaevum/service_registry.h"
#include "triaevum/audio_service_adapter.h"
#include "triaevum/input_service_adapter.h"
#include "triaevum/filesystem_service_adapter.h"
#include "triaevum/pica_service_adapter.h"
#include "triaevum/rooted_filesystem_backend.h"
#include "triaevum/pica_scanout_state.h"
#include "triaevum/tam_metadata.h"

#include <nlohmann/json.hpp>

// Host-side PICA service: frontend register state, submission queue, scanout
// and the checked guest-memory lease pool (TRIAEVUM_PICA_SERVICE_BRIDGE.md).
#include "triaevum_oot3d_pica_host.h"
#include "triaevum_oot3d_pica_batch.h"
#include "triaevum_oot3d_pica_renderer.h"

// Statically linked title AOT plugin (256 generated shards).
#include "triaevum_title_whole_aot_abi.h"

// The statically linked game module exports this query (mirrors the desktop
// TRIAEVUM_MODULE_QUERY_SYMBOL_V1 dlsym path; no dlopen on PS4).
extern "C" const TriAevumModuleApiV1 *TriAevumQueryModuleV1(
    uint32_t queryAbi, uint32_t runtimeAbi) noexcept;

namespace {

using namespace triaevum::module;

constexpr int kW = 1920;
constexpr int kH = 1080;
constexpr uint64_t kNanoPerSecond = 1000000000ULL;
// One host presentation per 1/30 s of simulated guest time (= two 60 Hz CTR
// vblank ticks per call, matching the Switch product's native30 cadence).
constexpr uint64_t kFrameStepNs = 33333333ULL;
// One guest step = one 60 Hz CTR vblank (the desktop module host's
// kSimulationStepNanoseconds); two run per presented frame.
constexpr uint64_t kGuestStepNs = 16666667ULL;
constexpr size_t kMaximumContentIndexBytes = 4ULL * 1024ULL * 1024ULL;
constexpr const char *kContentIndexPath = "/data/triaevum/game/content.tap";
constexpr const char *kSaveRoot = "/data/triaevum/game/savedata";
constexpr const char *kLogPath = "/data/triaevum_boot.log";
constexpr const char *kSettingsPath = "/data/triaevum/ps4_settings.json";

// ---------------------------------------------------------------- logging --

FILE *g_logfile = nullptr;

void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void logf_(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    // printf goes to the redirected boot log (g_logfile IS stdout after the
    // dup2 in main); sceKernelDebugOutText is a separate syscall and still
    // reaches the kernel TTY log (TCP 3232) for live console tests.
    printf("[triaevum] %s\n", buf);
    sceKernelDebugOutText(0, buf);
    sceKernelDebugOutText(0, "\n");
    if (g_logfile != nullptr && g_logfile != stdout) {
        fprintf(g_logfile, "%s\n", buf);
        fflush(g_logfile);
    }
}

// Ring of the most recent module log lines, shown on screen.
struct LogRing {
    static constexpr size_t kLines = 12;
    static constexpr size_t kCapacity = 168;
    char lines[kLines][kCapacity]{};
    size_t head = 0;
    size_t count = 0;

    void push(const char *text) {
        snprintf(lines[head], kCapacity, "%s", text);
        head = (head + 1) % kLines;
        if (count < kLines) ++count;
    }
    const char *line(size_t index) const {
        const size_t first = (head + kLines - count) % kLines;
        return lines[(first + index) % kLines];
    }
};

LogRing g_moduleLog;

void TRIAEVUM_ABI_CALL HostLog(void *, TriAevumLogLevelV1 level,
                               const char *message, size_t messageSize) {
    if (message == nullptr || messageSize == 0U) return;
    char text[LogRing::kCapacity];
    const size_t copy =
        messageSize < sizeof text - 16 ? messageSize : sizeof text - 16;
    snprintf(text, sizeof text, "module[%u] %.*s", level, (int)copy, message);
    g_moduleLog.push(text);
    printf("[triaevum] %s\n", text);
    sceKernelDebugOutText(0, text);
    sceKernelDebugOutText(0, "\n");
    if (g_logfile != nullptr && g_logfile != stdout) {
        fprintf(g_logfile, "%s\n", text);
        fflush(g_logfile);
    }
}

uint64_t TRIAEVUM_ABI_CALL HostMonotonicTimeNs(void *) {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * kNanoPerSecond + (uint64_t)ts.tv_nsec;
}

uint64_t NowNs() { return HostMonotonicTimeNs(nullptr); }

// ------------------------------------------------------------- host state --

struct HostState {
    // PICA service operation counters (observed at the registry trampoline).
    uint64_t registerWrites = 0;
    uint64_t gspCommands = 0;
    uint64_t framebufferUpdates = 0;
    uint64_t flushes = 0;
    uint64_t interruptTakes = 0;
    // Batch drain counters (dispatcher stats).
    uint64_t batchDraws = 0;
    uint64_t batchCompletions = 0;
    uint64_t drainedBytes = 0;
    uint64_t drainedBytesHighWater = 0;
    uint64_t windowMaxResourceBytes = 0;
    // Scanout presenter counters.
    uint64_t presents = 0;
    uint64_t presentFailures = 0;
    char presentError[64] = "";
    // Audio counters (discarded output; playback arrives with the audio
    // phase).
    uint64_t audioSubmissions = 0;
    uint64_t audioFrames = 0;
    uint64_t audioSamples = 0;
    // Input counters.
    uint64_t inputReads = 0;
    // Filesystem counters (snapshotted from the backend each second).
    uint64_t fsOpens = 0;
    uint64_t fsReads = 0;
    uint64_t fsReadBytes = 0;
};

HostState g_host;

// ---------------------------------------------------------------- status --

struct Status {
    char content[80] = "checking";
    char module[80] = "not started";
    uint64_t frames = 0;
    uint64_t aotEntries = 0;
    double fps = 0.0;
    char lastError[168] = "";
    bool running = false;
    // Short per-screen scanout summaries for the status page.
    char scanoutSummary[2][56] = {"unconfigured", "unconfigured"};
};

Status g_status;

// ------------------------------------------------------- video out screen --
//
// The status page and the software scanout presenter draw into one cached
// 1920x1080 buffer (g_screenPixels, 0xAARRGGBB like the old SDL host), and
// presentScreen() copies it into the garlic flip buffer. VideoOut's
// A8B8G8R8_SRGB format names channels MSB-first, so a little-endian pixel
// value is A<<24|B<<16|G<<8|R - byte 0 is RED. The console proof is the VK
// probe: VkFormat R8G8B8A8_UNORM linear images registered with this format
// displayed correct colors. swap() converts our ARGB constants once.

std::vector<uint32_t> g_screenPixels;  // kW x kH, 0xAARRGGBB
int g_videoOut = -1;
OrbisKernelEqueue g_flipQueue = 0;
void *g_videoBuffers[2] = {nullptr, nullptr};
uint32_t g_pitchPixels = kW;
// The buffer the next flip shows; the other one is on screen. Console
// evidence 0.91: CPU writes to garlic AND onion flip buffers both display
// (the 0.90 black screen did not come from the memory type).
uint32_t g_nextBuffer = 0;
// Phase 2 renderer. When it is up, the two VideoOut framebuffers are its
// linear scanout VkImages (the VK probe allocation), so GPU presentation and
// the CPU status page/HUD share them.
std::unique_ptr<triaevum::ps4::Ps4VkPicaRenderBackend> g_vk;

// ---------------------------------------------------------- settings menu --
//
// L3+R3 opens a host settings menu (the console stand-in for the desktop F1
// panel). It is CPU-drawn over the composed frame, owns the controller while
// open, and persists to /data/triaevum/ps4_settings.json. The TopScreen
// single-screen HUD and its options arrive with the UI lifecycle port.

std::atomic<bool> g_menuOpen{false};

struct Ps4Settings {
    uint32_t renderScale = 4;
    uint32_t layout = 0;  // Ps4VkPicaRenderBackend::ScreenLayout
    bool stretch = false;
    bool linearFilter = true;
    bool perfHud = true;
};

Ps4Settings g_settings;

void applySettings() {
    if (g_vk == nullptr) return;
    triaevum::ps4::Ps4VkPicaRenderBackend::PresentOptions options;
    options.renderScale = g_settings.renderScale;
    // Single-screen product: only the top screen reaches the TV.
    options.layout = triaevum::ps4::Ps4VkPicaRenderBackend::ScreenLayout::TopOnly;
    options.stretch = g_settings.stretch;
    options.linearFilter = g_settings.linearFilter;
    g_vk->SetPresentOptions(options);
}

void loadSettings() {
    std::ifstream stream(kSettingsPath);
    if (!stream) return;
    try {
        nlohmann::json json;
        stream >> json;
        g_settings.renderScale = std::clamp<uint32_t>(
            json.value("render_scale", g_settings.renderScale), 1U, 6U);
        g_settings.layout = std::min<uint32_t>(json.value("screen_layout", g_settings.layout), 2U);
        g_settings.stretch = json.value("stretch", g_settings.stretch);
        g_settings.linearFilter = json.value("linear_filter", g_settings.linearFilter);
        g_settings.perfHud = json.value("performance_hud", g_settings.perfHud);
    } catch (const std::exception &) {
        // A malformed file keeps the defaults; the next save rewrites it.
    }
}

void saveSettings() {
    const nlohmann::json json = {
        {"render_scale", g_settings.renderScale},
        {"screen_layout", g_settings.layout},
        {"stretch", g_settings.stretch},
        {"linear_filter", g_settings.linearFilter},
        {"performance_hud", g_settings.perfHud},
    };
    std::ofstream stream(kSettingsPath, std::ios::trunc);
    if (stream) stream << json.dump(2) << '\n';
}

inline uint32_t toVideoOutPixel(uint32_t argb) {
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0x000000FFu) |
           ((argb & 0x000000FFu) << 16);
}

void *allocateDirect(size_t bytes, const char *what, uint32_t type) {
    constexpr size_t kAlign = 2u << 20;  // VideoOut wants 2 MiB alignment
    const size_t total = (bytes + kAlign - 1) / kAlign * kAlign;
    off_t phys = 0;
    int32_t r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                              total, kAlign, type, &phys);
    if (r != 0) {
        logf_("%s: sceKernelAllocateDirectMemory(%zu) = %#x", what, total, r);
        return nullptr;
    }
    void *addr = nullptr;
    r = sceKernelMapDirectMemory(&addr, total,
                                 ORBIS_KERNEL_PROT_CPU_RW |
                                     ORBIS_KERNEL_PROT_GPU_RW,
                                 0, phys, kAlign);
    if (r != 0) {
        logf_("%s: sceKernelMapDirectMemory = %#x", what, r);
        return nullptr;
    }
    memset(addr, 0, total);
    logf_("%s: %zu bytes at %p (type=%u)", what, total, addr, type);
    return addr;
}

void backendLog(const char *text) { logf_("%s", text); }

bool initScreen() {
    if (sceUserServiceInitialize(nullptr) != 0) {
        logf_("sceUserServiceInitialize failed");
    }
    g_videoOut = sceVideoOutOpen(0xFF, 0, 0, nullptr);
    if (g_videoOut < 0) {
        logf_("sceVideoOutOpen = %#x", g_videoOut);
        return false;
    }
    g_vk = triaevum::ps4::Ps4VkPicaRenderBackend::Create(backendLog, kW, kH);
    if (g_vk != nullptr) {
        g_videoBuffers[0] = g_vk->ScanoutPixels(0);
        g_videoBuffers[1] = g_vk->ScanoutPixels(1);
        g_pitchPixels = g_vk->ScanoutPitchPixels();
    } else {
        constexpr size_t kFrameBytes =
            static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4u;
        for (uint32_t i = 0; i < 2; ++i) {
            char what[40];
            snprintf(what, sizeof what, "videoout fb %u (garlic)", i);
            g_videoBuffers[i] =
                allocateDirect(kFrameBytes, what, ORBIS_KERNEL_WC_GARLIC);
            if (g_videoBuffers[i] == nullptr) return false;
        }
    }
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, kW, kH,
                                  g_pitchPixels);
    const int32_t registered =
        sceVideoOutRegisterBuffers(g_videoOut, 0, g_videoBuffers, 2, &attribute);
    logf_("sceVideoOutRegisterBuffers = %d (%s: %p %p pitch %u)", registered,
          g_vk != nullptr ? "vulkan scanout images" : "cpu garlic",
          g_videoBuffers[0], g_videoBuffers[1], g_pitchPixels);
    if (registered < 0) {
        return false;
    }
    if (sceKernelCreateEqueue(&g_flipQueue, "triaevum_flip") != 0) {
        logf_("sceKernelCreateEqueue failed");
        return false;
    }
    if (sceVideoOutAddFlipEvent(g_flipQueue, g_videoOut, nullptr) != 0) {
        logf_("sceVideoOutAddFlipEvent failed");
        return false;
    }
    g_screenPixels.assign(static_cast<size_t>(kW) * kH, 0xFF101018u);
    return true;
}

// Copies rows [y0, y1) x columns [x0, x1) of the screen buffer into the
// next flip buffer.
void copyScreenRect(int x0, int y0, int x1, int y1) {
    uint32_t *dst = static_cast<uint32_t *>(g_videoBuffers[g_nextBuffer]);
    const uint32_t *src = g_screenPixels.data();
    for (int row = y0; row < y1; ++row) {
        uint32_t *d = dst + static_cast<size_t>(row) * g_pitchPixels;
        const uint32_t *s = src + static_cast<size_t>(row) * kW;
        for (int column = x0; column < x1; ++column) {
            d[column] = toVideoOutPixel(s[column]);
        }
    }
}

void copyScreenRegion(int y0, int y1, int width) { copyScreenRect(0, y0, width, y1); }

bool g_flipPending = false;

// Blocks until the last submitted flip is on screen, which frees the buffer
// that was showing before it (the next one to be written).
void waitPendingFlip() {
    if (!g_flipPending) return;
    OrbisKernelEvent event;
    int count = 0;
    sceKernelWaitEqueue(g_flipQueue, &event, 1, &count, nullptr);
    g_flipPending = false;
}

// Flips the next buffer (whatever wrote it: the CPU copy or the renderer's
// presentation pass) and makes the other one the next.
void flipNextBuffer(uint32_t frameNumber) {
    if (g_videoOut < 0) return;
    const uint32_t buffer = g_nextBuffer;
    const int flipped =
        sceVideoOutSubmitFlip(g_videoOut, static_cast<int32_t>(buffer),
                              ORBIS_VIDEO_OUT_FLIP_VSYNC, frameNumber);
    if (flipped != 0) {
        ++g_host.presentFailures;
        snprintf(g_host.presentError, sizeof g_host.presentError,
                 "flip=%#x", flipped);
        return;
    }
    // Not waited here: the next frame's work overlaps the vblank wait, and
    // waitPendingFlip() runs before anything writes the buffer still on
    // screen.
    g_flipPending = true;
    ++g_host.presents;
    g_nextBuffer ^= 1U;
}

void presentScreen(uint32_t frameNumber) {
    if (g_videoOut < 0) return;
    waitPendingFlip();
    copyScreenRegion(0, kH, kW);
    flipNextBuffer(frameNumber);
}

void fill(int x, int y, int w, int h, uint32_t argb) {
    if (w <= 0 || h <= 0 || x >= kW || y >= kH) return;
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w < kW ? x + w : kW;
    const int y1 = y + h < kH ? y + h : kH;
    for (int row = y0; row < y1; ++row) {
        uint32_t *line = &g_screenPixels[static_cast<size_t>(row) * kW];
        for (int column = x0; column < x1; ++column) {
            line[column] = argb;
        }
    }
}

// ------------------------------------------------------ PICA batch sink --

// The batch sink: Phase 1 consumes nothing on the GPU (draws become renderer
// work for the Vulkan phase; display transfers and fills already ran on the
// guest-memory CPU path), but completions MUST publish their interrupts or
// the guest GSP thread blocks forever waiting for PSC0/PSC1 after the first
// command list.
class Ps4BatchSink final
    : public Oot3dNativeGame::TriAevumOot3dPicaBatchSink {
public:
    explicit Ps4BatchSink(Oot3dNativeGame::TriAevumOot3dPicaHost *host)
        : mHost(host) {}

    bool SubmitDraw(Oot3dNativeGame::Oot3dPicaDrawSubmission &,
                    std::string *) override {
        ++g_host.batchDraws;
        return true;
    }

    bool SubmitDisplayTransfer(
        const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission &,
        std::string *) override {
        return true;
    }

    bool SubmitMemoryFill(
        const Oot3dNativeGame::Oot3dPicaMemoryFillSubmission &,
        std::string *) override {
        return true;
    }

    bool QueueCompletion(uint64_t,
                         Oot3dNativeGame::Oot3dPicaInterruptId interrupt,
                         std::string *) override {
        ++g_host.batchCompletions;
        mHost->PublishCompletedInterrupt(interrupt);
        return true;
    }

private:
    Oot3dNativeGame::TriAevumOot3dPicaHost *mHost;
};

// The real PICA service lives in TriAevumOot3dPicaHost (frontend state +
// submission queue + scanout + lease pool). The registry calls this
// trampoline; it also keeps the operation counters for the status page.
TriAevumModuleStatusV1 TRIAEVUM_ABI_CALL picaHostTrampoline(
    void *context, uint32_t operation, TriAevumReadOnlyBytesV1 request,
    TriAevumMutableBytesV1 response, size_t *responseSize) {
    auto *host =
        static_cast<Oot3dNativeGame::TriAevumOot3dPicaHost *>(context);
    switch (operation) {
    case TRIAEVUM_PICA_WRITE_REGISTERS_V1:
        ++g_host.registerWrites;
        break;
    case TRIAEVUM_PICA_SUBMIT_GSP_COMMAND_V1:
        ++g_host.gspCommands;
        break;
    case TRIAEVUM_PICA_SET_FRAMEBUFFER_V1:
        ++g_host.framebufferUpdates;
        break;
    case TRIAEVUM_PICA_TAKE_INTERRUPTS_V1:
        ++g_host.interruptTakes;
        break;
    case TRIAEVUM_PICA_FLUSH_V1:
        ++g_host.flushes;
        break;
    default:
        break;
    }
    return host->Invoke(operation, request, response, responseSize);
}

// ---------------------------------------------------- scanout presenter --

// CTR LCD framebuffers (GSP framebuffer info), mirroring the color table of
// tools/oot3d/native_pica_frontend/oot3d_native_pica_transfer.cpp:
// 0 = RGBA8 (A,B,G,R bytes), 1 = BGR8, 2 = RGB565, 3 = RGBA5551,
// 4 = RGBA4444.
uint32_t framebufferBytesPerPixel(uint32_t format) {
    switch (format & 7U) {
    case 0U:
        return 4U;
    case 1U:
        return 3U;
    default:
        return 2U;
    }
}

uint32_t decodeFramebufferPixel(uint32_t format, const uint8_t *source) {
    switch (format & 7U) {
    case 0U: { // RGBA8, bytes {A,B,G,R}
        return static_cast<uint32_t>(source[3]) |
               static_cast<uint32_t>(source[2]) << 8U |
               static_cast<uint32_t>(source[1]) << 16U |
               static_cast<uint32_t>(source[0]) << 24U;
    }
    case 1U: { // BGR8
        const uint32_t b = source[0], g = source[1], r = source[2];
        return 0xFF000000u | r << 16U | g << 8U | b;
    }
    case 2U: { // RGB565
        const uint16_t packed =
            static_cast<uint16_t>(source[0]) |
            static_cast<uint16_t>(source[1]) << 8U;
        const auto r = static_cast<uint8_t>(((packed >> 11U) & 0x1FU) << 3U |
                                            ((packed >> 13U) & 7U));
        const auto g = static_cast<uint8_t>(((packed >> 5U) & 0x3FU) << 2U |
                                            ((packed >> 9U) & 3U));
        const auto b = static_cast<uint8_t>((packed & 0x1FU) << 3U |
                                           ((packed >> 2U) & 7U));
        return 0xFF000000u | r << 16U | g << 8U | b;
    }
    case 3U: { // RGBA5551
        const uint16_t packed =
            static_cast<uint16_t>(source[0]) |
            static_cast<uint16_t>(source[1]) << 8U;
        const auto r = static_cast<uint8_t>(((packed >> 11U) & 0x1FU) << 3U |
                                            ((packed >> 13U) & 7U));
        const auto g = static_cast<uint8_t>(((packed >> 6U) & 0x1FU) << 3U |
                                            ((packed >> 8U) & 7U));
        const auto b = static_cast<uint8_t>(((packed >> 1U) & 0x1FU) << 3U |
                                            ((packed >> 3U) & 7U));
        return ((packed & 1U) != 0U ? 0xFF000000u : 0U) | r << 16U | g << 8U |
               b;
    }
    default: { // RGBA4444
        const uint16_t packed =
            static_cast<uint16_t>(source[0]) |
            static_cast<uint16_t>(source[1]) << 8U;
        const auto expand = [](uint32_t nibble) {
            return static_cast<uint8_t>(nibble << 4U | nibble);
        };
        return static_cast<uint32_t>(expand(packed & 0xFU)) << 24U |
               static_cast<uint32_t>(expand((packed >> 4U) & 0xFU)) << 16U |
               static_cast<uint32_t>(expand((packed >> 8U) & 0xFU)) << 8U |
               static_cast<uint32_t>(expand((packed >> 12U) & 0xFU));
    }
    }
}

// Presenter: reads the shown top-screen LCD framebuffer out of guest memory
// through a checked read lease, applies the CTR physical portrait-to-
// landscape transform and blits it into the screen buffer at 3x scale.
//
// Console evidence (the phase 3.0 PKGs): the title scans out a portrait
// 240x400 BGR8 framebuffer (stride 720). The reference scanout shader
// (three_ds_recomp gfx_opengl_pica.cpp) samples the portrait texture at
// (uv.y, uv.x) - a transpose; its extra Y flip only compensates OpenGL's
// bottom-left origin, which a top-down buffer does not need. So:
// landscape(lx, ly) = portrait(column = ly, row = lx).
//
// The 3D draws that fill the source render target are NOT emulated yet, so
// the presented image is whatever the CPU-side fills/transfers composed.
// The SingleScreen title profile means only the top screen is worth
// presenting.
constexpr int kLandscapeWidth = 400;
constexpr int kLandscapeHeight = 240;
constexpr int kPortraitRows = 400;  // landscape width == portrait row count
constexpr int kPresentScale = 3;
constexpr int kPresentX = 680;
constexpr int kPresentY = 240;

std::vector<uint32_t> g_topSurface;  // 400x240 ARGB (landscape)

void presentScanout(const triaevum::module::PicaScanoutStateV1 &scanout,
                    const TriAevumModuleApiV1 *api, bool refresh) {
    const int destX = kPresentX;
    const int destY = kPresentY;
    const int destW = kLandscapeWidth * kPresentScale;
    const int destH = kLandscapeHeight * kPresentScale;
    if (!refresh) {
        // Software presentation is expensive on the Jaguar; refresh the
        // lease-backed image at half rate and re-blit the cached buffer in
        // between so the status background never flickers through.
        if (!g_topSurface.empty()) {
            fill(destX, destY, destW, destH, 0xFF000000u);
            for (int ly = 0; ly < kLandscapeHeight; ++ly) {
                for (int lx = 0; lx < kLandscapeWidth; ++lx) {
                    const uint32_t argb =
                        g_topSurface[static_cast<size_t>(ly) *
                                         kLandscapeWidth +
                                     lx];
                    fill(destX + lx * kPresentScale, destY + ly * kPresentScale,
                         kPresentScale, kPresentScale, argb);
                }
            }
        }
        return;
    }
    const auto framebuffer = scanout.Framebuffer(0U);
    if (scanout.LcdForceBlack() || !framebuffer.has_value() ||
        framebuffer->addressLeft == 0U || framebuffer->stride == 0U) {
        fill(destX, destY, destW, destH, 0xFF000000u);
        return;
    }
    const uint32_t bpp = framebufferBytesPerPixel(framebuffer->format);
    if (bpp == 0U) {
        return;
    }
    const uint32_t portraitColumns = framebuffer->stride / bpp;
    if (portraitColumns < 1U ||
        portraitColumns < static_cast<uint32_t>(kLandscapeHeight)) {
        return;
    }
    const TriAevumGuestMemoryMapRequestV1 request = {
        sizeof(TriAevumGuestMemoryMapRequestV1),
        TRIAEVUM_GUEST_MEMORY_READ_V1,
        framebuffer->addressLeft,
        framebuffer->stride * static_cast<uint32_t>(kPortraitRows),
    };
    TriAevumGuestMemoryViewV1 view{};
    const TriAevumModuleStatusV1 status =
        api->map_guest_memory(&request, &view);
    if (status != TRIAEVUM_MODULE_OK_V1) {
        ++g_host.presentFailures;
        snprintf(g_host.presentError, sizeof g_host.presentError,
                 "lease top failed (%u)", status);
        fill(destX, destY, destW, destH, 0xFF400000u);
        return;
    }
    if (g_topSurface.empty()) {
        g_topSurface.assign(static_cast<size_t>(kLandscapeWidth) *
                                kLandscapeHeight,
                            0u);
    }
    const uint8_t *source = static_cast<const uint8_t *>(view.data);
    const uint32_t stride = framebuffer->stride;
    for (int landscapeX = 0; landscapeX < kLandscapeWidth; ++landscapeX) {
        // landscapeX selects the portrait ROW; landscapeY the column.
        const uint8_t *column =
            source + static_cast<size_t>(landscapeX) * stride;
        for (int landscapeY = 0; landscapeY < kLandscapeHeight; ++landscapeY) {
            g_topSurface[static_cast<size_t>(landscapeY) * kLandscapeWidth +
                         static_cast<size_t>(landscapeX)] =
                decodeFramebufferPixel(framebuffer->format,
                                       column + landscapeY * bpp);
        }
    }
    fill(destX, destY, destW, destH, 0xFF000000u);
    for (int ly = 0; ly < kLandscapeHeight; ++ly) {
        for (int lx = 0; lx < kLandscapeWidth; ++lx) {
            fill(destX + lx * kPresentScale, destY + ly * kPresentScale,
                 kPresentScale, kPresentScale,
                 g_topSurface[static_cast<size_t>(ly) * kLandscapeWidth +
                              lx]);
        }
    }
    ++g_host.presents;
    api->unmap_guest_memory(view.token, 0U);
}

// Log framebuffer changes: this is the console evidence for what geometry
// and pixel format the title actually scans out.
void logFramebufferIfChanged(
    const triaevum::module::PicaScanoutStateV1 &scanout) {
    static uint32_t lastAddress[2] = {};
    static uint32_t lastFormat[2] = {0xFFU, 0xFFU};
    static uint32_t lastStride[2] = {};
    static uint32_t lastShown[2] = {0xFFU, 0xFFU};
    for (uint32_t screen = 0; screen < 2U; ++screen) {
        const auto framebuffer = scanout.Framebuffer(screen);
        if (!framebuffer.has_value()) {
            continue;
        }
        if (framebuffer->addressLeft != lastAddress[screen] ||
            framebuffer->format != lastFormat[screen] ||
            framebuffer->stride != lastStride[screen] ||
            framebuffer->bufferIndex != lastShown[screen]) {
            lastAddress[screen] = framebuffer->addressLeft;
            lastFormat[screen] = framebuffer->format;
            lastStride[screen] = framebuffer->stride;
            lastShown[screen] = framebuffer->bufferIndex;
            snprintf(g_status.scanoutSummary[screen],
                     sizeof g_status.scanoutSummary[screen],
                     "0x%08x f%u s%u", framebuffer->addressLeft,
                     framebuffer->format, framebuffer->stride);
            logf_("scanout screen=%u address=0x%08x format=%u stride=%u "
                  "shown=%u",
                  screen, framebuffer->addressLeft, framebuffer->format,
                  framebuffer->stride, framebuffer->bufferIndex);
        }
    }
}

// -------------------------------------------------------------- audio sink --

class Ps4AudioBackend final : public AudioServiceBackendV1 {
public:
    TriAevumModuleStatusV1 SubmitPcm(const AudioPcmSubmissionV1 &submission,
                                     uint32_t *acceptedFrameCount,
                                     uint32_t *queuedFrameCount) override {
        // The native DSP HLE always submits S16 interleaved stereo at the
        // native 32728 Hz rate in 160-frame units.
        if (acceptedFrameCount == nullptr || queuedFrameCount == nullptr ||
            submission.sampleFormat != TRIAEVUM_AUDIO_S16_V1 ||
            submission.channelCount != 2U ||
            submission.sampleRateHz != 32728U) {
            return TRIAEVUM_MODULE_INVALID_ARGUMENT_V1;
        }
        ++g_host.audioSubmissions;
        g_host.audioFrames += submission.frameCount;
        g_host.audioSamples += submission.frameCount * submission.channelCount;
        *acceptedFrameCount = submission.frameCount;
        *queuedFrameCount = 0U;
        return TRIAEVUM_MODULE_OK_V1;
    }

    TriAevumModuleStatusV1 SetStreamState(
        uint32_t, TriAevumAudioStreamStateV1) override {
        return TRIAEVUM_MODULE_OK_V1;
    }
};

// ----------------------------------------------------------------- input --

class Ps4InputBackend final : public InputServiceBackendV1 {
public:
    TriAevumModuleStatusV1 ReadState(uint32_t playerIndex,
                                     InputStateV1 *state) override {
        if (playerIndex != 0U || state == nullptr) {
            return TRIAEVUM_MODULE_INVALID_ARGUMENT_V1;
        }
        *state = {};
        ++mReads;
        state->sampleSequence = mReads;

        // The settings menu owns the controller while it is open: the game
        // sees a neutral pad.
        if (g_menuOpen.load(std::memory_order_relaxed)) {
            return TRIAEVUM_MODULE_OK_V1;
        }
        OrbisPadData data{};
        if (mPad < 0 || scePadReadState(mPad, &data) != 0 || !data.connected) {
            return TRIAEVUM_MODULE_OK_V1;
        }
        const uint32_t b = data.buttons;
        uint64_t buttons = 0U;
        // Same layout the SDL host used (SDL gamepad on a DualShock):
        // cross=A, circle=B, square=X, triangle=Y. The touch-pad click
        // stands in for SELECT (the DualShock has no share/back button in
        // OrbisPadData).
        if (b & ORBIS_PAD_BUTTON_CROSS) buttons |= TRIAEVUM_INPUT_BUTTON_A_V1;
        if (b & ORBIS_PAD_BUTTON_CIRCLE) buttons |= TRIAEVUM_INPUT_BUTTON_B_V1;
        if (b & ORBIS_PAD_BUTTON_SQUARE) buttons |= TRIAEVUM_INPUT_BUTTON_X_V1;
        if (b & ORBIS_PAD_BUTTON_TRIANGLE) buttons |= TRIAEVUM_INPUT_BUTTON_Y_V1;
        if (b & ORBIS_PAD_BUTTON_OPTIONS) buttons |= TRIAEVUM_INPUT_BUTTON_START_V1;
        if (b & ORBIS_PAD_BUTTON_TOUCH_PAD) buttons |= TRIAEVUM_INPUT_BUTTON_SELECT_V1;
        if (b & ORBIS_PAD_BUTTON_UP) buttons |= TRIAEVUM_INPUT_BUTTON_DPAD_UP_V1;
        if (b & ORBIS_PAD_BUTTON_DOWN) buttons |= TRIAEVUM_INPUT_BUTTON_DPAD_DOWN_V1;
        if (b & ORBIS_PAD_BUTTON_LEFT) buttons |= TRIAEVUM_INPUT_BUTTON_DPAD_LEFT_V1;
        if (b & ORBIS_PAD_BUTTON_RIGHT) buttons |= TRIAEVUM_INPUT_BUTTON_DPAD_RIGHT_V1;
        if (b & ORBIS_PAD_BUTTON_L1) buttons |= TRIAEVUM_INPUT_BUTTON_L_V1;
        if (b & ORBIS_PAD_BUTTON_R1) buttons |= TRIAEVUM_INPUT_BUTTON_R_V1;
        // Analog triggers mapped to the 3DS ZL/ZR buttons (threshold keeps
        // the old SDL axis>16384 semantics).
        if (data.analogButtons.l2 > 64u) buttons |= TRIAEVUM_INPUT_BUTTON_ZL_V1;
        if (data.analogButtons.r2 > 64u) buttons |= TRIAEVUM_INPUT_BUTTON_ZR_V1;
        state->buttons = buttons;
        // Orbis sticks are unsigned, 128-centered. The circle pad wants
        // up-positive Y; the DualShock reports up as a value below 128.
        state->leftStickX = axis(data.leftStick.x);
        state->leftStickY = -axis(data.leftStick.y);
        return TRIAEVUM_MODULE_OK_V1;
    }

    void openFirst() {
        // Console evidence 0.90/0.91: scePadOpen returned 0x80920005
        // (ORBIS_PAD_ERROR_NOT_INITIALIZED) - the library needs scePadInit.
        const int32_t init = scePadInit();
        if (init != 0) {
            logf_("input: scePadInit = %#x", init);
        }
        OrbisUserServiceUserId userId = 0;
        if (sceUserServiceGetInitialUser(&userId) != 0) {
            logf_("input: sceUserServiceGetInitialUser failed");
            return;
        }
        mPad = scePadOpen(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
        if (mPad < 0) {
            logf_("input: scePadOpen = %#x (no controller?)", mPad);
        } else {
            logf_("input: scePad opened (handle %d)", mPad);
        }
    }

    uint64_t reads() const { return mReads; }

    // Host-side read for the settings menu (L3+R3 and navigation).
    uint32_t hostButtons() const {
        OrbisPadData data{};
        if (mPad < 0 || scePadReadState(mPad, &data) != 0 || !data.connected) return 0U;
        return data.buttons;
    }

private:
    static float axis(uint8_t value) {
        float v = (static_cast<int>(value) - 128) / 128.0F;
        if (v > 1.0F) v = 1.0F;
        if (v < -1.0F) v = -1.0F;
        if (v > -0.12F && v < 0.12F) v = 0.0F;
        return v;
    }

    uint64_t mReads = 0;
    int mPad = -1;
};

// ------------------------------------------------------------ screen text --

void drawText(int x, int y, const char *text, uint32_t argb, int scale = 2) {
    for (const char *p = text; *p != '\0'; ++p) {
        const unsigned char glyph = (unsigned char)*p & 0x7FU;
        const unsigned char *rows = kPs4Font8x8[glyph];
        for (int row = 0; row < 8; ++row) {
            for (int col = 0; col < 8; ++col) {
                if ((rows[row] & (1u << col)) != 0U) {
                    fill(x + col * scale, y + row * scale, scale, scale, argb);
                }
            }
        }
        x += 8 * scale;
    }
}

// ------------------------------------------------------------ cpu/memory --

void reportSystem() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char vendor[13] = {};
    __get_cpuid(0, &a, &b, &c, &d);
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    __get_cpuid(1, &a, &b, &c, &d);
    const bool avx = (c >> 28) & 1, f16c = (c >> 29) & 1, sse42 = (c >> 20) & 1;
    unsigned a7 = 0, b7 = 0, c7 = 0, d7 = 0;
    __cpuid_count(7, 0, a7, b7, c7, d7);
    const bool avx2 = (b7 >> 5) & 1, bmi1 = (b7 >> 3) & 1;
    logf_("cpu vendor=%s sse4.2=%d avx=%d f16c=%d avx2=%d bmi1=%d", vendor,
          sse42, avx, f16c, avx2, bmi1);
    const size_t dmem = sceKernelGetDirectMemorySize();
    logf_("direct memory total = %llu MiB", (unsigned long long)(dmem >> 20));
    // Guest memory arrives as ordinary heap vectors (the malloc-replace crt1
    // routes every allocation to the dlmalloc heap in system flexible
    // memory), so probe malloc the same way the game module will use it.
    void *probe = malloc(256ULL << 20);
    logf_("malloc 256MiB probe = %s", probe ? "ok" : "failed");
    free(probe);
}

// Mirrors Oot3dNativeGame::CreateTriAevumForgeContentFilesystem, with one
// PS4-specific difference: descriptor paths are resolved against the
// content.tap directory. The desktop factory consumes the descriptor path
// as-is (the legacy Forge layout writes absolute paths); the PS4 staging
// writes path_mode=manifest_relative_v1 ("inputs/code.bin"), and a raw
// relative path would resolve against the eboot's CWD (app0), which is why
// the first console test reported "content.tap references an unavailable or
// ambiguous file".
std::unique_ptr<RootedFilesystemBackendV1> createPs4ContentFilesystem(
    const std::filesystem::path &contentIndexPath,
    const std::filesystem::path &saveRoot, std::string *error) {
    try {
        namespace fs = std::filesystem;
        std::error_code fileError;
        if (!fs::is_regular_file(contentIndexPath, fileError) || fileError) {
            throw std::runtime_error("content.tap is not a regular file");
        }
        std::ifstream stream(contentIndexPath, std::ios::binary);
        nlohmann::json content;
        if (!stream || !(stream >> content) || !content.is_object() ||
            content.value("format", "") != "triaevum_content_index_v1" ||
            !content.value("private_local_artifact", false) ||
            content.value("redistributable", true)) {
            throw std::runtime_error("private content index is invalid");
        }

        RootedFilesystemConfigV1 config;
        const fs::path base = contentIndexPath.parent_path();
        auto addMapping = [&](const std::string &logical,
                              const nlohmann::json &descriptor) {
            if (!descriptor.is_object() || !descriptor.contains("path") ||
                !descriptor.at("path").is_string() ||
                !descriptor.contains("bytes") ||
                !descriptor.at("bytes").is_number_unsigned()) {
                throw std::runtime_error(
                    "content.tap file descriptor is invalid");
            }
            fs::path hostPath{descriptor.at("path").get<std::string>()};
            if (hostPath.is_relative()) {
                hostPath = base / hostPath;
            }
            const std::uint64_t expected =
                descriptor.at("bytes").get<std::uint64_t>();
            const std::uintmax_t actual = fs::file_size(hostPath, fileError);
            char mismatch[128] = {};
            if (fileError) {
                snprintf(mismatch, sizeof mismatch, " (missing)");
            } else if (actual != expected) {
                snprintf(mismatch, sizeof mismatch,
                         " (want %llu got %llu)",
                         (unsigned long long)expected,
                         (unsigned long long)actual);
            }
            if (fileError || actual != expected || expected == 0U ||
                !config.contentFiles.emplace(logical, hostPath).second) {
                throw std::runtime_error(
                    "content.tap references an unavailable or ambiguous file: "
                    + logical + " -> " + hostPath.string() + mismatch);
            }
        };
        const auto &inputs = content.at("inputs");
        if (!inputs.is_object() || inputs.empty()) {
            throw std::runtime_error("private content index has no inputs");
        }
        for (const auto &[name, descriptor] : inputs.items()) {
            addMapping("inputs/" + name, descriptor);
        }
        addMapping("process_manifest", content.at("process_manifest"));
        config.saveRoot = saveRoot;
        auto backend =
            RootedFilesystemBackendV1::Create(std::move(config), error);
        if (backend == nullptr && error != nullptr && error->empty()) {
            *error = "rooted filesystem backend could not be created";
        }
        return backend;
    } catch (const std::exception &exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return nullptr;
    }
}

void drawStatusPage(uint32_t frame) {
    fill(0, 0, kW, kH, 0xFF101018u);
    int y = 24;
    drawText(24, y,
             g_vk != nullptr ? "TRIAEVUM PS4 - VK PHASE 2 - VULKAN RENDERER (WAITING FOR THE FIRST FRAME)"
                             : "TRIAEVUM PS4 - VK PHASE 2 - NO VULKAN (CPU FALLBACK)",
             0xFF60FF90u);
    y += 30;
    char line[192];

    snprintf(line, sizeof line, "content: %s", g_status.content);
    drawText(24, y, line, 0xFFB0B0C0u);
    y += 24;
    snprintf(line, sizeof line, "module: %s", g_status.module);
    drawText(24, y, line, 0xFFB0B0C0u);
    y += 24;
    snprintf(line, sizeof line, "whole-aot entry points: %llu",
             (unsigned long long)g_status.aotEntries);
    drawText(24, y, line, 0xFFB0B0C0u);
    y += 24;
    snprintf(line, sizeof line,
             "frames=%llu fps=%.1f pica[reg=%llu gsp=%llu intr=%llu fl=%llu]",
             (unsigned long long)g_status.frames, g_status.fps,
             (unsigned long long)g_host.registerWrites,
             (unsigned long long)g_host.gspCommands,
             (unsigned long long)g_host.interruptTakes,
             (unsigned long long)g_host.flushes);
    drawText(24, y, line, 0xFFB0B0C0u);
    y += 24;
    snprintf(line, sizeof line,
             "scanout presents=%llu fails=%llu draws=%llu compl=%llu top=%s",
             (unsigned long long)g_host.presents,
             (unsigned long long)g_host.presentFailures,
             (unsigned long long)g_host.batchDraws,
             (unsigned long long)g_host.batchCompletions,
             g_status.scanoutSummary[0]);
    drawText(24, y, line, 0xFFB0B0C0u);
    y += 24;
    snprintf(line, sizeof line,
             "audio[frames=%llu samples=%llu discarded - output is the audio phase]",
             (unsigned long long)g_host.audioFrames,
             (unsigned long long)g_host.audioSamples);
    drawText(24, y, line, 0xFFB0B0C0u);
    y += 24;
    snprintf(line, sizeof line,
             "input reads=%llu fs opens=%llu reads=%llu readBytes=%llu",
             (unsigned long long)g_host.inputReads,
             (unsigned long long)g_host.fsOpens,
             (unsigned long long)g_host.fsReads,
             (unsigned long long)g_host.fsReadBytes);
    drawText(24, y, line, 0xFFB0B0C0u);
    y += 24;
    if (g_vk != nullptr) {
        const auto &vs = g_vk->Stats();
        snprintf(line, sizeof line,
                 "vk draws=%llu skip=%llu shaders=%llu+%llu fail=%llu pipes=%llu "
                 "tex=%llu xfer=%llu pres=%llu",
                 (unsigned long long)vs.draws, (unsigned long long)vs.drawsSkipped,
                 (unsigned long long)vs.shadersCompiled,
                 (unsigned long long)vs.shadersFromDisk,
                 (unsigned long long)vs.shaderFailures,
                 (unsigned long long)vs.pipelines, (unsigned long long)vs.textures,
                 (unsigned long long)vs.transfers, (unsigned long long)vs.presents);
        drawText(24, y, line, 0xFFB0B0C0u);
        y += 24;
        if (vs.lastError[0] != '\0') {
            snprintf(line, sizeof line, "vk: %s", vs.lastError);
            drawText(24, y, line, 0xFFFFB070u);
            y += 24;
        }
    }
    y += 6;

    if (g_status.lastError[0] != '\0') {
        drawText(24, y, "ERROR:", 0xFFFF7070u);
        y += 24;
        drawText(24, y, g_status.lastError, 0xFFFF9090u);
        y += 24;
    }

    drawText(24, y, "module log:", 0xFF7090FFu);
    y += 24;
    for (size_t i = 0; i < g_moduleLog.count && y < kH - 80; ++i) {
        drawText(24, y, g_moduleLog.line(i), 0xFF9090A8u);
        y += 20;
    }

    // Heartbeat so a frozen frame is obvious.
    fill(24, kH - 60, 40 + (int)(frame % 300), 20, 0xFF60FF60u);
}

// ------------------------------------------------------------------ menu --

struct MenuState {
    uint32_t selected = 0;
    uint32_t previousButtons = 0;
};
MenuState g_menu;

constexpr uint32_t kMenuItems = 5;

// Edge-triggered controller handling. L3+R3 toggles the menu from anywhere;
// inside it the D-pad moves/changes, cross toggles, circle closes.
void updateMenu(uint32_t buttons) {
    const uint32_t pressed = buttons & ~g_menu.previousButtons;
    g_menu.previousButtons = buttons;
    const uint32_t combo = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
    if ((buttons & combo) == combo && (pressed & combo) != 0U) {
        const bool open = !g_menuOpen.load();
        g_menuOpen.store(open);
        logf_("settings menu %s", open ? "opened" : "closed");
        return;
    }
    if (!g_menuOpen.load()) return;
    if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0U) {
        g_menuOpen.store(false);
        return;
    }
    if ((pressed & ORBIS_PAD_BUTTON_UP) != 0U)
        g_menu.selected = (g_menu.selected + kMenuItems - 1U) % kMenuItems;
    if ((pressed & ORBIS_PAD_BUTTON_DOWN) != 0U)
        g_menu.selected = (g_menu.selected + 1U) % kMenuItems;
    int step = 0;
    if ((pressed & ORBIS_PAD_BUTTON_RIGHT) != 0U || (pressed & ORBIS_PAD_BUTTON_CROSS) != 0U)
        step = 1;
    if ((pressed & ORBIS_PAD_BUTTON_LEFT) != 0U) step = -1;
    if (step == 0) return;
    switch (g_menu.selected) {
        case 0:
            g_settings.renderScale = static_cast<uint32_t>(
                std::clamp<int>(static_cast<int>(g_settings.renderScale) + step, 1, 6));
            break;
        case 1: g_settings.stretch = !g_settings.stretch; break;
        case 2: g_settings.linearFilter = !g_settings.linearFilter; break;
        case 3: g_settings.perfHud = !g_settings.perfHud; break;
        case 4:
            g_menuOpen.store(false);
            return;
        default: break;
    }
    applySettings();
    saveSettings();
    logf_("settings: scale=%u layout=%u stretch=%d linear=%d hud=%d", g_settings.renderScale,
          g_settings.layout, g_settings.stretch, g_settings.linearFilter, g_settings.perfHud);
}

constexpr int kMenuX = 460;
constexpr int kMenuY = 300;
constexpr int kMenuW = 1000;
constexpr int kMenuH = 440;

void drawMenu() {
    fill(kMenuX, kMenuY, kMenuW, kMenuH, 0xFF20242Cu);
    fill(kMenuX, kMenuY, kMenuW, 4, 0xFF60FF90u);
    drawText(kMenuX + 24, kMenuY + 20, "TRIAEVUM - CONFIGURACOES", 0xFF60FF90u, 3);
    char values[kMenuItems][64];
    snprintf(values[0], sizeof values[0], "%ux (tela de cima %ux%u)", g_settings.renderScale,
             400U * g_settings.renderScale, 240U * g_settings.renderScale);
    snprintf(values[1], sizeof values[1], "%s", g_settings.stretch ? "sim" : "nao");
    snprintf(values[2], sizeof values[2], "%s",
             g_settings.linearFilter ? "suave (linear)" : "nitido (nearest)");
    snprintf(values[3], sizeof values[3], "%s", g_settings.perfHud ? "sim" : "nao");
    values[4][0] = '\0';
    static const char *const kLabels[kMenuItems] = {"Resolucao interna", "Esticar para 16:9",
                                                    "Filtro da imagem", "HUD de desempenho",
                                                    "Fechar"};
    int y = kMenuY + 80;
    for (uint32_t item = 0; item < kMenuItems; ++item) {
        const bool selected = item == g_menu.selected;
        if (selected) fill(kMenuX + 12, y - 6, kMenuW - 24, 30, 0xFF384050u);
        drawText(kMenuX + 28, y, kLabels[item], selected ? 0xFFFFFFFFu : 0xFFB0B0C0u, 2);
        if (values[item][0] != '\0') {
            char line[80];
            snprintf(line, sizeof line, "< %s >", values[item]);
            drawText(kMenuX + 400, y, line, selected ? 0xFF60FF90u : 0xFF90A0B0u, 2);
        }
        y += 40;
    }
    drawText(kMenuX + 24, kMenuY + kMenuH - 64,
             "D-pad: navegar/alterar   X: alternar   O ou L3+R3: fechar", 0xFF9090A8u, 2);
    drawText(kMenuX + 24, kMenuY + kMenuH - 36,
             "HUD single screen (TopScreen): em desenvolvimento", 0xFFFFB070u, 2);
}

void presentMenuIfOpen() {
    if (!g_menuOpen.load()) return;
    drawMenu();
    copyScreenRect(kMenuX, kMenuY, kMenuX + kMenuW, kMenuY + kMenuH);
}

// One-line telemetry strip over the game image (CPU-drawn into the flip
// buffer after the GPU finished it).
void drawHudStrip() {
    constexpr int kHudWidth = 1100;
    constexpr int kHudHeight = 14;
    fill(0, 0, kHudWidth, kHudHeight, 0xC0000000u);
    char line[160];
    const auto &vs = g_vk->Stats();
    snprintf(line, sizeof line,
             "fps %.1f  draws %llu skip %llu  shaders %llu+%llu fail %llu  pipes %llu  "
             "tex %llu  xfer-fail %llu",
             g_status.fps, (unsigned long long)vs.draws, (unsigned long long)vs.drawsSkipped,
             (unsigned long long)vs.shadersCompiled, (unsigned long long)vs.shadersFromDisk,
             (unsigned long long)vs.shaderFailures, (unsigned long long)vs.pipelines,
             (unsigned long long)vs.textures, (unsigned long long)vs.transferFailures);
    drawText(4, 3, line, 0xFF80FF80u, 1);
    copyScreenRegion(0, kHudHeight, kHudWidth);
}

} // namespace

int main(int, char **) {
    // stdout/stderr carry the boot log (the same evidence channel the old
    // host used; logf_ additionally reaches the kernel TTY through
    // sceKernelDebugOutText, which the redirect cannot silence).
    const int bootLog =
        open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (bootLog >= 0) {
        dup2(bootLog, STDOUT_FILENO);
        dup2(bootLog, STDERR_FILENO);
        if (bootLog > STDERR_FILENO) {
            close(bootLog);
        }
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_logfile = stdout;

    logf_("TriAevum PS4 - VK port phase 2 - whole-AOT host + Vulkan renderer - "
          "build " TRIAEVUM_PS4_BUILD_VERSION);

    // Calling into a system library whose module is not loaded kills the
    // process with PRX_NOT_RESOLVED_FUNCTION (DolphinPS4's PlatformPS4
    // documents the same for every Orbis homebrew).
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);

    reportSystem();

    if (!initScreen()) {
        logf_("video out initialization failed - continuing headless");
    }
    loadSettings();
    applySettings();
    logf_("settings: scale=%u layout=%u stretch=%d linear=%d hud=%d (L3+R3 opens the menu)",
          g_settings.renderScale, g_settings.layout, g_settings.stretch,
          g_settings.linearFilter, g_settings.perfHud);

    // 1. Statically linked title AOT program sanity.
    const Oot3dNativeGame::Oot3dWholeAotProgramV2 *program =
        Oot3dNativeGame::triaevum_title_whole_aot_query(
            Oot3dNativeGame::kOot3dWholeAotPluginAbiV2);
    if (program == nullptr || program->EntryPoints == nullptr ||
        program->EntryPointCount == 0U) {
        logf_("whole-aot plugin query failed");
        fill(0, 0, kW, kH, 0xFF301010u);
        drawText(24, 40, "WHOLE-AOT PLUGIN QUERY RETURNED NULL", 0xFFFF8080u, 2);
        for (uint32_t f = 0;; ++f) {
            presentScreen(f);
            sceKernelUsleep(1000000);
        }
    }
    g_status.aotEntries = program->EntryPointCount;
    logf_("whole-aot program: abi=%u entries=%u memobs=%u", program->AbiVersion,
          (unsigned)program->EntryPointCount,
          program->MemoryWriteObservationVersion);

    // 2. Statically linked game module query.
    const TriAevumModuleApiV1 *api = TriAevumQueryModuleV1(
        TRIAEVUM_MODULE_QUERY_ABI_V1, TRIAEVUM_RUNTIME_ABI_V1);
    if (api == nullptr || api->abi_version != TRIAEVUM_RUNTIME_ABI_V1 ||
        api->struct_size < sizeof(TriAevumModuleApiV1) ||
        api->initialize == nullptr || api->run_frame == nullptr ||
        api->shutdown == nullptr) {
        logf_("static game module query failed");
        fill(0, 0, kW, kH, 0xFF301010u);
        drawText(24, 40, "GAME MODULE QUERY FAILED", 0xFFFF8080u, 2);
        for (uint32_t f = 0;; ++f) {
            presentScreen(f);
            sceKernelUsleep(1000000);
        }
    }
    logf_("game module api ok (struct_size=%u)", api->struct_size);

    // 3. Private content index (never shipped inside the PKG).
    std::vector<uint8_t> contentIndex;
    {
        FILE *f = fopen(kContentIndexPath, "rb");
        if (f == nullptr) {
            snprintf(g_status.content, sizeof g_status.content,
                     "MISSING /data/triaevum/game/content.tap");
        } else {
            fseek(f, 0, SEEK_END);
            const long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0 && (uint64_t)size <= kMaximumContentIndexBytes) {
                contentIndex.assign((size_t)size, 0U);
                if (fread(contentIndex.data(), 1, (size_t)size, f) ==
                    (size_t)size) {
                    snprintf(g_status.content, sizeof g_status.content,
                             "content.tap %ld bytes", size);
                } else {
                    contentIndex.clear();
                    snprintf(g_status.content, sizeof g_status.content,
                             "content.tap read failed");
                }
            } else {
                snprintf(g_status.content, sizeof g_status.content,
                         "content.tap has invalid size");
            }
            fclose(f);
        }
    }
    logf_("content index: %s", g_status.content);

    // 4. Host services + module initialization.
    Ps4AudioBackend audio;
    AudioHostServiceAdapterV1 audioService(audio);
    Ps4InputBackend input;
    InputHostServiceAdapterV1 inputService(input);

    HostServiceRegistry registry(
        HostRuntimeCallbacksV1{nullptr, HostLog, HostMonotonicTimeNs});
    std::unique_ptr<RootedFilesystemBackendV1> filesystemBackend;
    std::unique_ptr<FilesystemHostServiceAdapterV1> filesystemService;
    if (!contentIndex.empty()) {
        std::string filesystemError;
        filesystemBackend = createPs4ContentFilesystem(
            std::filesystem::path(kContentIndexPath),
            std::filesystem::path(kSaveRoot), &filesystemError);
        if (filesystemBackend == nullptr) {
            snprintf(g_status.lastError, sizeof g_status.lastError, "%s",
                     filesystemError.c_str());
            logf_("content filesystem failed: %s", filesystemError.c_str());
        } else {
            filesystemService =
                std::make_unique<FilesystemHostServiceAdapterV1>(
                    *filesystemBackend);
        }
    }

    // 4b. Real PICA host (frontend state + submission queue + scanout). The
    // metadata synthesis replaces the desktop TAM metadata: the module ABI
    // only needs the declared PICA service and the validated physical memory
    // regions, both of which the content index already carries. With the
    // Vulkan backend the GPU-backed display transfers are deferred to it
    // (the desktop setting); without it the guest-memory CPU path runs them.
    std::unique_ptr<Oot3dNativeGame::TriAevumOot3dPicaHost> picaHost;
    if (!contentIndex.empty()) {
        try {
            triaevum::module::TamModuleMetadataV1 picaMetadata{};
            picaMetadata.runtimeAbi = TRIAEVUM_RUNTIME_ABI_V1;
            picaMetadata.requiredServices.push_back(
                {TRIAEVUM_SERVICE_PICA_V1, TRIAEVUM_SERVICE_SCHEMA_V1});
            const nlohmann::json content = nlohmann::json::parse(
                contentIndex.begin(), contentIndex.end());
            for (const auto &region :
                 content.at("physical_memory_regions")) {
                picaMetadata.physicalMemoryRegions.push_back(
                    {region.at("physical_base").get<std::uint32_t>(),
                     region.at("guest_base").get<std::uint32_t>(),
                     region.at("bytes").get<std::uint32_t>()});
            }
            std::string picaError;
            picaHost = Oot3dNativeGame::TriAevumOot3dPicaHost::Create(
                picaMetadata,
                [api](const TriAevumGuestMemoryMapRequestV1 &request,
                      TriAevumGuestMemoryViewV1 *view) {
                    return api->map_guest_memory(&request, view);
                },
                [api](std::uint64_t token, std::uint32_t flags) {
                    return api->unmap_guest_memory(token, flags);
                },
                {}, /* deferGpuBackedDisplayTransfers */ g_vk != nullptr,
                &picaError);
            if (picaHost == nullptr) {
                logf_("pica host failed: %s", picaError.c_str());
                snprintf(g_status.lastError, sizeof g_status.lastError,
                         "pica host: %s", picaError.c_str());
            } else {
                picaHost->Queue().SetRuntimeProfilingEnabled(true);
                // The frontend's diagnostic history retains EVERY draw
                // packet (11 KiB each) by default. On the PS4 its amortized
                // growth is fatal: the 8192->16384 element doubling requests
                // exactly 181534720 bytes and kills run_frame (every GLES
                // console crash was at draws=8193). The renderer never
                // consumes this history; disable it.
                picaHost->Frontend().SetDiagnosticHistoryEnabled(false);
                logf_("pica host ready (regions=%zu)",
                      picaMetadata.physicalMemoryRegions.size());
            }
        } catch (const std::exception &exception) {
            logf_("pica host metadata failed: %s", exception.what());
        }
    }

    bool servicesReady = false;
    do {
        if (filesystemService == nullptr) break;
        if (picaHost == nullptr) break;
        if (registry.Register(TRIAEVUM_SERVICE_FILESYSTEM_V1,
                              FilesystemHostServiceAdapterV1::Invoke,
                              filesystemService.get()) !=
            ServiceRegistrationResult::Registered)
            break;
        if (registry.Register(TRIAEVUM_SERVICE_PICA_V1, picaHostTrampoline,
                              picaHost.get()) !=
            ServiceRegistrationResult::Registered)
            break;
        if (registry.Register(TRIAEVUM_SERVICE_AUDIO_V1,
                              AudioHostServiceAdapterV1::Invoke,
                              &audioService) !=
            ServiceRegistrationResult::Registered)
            break;
        if (registry.Register(TRIAEVUM_SERVICE_INPUT_V1,
                              InputHostServiceAdapterV1::Invoke,
                              &inputService) !=
            ServiceRegistrationResult::Registered)
            break;
        servicesReady = true;
    } while (false);

    if (servicesReady) {
        snprintf(g_status.module, sizeof g_status.module, "services ready");
        input.openFirst();
        const TriAevumHostApiV1 hostApi = registry.SealAndCreateHostApi();
        const TriAevumModuleStatusV1 status = api->initialize(
            &hostApi, (const char *)contentIndex.data(), contentIndex.size());
        if (status == TRIAEVUM_MODULE_OK_V1) {
            g_status.running = true;
            snprintf(g_status.module, sizeof g_status.module, "initialized");
            logf_("game module initialized - entering frame loop");
        } else {
            snprintf(g_status.module, sizeof g_status.module,
                     "initialize failed (status %u)", status);
            logf_("game module initialize failed: status=%u", status);
        }
    } else if (contentIndex.empty()) {
        snprintf(g_status.module, sizeof g_status.module,
                 "waiting for content at /data/triaevum/game");
        logf_("no content: stage the Forge-prepared game directory to "
              "/data/triaevum via FTP; the PKG never ships user content");
    } else {
        snprintf(g_status.module, sizeof g_status.module,
                 "content mapping failed - see ERROR");
    }

    // 5. Frame loop: 30 host presentations per second, each advancing 1/30 s
    // of simulated guest time (two 60 Hz CTR vblanks). The batch dispatcher
    // drains the PICA submission queue through the counting sink so the
    // guest's GSP flow keeps moving; the software presenter composes the
    // status page and the scanout blit into the screen buffer.
    Oot3dNativeGame::TriAevumOot3dPicaBatchDispatcher batchDispatcher;
    std::unique_ptr<Ps4BatchSink> batchSink;
    std::unique_ptr<Oot3dNativeGame::TriAevumOot3dPicaRenderer> picaRenderer;
    if (g_vk != nullptr && picaHost != nullptr) {
        // Physical-memory callbacks: guest leases through the module API
        // (the same checked path the software presenter uses).
        g_vk->SetPicaPhysicalMemoryAccess(
            [api](uint32_t address, std::span<uint8_t> out) {
                if (out.empty()) return true;
                const TriAevumGuestMemoryMapRequestV1 request = {
                    sizeof(TriAevumGuestMemoryMapRequestV1),
                    TRIAEVUM_GUEST_MEMORY_READ_V1, address,
                    static_cast<uint32_t>(out.size())};
                TriAevumGuestMemoryViewV1 view{};
                if (api->map_guest_memory(&request, &view) !=
                    TRIAEVUM_MODULE_OK_V1) {
                    return false;
                }
                std::memcpy(out.data(), view.data, out.size());
                api->unmap_guest_memory(view.token, 0U);
                return true;
            },
            [api](std::span<const Fast::Renderer3ds::PicaPhysicalMemoryWrite>
                      writes) {
                for (const auto &write : writes) {
                    if (write.Bytes.empty()) continue;
                    const TriAevumGuestMemoryMapRequestV1 request = {
                        sizeof(TriAevumGuestMemoryMapRequestV1),
                        TRIAEVUM_GUEST_MEMORY_WRITE_V1, write.Address,
                        static_cast<uint32_t>(write.Bytes.size())};
                    TriAevumGuestMemoryViewV1 view{};
                    if (api->map_guest_memory(&request, &view) !=
                        TRIAEVUM_MODULE_OK_V1) {
                        return false;
                    }
                    std::memcpy(view.data, write.Bytes.data(),
                                write.Bytes.size());
                    api->unmap_guest_memory(
                        view.token, TRIAEVUM_GUEST_MEMORY_UNMAP_WRITTEN_V1);
                }
                return true;
            },
            nullptr, nullptr, nullptr);
        picaRenderer =
            std::make_unique<Oot3dNativeGame::TriAevumOot3dPicaRenderer>(
                *g_vk, *picaHost);
        logf_("vulkan PICA renderer attached to the submission queue");
    } else if (picaHost != nullptr) {
        batchSink = std::make_unique<Ps4BatchSink>(picaHost.get());
    }
    // Last time the renderer put a game frame into a flip buffer; the
    // status page takes the screen back when the game stops presenting.
    uint64_t lastGamePresentNs = 0;
    uint64_t lastPipelineSaveNs = NowNs();
    uint64_t lastProfileTotalNs = 0;
    uint64_t lastProfileDrawCalls = 0;
    // Per-second loop telemetry (pacing evidence): average time spent in
    // run_frame + drain, sampled once per loop iteration.
    uint64_t workNanosAccum = 0;
    uint32_t workSamples = 0;
    uint64_t nextPresentationNs = NowNs();
    uint64_t fpsWindowStart = nextPresentationNs;
    uint64_t fpsWindowFrames = 0;
    uint32_t drawFrame = 0;
    for (;;) {
        // Hard 30 Hz pacing against the monotonic clock. The single-shot
        // sleep let the loop free-run at 52 fps on the console, so the
        // deadline is re-checked after every sleep: a sleep that returns
        // early can no longer speed the guest up, and the work time lands
        // in the heartbeat.
        while (NowNs() + 500000ULL < nextPresentationNs) {
            const uint64_t until = NowNs();
            sceKernelUsleep(
                (useconds_t)((nextPresentationNs - until) / 1000ULL));
        }
        const uint64_t workStartNs = NowNs();

        // One guest step: publish the GPU completions, advance the title,
        // then drain its submissions (the desktop module host order).
        const auto runStep = [&](uint64_t stepNs, uint64_t presentationNs) {
            if (!g_status.running) return;
            if (picaRenderer != nullptr) {
                std::string pollError;
                if (!picaRenderer->PollCompletions(&pollError)) {
                    logf_("pica completion poll failed: %s", pollError.c_str());
                }
            }
            const TriAevumFrameInputV1 frameInput = {
                sizeof(TriAevumFrameInputV1),
                0U,
                presentationNs,
                stepNs,
                {nullptr, 0U},
            };
            const TriAevumModuleStatusV1 status = api->run_frame(&frameInput);
            ++g_status.frames;
            if (status != TRIAEVUM_MODULE_OK_V1) {
                g_status.running = false;
                snprintf(g_status.module, sizeof g_status.module,
                         "run_frame failed (status %u) at frame %llu", status,
                         (unsigned long long)g_status.frames);
                snprintf(g_status.lastError, sizeof g_status.lastError,
                         "run_frame status %u at frame %llu", status,
                         (unsigned long long)g_status.frames);
                logf_("run_frame failed: status=%u frame=%llu", status,
                      (unsigned long long)g_status.frames);
            }
            if (picaRenderer != nullptr) {
                std::string drainError;
                if (!picaRenderer->DrainPending(&drainError)) {
                    logf_("pica renderer drain faulted: %s", drainError.c_str());
                    snprintf(g_status.lastError, sizeof g_status.lastError,
                             "pica drain: %s", drainError.c_str());
                }
            }
        };

        // Renderer mode: two 1/60 s guest steps per presented frame. Console
        // evidence 0.92/0.93: one 1/30 s step per frame let the title make a
        // single GPU round trip (submit -> completion interrupt) per two
        // vblanks, so it rendered at half rate - slow motion at a steady
        // host 30 fps. The desktop module host runs 1/60 s steps
        // (kSimulationStepNanoseconds) with a drain after each. Without the
        // renderer, the counting sink path keeps the single 1/30 s step.
        bool gamePresented = false;
        if (picaRenderer != nullptr) {
            // The buffer this frame composes into is the one on screen until
            // the previous flip lands.
            waitPendingFlip();
            g_vk->BeginFrame(g_nextBuffer);
            runStep(kGuestStepNs, nextPresentationNs - kGuestStepNs);
            runStep(kFrameStepNs - kGuestStepNs, nextPresentationNs);
            gamePresented = g_vk->EndFrame();
            g_host.batchDraws = picaRenderer->Stats().Draws;
            g_host.batchCompletions = picaRenderer->Stats().Completions;
        } else if (picaHost != nullptr && batchSink != nullptr) {
            runStep(kFrameStepNs, nextPresentationNs);
            auto batch = picaHost->TakePendingBatch();
            if (!batch.Empty()) {
                uint64_t batchBytes = 0;
                for (const auto &draw : batch.Draws) {
                    for (const auto &resource : draw.Resources) {
                        const size_t resourceBytes =
                            resource.ResolvedBytes().size();
                        batchBytes += resourceBytes;
                        if (resourceBytes > g_host.windowMaxResourceBytes) {
                            g_host.windowMaxResourceBytes = resourceBytes;
                        }
                    }
                }
                g_host.drainedBytes += batchBytes;
                if (batchBytes > g_host.drainedBytesHighWater) {
                    g_host.drainedBytesHighWater = batchBytes;
                }
                std::string dispatchError;
                if (!batchDispatcher.Dispatch(std::move(batch), *batchSink,
                                              &dispatchError)) {
                    logf_("batch dispatch faulted: %s", dispatchError.c_str());
                    snprintf(g_status.lastError,
                             sizeof g_status.lastError, "batch: %s",
                             dispatchError.c_str());
                }
            }
        }

        workNanosAccum += NowNs() - workStartNs;
        ++workSamples;

        nextPresentationNs += kFrameStepNs;
        if (nextPresentationNs + kNanoPerSecond / 2ULL < NowNs()) {
            // More than half a second behind: drop the debt instead of
            // bursting catch-up work through the guest.
            nextPresentationNs = NowNs();
        }

        ++fpsWindowFrames;
        const uint64_t now2 = NowNs();
        if (now2 - fpsWindowStart >= kNanoPerSecond) {
            g_status.fps =
                (double)fpsWindowFrames * (double)kNanoPerSecond /
                (double)(now2 - fpsWindowStart);
            fpsWindowStart = now2;
            fpsWindowFrames = 0;
            g_host.inputReads = input.reads();
            if (filesystemBackend != nullptr) {
                const RootedFilesystemStatsV1 stats = filesystemBackend->Stats();
                g_host.fsOpens = stats.opens;
                g_host.fsReads = stats.reads;
                g_host.fsReadBytes = stats.readBytes;
            }
            // Heap availability probe: largest block that still allocates.
            int heapProbeMiB = 0;
            for (int probe : {512, 256, 128, 64, 32, 16, 8, 4}) {
                void *block = malloc(static_cast<size_t>(probe) << 20);
                if (block != nullptr) {
                    free(block);
                    heapProbeMiB = probe;
                    break;
                }
            }
            // Queue runtime profile deltas (nanoseconds spent inside the
            // submission pipeline this second).
            const auto profile = picaHost != nullptr
                                     ? picaHost->Queue().RuntimeProfile()
                                     : Oot3dNativeGame::
                                           Oot3dPicaSubmissionRuntimeProfile{};
            const uint64_t profileTotalDelta =
                profile.TotalNanoseconds - lastProfileTotalNs;
            const uint64_t profileDrawCalls =
                profile.DrawCalls - lastProfileDrawCalls;
            lastProfileTotalNs = profile.TotalNanoseconds;
            lastProfileDrawCalls = profile.DrawCalls;
            const double workMs =
                workSamples != 0
                    ? static_cast<double>(workNanosAccum) / 1000000.0 /
                          static_cast<double>(workSamples)
                    : 0.0;
            workNanosAccum = 0;
            workSamples = 0;
            logf_("heartbeat frames=%llu fps=%.1f work_ms=%.1f "
                  "pica_gsp=%llu draws=%llu compl=%llu presents=%llu "
                  "fails=%llu err=%s drained=%lluKiB "
                  "heap~%dMiB queue_ms=%.1f qdraws=%llu "
                  "maxRes=%lluKiB audio_frames=%llu fs_reads=%llu",
                  (unsigned long long)g_status.frames, g_status.fps, workMs,
                  (unsigned long long)g_host.gspCommands,
                  (unsigned long long)g_host.batchDraws,
                  (unsigned long long)g_host.batchCompletions,
                  (unsigned long long)g_host.presents,
                  (unsigned long long)g_host.presentFailures,
                  g_host.presentError,
                  (unsigned long long)(g_host.drainedBytes >> 10),
                  heapProbeMiB,
                  static_cast<double>(profileTotalDelta) / 1000000.0,
                  (unsigned long long)profileDrawCalls,
                  (unsigned long long)(g_host.windowMaxResourceBytes >> 10),
                  (unsigned long long)g_host.audioFrames,
                  (unsigned long long)g_host.fsReads);
            g_host.windowMaxResourceBytes = 0;
            if (g_vk != nullptr) {
                const auto &vs = g_vk->Stats();
                logf_("vk draws=%llu fail=%llu skip=%llu[src=%llu geo=%llu "
                      "fragop=%llu sc=%llu ext=%llu shader=%llu pipe=%llu] "
                      "shaders=%llu disk=%llu fail=%llu glslang_ms=%llu "
                      "pipes=%llu aco_ms=%llu tex=%llu texMiB=%llu "
                      "targets=%llu xfer=%llu xfail=%llu pres=%llu mat=%llu "
                      "fills=%llu clears=%llu submits=%llu gpu_wait_ms=%llu "
                      "vkerr=%llu last=%s",
                      (unsigned long long)vs.draws, (unsigned long long)vs.drawFailures,
                      (unsigned long long)vs.drawsSkipped,
                      (unsigned long long)vs.skipNoSource,
                      (unsigned long long)vs.skipNoGeometry,
                      (unsigned long long)vs.skipFragOp,
                      (unsigned long long)vs.skipScissor,
                      (unsigned long long)vs.skipExtent,
                      (unsigned long long)vs.skipShader,
                      (unsigned long long)vs.skipPipeline,
                      (unsigned long long)vs.shadersCompiled,
                      (unsigned long long)vs.shadersFromDisk,
                      (unsigned long long)vs.shaderFailures,
                      (unsigned long long)(vs.shaderCompileUs / 1000U),
                      (unsigned long long)vs.pipelines,
                      (unsigned long long)(vs.pipelineCompileUs / 1000U),
                      (unsigned long long)vs.textures,
                      (unsigned long long)(vs.textureUploadBytes >> 20),
                      (unsigned long long)vs.targets, (unsigned long long)vs.transfers,
                      (unsigned long long)vs.transferFailures,
                      (unsigned long long)vs.presents,
                      (unsigned long long)vs.materializations,
                      (unsigned long long)vs.fills, (unsigned long long)vs.clears,
                      (unsigned long long)vs.submits,
                      (unsigned long long)(vs.gpuWaitUs / 1000U),
                      (unsigned long long)vs.vkErrors, vs.lastError);
                if (now2 - lastPipelineSaveNs >= 10ULL * kNanoPerSecond) {
                    lastPipelineSaveNs = now2;
                    g_vk->SavePipelineCache();
                }
            }
        }

        // Presentation. Renderer mode: the game frame is already in the
        // next flip buffer - add the HUD strip and flip. While no game
        // frame arrives (boot, or more than 2 s without one) the status
        // page owns the screen. CPU fallback mode: status page + the
        // software scanout blit, as in phase 1.
        const uint32_t frameNumber = drawFrame++;
        if (picaHost != nullptr) {
            logFramebufferIfChanged(picaHost->Scanout());
        }
        updateMenu(input.hostButtons());
        if (gamePresented) {
            lastGamePresentNs = NowNs();
            if (g_settings.perfHud) drawHudStrip();
            presentMenuIfOpen();
            flipNextBuffer(frameNumber);
        } else if (lastGamePresentNs == 0U ||
                   NowNs() - lastGamePresentNs > 2ULL * kNanoPerSecond) {
            drawStatusPage(frameNumber);
            if (picaHost != nullptr && g_vk == nullptr) {
                presentScanout(picaHost->Scanout(), api,
                               (frameNumber & 1U) == 0U);
            }
            if (g_menuOpen.load()) drawMenu();
            presentScreen(frameNumber);
        }
    }
    return 0;
}

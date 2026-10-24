#pragma once

// PlayStation 4 (OpenOrbis) Fast3D backends. The window backend (VideoOut +
// SDL joystick/audio, no SDL video) and the rendering API (RADV on GNM:
// native PICA, Fast3D UI draws and ImGui) are implemented by the PS4 port
// (triaevum-ps4-vk ports/ps4-product), not by this tree; these are the hooks
// the shared window/GUI code calls under __ORBIS__.
#if defined(__ORBIS__)

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct ImDrawData;

namespace Fast {

class GfxWindowBackend;
class GfxRenderingAPI;

GfxWindowBackend* CreateOrbisWindowBackend();
GfxRenderingAPI* CreateOrbisRenderingApi();

// ImGui rendering owned by the rendering API (the desktop Vulkan backend
// exposes the same five operations on its concrete class).
class ImGuiRenderHooks {
  public:
    virtual ~ImGuiRenderHooks() = default;
    virtual bool InitImGuiBackend() = 0;
    virtual void ShutdownImGuiBackend() = 0;
    virtual bool ImGuiBackendReady() const = 0;
    virtual void NewImGuiFrame() = 0;
    virtual void RenderImGuiDrawData(ImDrawData* drawData) = 0;
};

// ImGui platform side: display size, timing and gamepad navigation from the
// DualShock. L3+R3 shows or hides the performance overlay.
void OrbisImGuiPlatformInit();
void OrbisImGuiPlatformNewFrame();

// Performance overlay (top-left corner, drawn inside the ImGui frame). The
// game thread and the rendering API each publish their numbers every half
// second; the overlay shows the latest.
struct OrbisPerformanceGame {
    std::atomic<float> SpeedPercent{0.0F};  // DSP frames against real time (204.5/s)
    std::atomic<float> GuestFps{0.0F};      // game frames per second (60 = full speed)
    std::atomic<float> GameCpuMs{0.0F};     // game thread time per presented frame
};
struct OrbisPerformanceRender {
    std::atomic<float> Fps{0.0F};           // frames presented per second
    std::atomic<float> FrameMsAverage{0.0F};
    std::atomic<float> FrameMsMaximum{0.0F};
    std::atomic<float> GpuMs{0.0F};         // GPU time per frame (submit to fence)
    std::atomic<float> GpuPercent{0.0F};    // share of wall time the GPU was busy
};
extern OrbisPerformanceGame gOrbisPerformanceGame;
extern OrbisPerformanceRender gOrbisPerformanceRender;
void OrbisDrawPerformanceOverlay();

// Process start (before main's work): system modules, boot log on
// /data/triaevum_boot.log, chdir to /data/triaevum, the DualShock mapping.
void OrbisProcessInit();

// The console Forge (ps4 product, src/ps4_forge.cpp): a decrypted ROM sent
// to /data/triaevum/forge becomes the game in /data/triaevum/game. False when
// there is no game to run (the user was told how to provide one).
bool OrbisPrepareGame();
// Runtime arguments for what the Forge installed (the HD UI texture pack and
// its TopScreen atlas), appended to the PS4 launch arguments.
std::vector<std::string> OrbisForgeLaunchArguments();
// The in-game menu's way back to Forge: restarts the app into its screen.
void OrbisOpenForge();
// One line on what Forge installed (the in-game menu shows it).
std::string OrbisForgeStatus();

// The game's busy threads ("main", "render", "pica plan") register: each is
// kept on its cores of the CPU's first cluster, and the sampling profiler
// (DolphinPS4's, /data/DolphinPS4/samples.log) follows it when
// /data/triaevum/profile exists.
void OrbisRegisterThread(const char* name);

// Stall report: every frame the rendering API bumps the heartbeat, and the
// game thread's blocking waits name themselves while they block. When the
// heartbeat stops for 10 s a watchdog logs the named wait (1.23: the game
// froze with every thread asleep and nothing said where).
extern std::atomic<uint64_t> gOrbisFrameHeartbeat;
extern std::atomic<const char*> gOrbisWaitSite;

class OrbisWaitScope final {
  public:
    explicit OrbisWaitScope(const char* site) noexcept
        : mPrevious(gOrbisWaitSite.exchange(site, std::memory_order_relaxed)) {}
    ~OrbisWaitScope() { gOrbisWaitSite.store(mPrevious, std::memory_order_relaxed); }
    OrbisWaitScope(const OrbisWaitScope&) = delete;
    OrbisWaitScope& operator=(const OrbisWaitScope&) = delete;

  private:
    const char* mPrevious;
};

} // namespace Fast

#endif

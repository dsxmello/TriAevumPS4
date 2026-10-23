#pragma once

// PlayStation 4 (OpenOrbis) Fast3D backends. The window backend (VideoOut +
// SDL joystick/audio, no SDL video) and the rendering API (RADV on GNM:
// native PICA, Fast3D UI draws and ImGui) are implemented by the PS4 port
// (triaevum-ps4-vk ports/ps4-product), not by this tree; these are the hooks
// the shared window/GUI code calls under __ORBIS__.
#if defined(__ORBIS__)

#include <atomic>
#include <cstdint>

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
// DualShock. NewFrame returns true on the L3+R3 press that stands in for F1.
void OrbisImGuiPlatformInit();
bool OrbisImGuiPlatformNewFrame();

// Process start (before main's work): system modules, boot log on
// /data/triaevum_boot.log, chdir to /data/triaevum, the DualShock mapping.
void OrbisProcessInit();

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

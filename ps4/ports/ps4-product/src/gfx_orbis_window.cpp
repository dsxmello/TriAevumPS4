// TriAevum PS4 product - Fast3D window backend: the TV through VideoOut
// (ps4_display), SDL for events/joystick/audio only. SDL's PS4 video driver
// would bring up Piglet, so SDL_INIT_VIDEO is never requested; the Switch
// product's gfx_sdl2 role is split between this class and the renderer.

#include "fast/backends/gfx_orbis.h"
#include "fast/backends/gfx_window_manager_api.h"
#include "fast/Fast3dGui.h"
#include "fast/Fast3dWindow.h"
#include "ship/Context.h"

#include <SDL2/SDL.h>

#include <chrono>

#include "ps4_display.h"

namespace Fast {
namespace {

class GfxWindowBackendOrbis final : public GfxWindowBackend {
  public:
    void Init(const char*, const char*, bool, uint32_t, uint32_t, int32_t, int32_t) override {
        auto& display = triaevum::ps4::Ps4Display::Instance();
        display.Open();
        SDL_InitSubSystem(SDL_INIT_EVENTS);
        mFullScreen = true;
        mStart = std::chrono::steady_clock::now();
        GuiWindowInitData windowImpl{};
        windowImpl.Gx2 = {display.Width(), display.Height()};
        windowImpl.Backend = WindowBackend::FAST3D_ORBIS_VULKAN;
        std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())
            ->Init(windowImpl);
    }
    void Close() override { mIsRunning = false; }
    void SetKeyboardCallbacks(bool (*onKeyDown)(int), bool (*onKeyUp)(int),
                              void (*onAllKeysUp)()) override {
        mOnKeyDown = onKeyDown;
        mOnKeyUp = onKeyUp;
        (void)onAllKeysUp;
    }
    void SetMouseCallbacks(bool (*onDown)(int), bool (*onUp)(int)) override {
        mOnMouseButtonDown = onDown;
        mOnMouseButtonUp = onUp;
    }
    void SetFullscreenChangedCallback(void (*callback)(bool)) override {
        mOnFullscreenChanged = callback;
    }
    void SetFullscreen(bool) override {}
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override { *refreshRate = 60; }
    void SetCursorVisibility(bool) override {}
    void SetMousePos(int32_t, int32_t) override {}
    void GetMousePos(int32_t* x, int32_t* y) override { *x = *y = 0; }
    void GetMouseDelta(int32_t* x, int32_t* y) override { *x = *y = 0; }
    void GetMouseWheel(float* x, float* y) override { *x = *y = 0.0F; }
    bool GetMouseState(uint32_t) override { return false; }
    void SetMouseCapture(bool) override {}
    bool IsMouseCaptured() override { return false; }
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override {
        const auto& display = triaevum::ps4::Ps4Display::Instance();
        *width = display.Width();
        *height = display.Height();
        *posX = 0;
        *posY = 0;
    }
    void SetDimensions(uint32_t, uint32_t, int32_t, int32_t) override {}
    Ship::WindowRect GetPrimaryMonitorRect() override {
        const auto& display = triaevum::ps4::Ps4Display::Instance();
        return {0, 0, static_cast<int32_t>(display.Width()), static_cast<int32_t>(display.Height())};
    }
    void HandleEvents() override {
        // Controller hot-plug events stay queued for the controller device
        // manager (the gfx_sdl2 split); everything else is drained here.
        SDL_PumpEvents();
        SDL_Event event;
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT,
                              SDL_CONTROLLERDEVICEADDED - 1) > 0) {
        }
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED + 1,
                              SDL_LASTEVENT) > 0) {
        }
    }
    bool IsFrameReady() override { return true; }
    void SwapBuffersBegin() override { triaevum::ps4::Ps4Display::Instance().Flip(); }
    void SwapBuffersEnd() override {}
    double GetTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
    }
    int GetTargetFps() override { return static_cast<int>(mTargetFps); }
    void SetTargetFps(int fps) override { mTargetFps = static_cast<uint32_t>(fps); }
    void SetMaxFrameLatency(int) override {}
    const char* GetKeyName(int) override { return ""; }
    bool CanDisableVsync() override { return false; }
    bool IsRunning() override { return mIsRunning; }
    void Destroy() override {}
    bool IsFullscreen() override { return true; }

  private:
    std::chrono::steady_clock::time_point mStart{};
};

} // namespace

GfxWindowBackend* CreateOrbisWindowBackend() {
    return new GfxWindowBackendOrbis();
}

} // namespace Fast

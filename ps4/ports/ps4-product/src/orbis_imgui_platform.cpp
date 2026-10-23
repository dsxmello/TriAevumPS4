// TriAevum PS4 product - ImGui platform backend. There is no SDL window (the
// PS4 never initializes SDL video), so imgui_impl_sdl2 cannot run; this feeds
// ImGui the VideoOut extent, the frame time and gamepad navigation from the
// first SDL game controller (the DualShock through SDL's scePad driver).

#include "fast/backends/gfx_orbis.h"

#include <imgui.h>
#include <SDL2/SDL.h>

#include <chrono>

#include "ps4_display.h"

namespace Fast {
namespace {

SDL_GameController* gController = nullptr;
std::chrono::steady_clock::time_point gLastFrame{};
bool gComboHeld = false;

SDL_GameController* Controller() {
    if (gController != nullptr && SDL_GameControllerGetAttached(gController)) {
        return gController;
    }
    if (gController != nullptr) {
        SDL_GameControllerClose(gController);
        gController = nullptr;
    }
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (SDL_IsGameController(index)) {
            gController = SDL_GameControllerOpen(index);
            if (gController != nullptr) break;
        }
    }
    return gController;
}

void Button(ImGuiIO& io, SDL_GameController* pad, ImGuiKey key, SDL_GameControllerButton button) {
    io.AddKeyEvent(key, SDL_GameControllerGetButton(pad, button) != 0);
}

void Analog(ImGuiIO& io, SDL_GameController* pad, ImGuiKey key, SDL_GameControllerAxis axis,
            float from, float to) {
    float value = static_cast<float>(SDL_GameControllerGetAxis(pad, axis)) / 32767.0F;
    value = (value - from) / (to - from);
    value = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
    io.AddKeyAnalogEvent(key, value > 0.1F, value);
}

} // namespace

void OrbisImGuiPlatformInit() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_orbis";
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    io.MouseDrawCursor = false;
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    gLastFrame = std::chrono::steady_clock::now();
}

bool OrbisImGuiPlatformNewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    const auto& display = triaevum::ps4::Ps4Display::Instance();
    io.DisplaySize = ImVec2(static_cast<float>(display.Width()),
                            static_cast<float>(display.Height()));
    io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - gLastFrame).count();
    io.DeltaTime = delta > 0.0F ? delta : 1.0F / 60.0F;
    gLastFrame = now;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

    SDL_GameControllerUpdate();
    SDL_GameController* pad = Controller();
    if (pad == nullptr) {
        gComboHeld = false;
        return false;
    }
    // Sony layout: cross confirms (FaceDown), circle cancels (FaceRight).
    Button(io, pad, ImGuiKey_GamepadStart, SDL_CONTROLLER_BUTTON_START);
    Button(io, pad, ImGuiKey_GamepadBack, SDL_CONTROLLER_BUTTON_BACK);
    Button(io, pad, ImGuiKey_GamepadFaceDown, SDL_CONTROLLER_BUTTON_A);
    Button(io, pad, ImGuiKey_GamepadFaceRight, SDL_CONTROLLER_BUTTON_B);
    Button(io, pad, ImGuiKey_GamepadFaceLeft, SDL_CONTROLLER_BUTTON_X);
    Button(io, pad, ImGuiKey_GamepadFaceUp, SDL_CONTROLLER_BUTTON_Y);
    Button(io, pad, ImGuiKey_GamepadDpadUp, SDL_CONTROLLER_BUTTON_DPAD_UP);
    Button(io, pad, ImGuiKey_GamepadDpadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    Button(io, pad, ImGuiKey_GamepadDpadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    Button(io, pad, ImGuiKey_GamepadDpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    Button(io, pad, ImGuiKey_GamepadL1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    Button(io, pad, ImGuiKey_GamepadR1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    Button(io, pad, ImGuiKey_GamepadL3, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    Button(io, pad, ImGuiKey_GamepadR3, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    Analog(io, pad, ImGuiKey_GamepadL2, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0.0F, 1.0F);
    Analog(io, pad, ImGuiKey_GamepadR2, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0.0F, 1.0F);
    constexpr float kDeadZone = 0.25F;
    Analog(io, pad, ImGuiKey_GamepadLStickLeft, SDL_CONTROLLER_AXIS_LEFTX, -kDeadZone, -1.0F);
    Analog(io, pad, ImGuiKey_GamepadLStickRight, SDL_CONTROLLER_AXIS_LEFTX, kDeadZone, 1.0F);
    Analog(io, pad, ImGuiKey_GamepadLStickUp, SDL_CONTROLLER_AXIS_LEFTY, -kDeadZone, -1.0F);
    Analog(io, pad, ImGuiKey_GamepadLStickDown, SDL_CONTROLLER_AXIS_LEFTY, kDeadZone, 1.0F);
    Analog(io, pad, ImGuiKey_GamepadRStickLeft, SDL_CONTROLLER_AXIS_RIGHTX, -kDeadZone, -1.0F);
    Analog(io, pad, ImGuiKey_GamepadRStickRight, SDL_CONTROLLER_AXIS_RIGHTX, kDeadZone, 1.0F);
    Analog(io, pad, ImGuiKey_GamepadRStickUp, SDL_CONTROLLER_AXIS_RIGHTY, -kDeadZone, -1.0F);
    Analog(io, pad, ImGuiKey_GamepadRStickDown, SDL_CONTROLLER_AXIS_RIGHTY, kDeadZone, 1.0F);

    const bool combo = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) &&
                       SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    const bool pressed = combo && !gComboHeld;
    gComboHeld = combo;
    return pressed;
}

} // namespace Fast

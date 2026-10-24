// TriAevum PS4 product - the console Forge at boot (fast/backends/gfx_orbis.h
// OrbisPrepareGame; ps4/docs/FASE6-FORGE.md).
//
// The user sends their own decrypted ROM (.3ds/.cci) to /data/triaevum/forge
// over FTP. src/forge/forge_core.cpp prepares the game from it exactly like
// the desktop Forge (recipe match, USA -> canonical EUR adaptation,
// verification, process manifest) and replaces /data/triaevum/game only when
// everything verified.
//
// The Forge screen (laid out like the desktop TriAevum Forge) opens when a
// ROM in the folder is not prepared yet, when there is no game, or when L1+R1
// are held at start. It draws with its own Vulkan backend and ImGui; "Launch
// game" restarts the app (sceSystemServiceLoadExec) so the game starts in a
// fresh process with the display to itself. Without the screen the game
// starts directly; the TopScreen pack and configuration come from the PKG
// when missing.

#include "fast/backends/gfx_orbis.h"
#include "forge/forge_core.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <SDL2/SDL.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include <sys/stat.h>

#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include "ps4_display.h"
#include "ps4_vk_pica_backend.h"
#include "ps4_vk_pica_backend_impl.h"

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                              const char* name);

namespace {
namespace fs = std::filesystem;
namespace forge = triaevum::forge;
using Backend = triaevum::ps4::Ps4VkPicaRenderBackend;

constexpr char kDrop[] = "/data/triaevum/forge";
constexpr char kWork[] = "/data/triaevum/forge/.work";
constexpr char kGame[] = "/data/triaevum/game";
constexpr char kError[] = "/data/triaevum/forge/forge-error.txt";
constexpr char kTopScreen[] = "/data/triaevum/mods/topscreen";
constexpr char kTopScreenConfig[] = "/data/triaevum/config/topscreen_ui.json";
constexpr char kAppForge[] = "/app0/forge";
constexpr char kAppLinkAnimations[] = "/app0/resources/topscreen/singlescreen-3.0/zelda_link_ext.zar";
constexpr char kEboot[] = "/app0/eboot.bin";
constexpr int kStateFormat = 1;

// ---- the game and the drop folder -------------------------------------------

struct RomFile {
    fs::path Path;
    uint64_t Bytes = 0;
};

// The newest .3ds/.cci in the drop folder.
std::optional<RomFile> FindRom() {
    std::error_code error;
    std::optional<RomFile> newest;
    fs::file_time_type newestTime{};
    for (const auto& entry : fs::directory_iterator(kDrop, error)) {
        if (!entry.is_regular_file(error)) continue;
        std::string extension = entry.path().extension().string();
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".3ds" && extension != ".cci") continue;
        const auto time = entry.last_write_time(error);
        if (!newest || time > newestTime) {
            newest = RomFile{entry.path(), static_cast<uint64_t>(entry.file_size(error))};
            newestTime = time;
        }
    }
    return newest;
}

// What the state remembers of the ROM: preparing it again reads the whole
// image, so a prepared ROM is recognized by name, size and modification time.
nlohmann::json RomKey(const fs::path& rom) {
    struct stat info {};
    ::stat(rom.c_str(), &info);
    return {{"name", rom.filename().string()},
            {"bytes", static_cast<uint64_t>(info.st_size)},
            {"mtime", static_cast<int64_t>(info.st_mtime)}};
}

nlohmann::json LoadState() {
    std::error_code error;
    const fs::path path = fs::path(kGame) / "forge-state.json";
    if (!fs::is_regular_file(path, error)) return nullptr;
    try {
        const auto bytes = forge::ReadFile(path);
        return nlohmann::json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception&) {
        return nullptr;
    }
}

bool GameReady() {
    std::error_code error;
    return fs::is_regular_file(fs::path(kGame) / "process-manifest.json", error) &&
           fs::is_regular_file(fs::path(kGame) / "inputs/code.bin", error) &&
           fs::is_regular_file(fs::path(kGame) / "inputs/romfs.bin", error);
}

bool RomPrepared(const RomFile& rom) {
    const nlohmann::json state = LoadState();
    return GameReady() && state.is_object() && state.value("state_format", 0) == kStateFormat &&
           state.value("rom", nlohmann::json()) == RomKey(rom.Path);
}

void CopyIfMissing(const fs::path& from, const fs::path& to) {
    std::error_code error;
    if (fs::is_regular_file(to, error)) return;
    fs::create_directories(to.parent_path(), error);
    try {
        forge::CopyFile(from, to);
        std::printf("[forge] %s -> %s: installed\n", from.c_str(), to.c_str());
    } catch (const std::exception& failure) {
        std::printf("[forge] %s -> %s: %s\n", from.c_str(), to.c_str(), failure.what());
    }
}

void InstallDefaults() {
    CopyIfMissing(fs::path(kAppForge) / "topscreen/atlas_overrides.o3tu",
                  fs::path(kTopScreen) / "atlas_overrides.o3tu");
    CopyIfMissing(kAppLinkAnimations, fs::path(kTopScreen) / "zelda_link_ext.zar");
    CopyIfMissing(fs::path(kAppForge) / "topscreen_ui.example.json", kTopScreenConfig);
}

// ---- preparation on a worker thread ------------------------------------------

struct Stage {
    const char* Name;
    const char* Text;
    double Start, Weight;  // share of the whole preparation
};
// Measured on the console's storage: every stage is one pass over the RomFS.
constexpr Stage kStages[] = {
    {"extract", "Extracting the ROM", 0.00, 0.30},
    {"check", "Checking the extracted data", 0.30, 0.20},
    {"adapt", "Adapting the USA revision to the canonical EUR runtime", 0.50, 0.25},
    {"verify", "Verifying the game data", 0.75, 0.25},
};

class PrepareJob final {
  public:
    ~PrepareJob() {
        if (mThread.joinable()) mThread.join();
    }
    bool Running() const { return mRunning.load(); }
    void Start(const RomFile& rom) {
        if (mThread.joinable()) mThread.join();
        {
            std::lock_guard lock(mMutex);
            mStatus = "Preparing " + rom.Path.filename().string() + "...";
            mFailed = false;
            mFraction = 0.0;
        }
        mRunning = true;
        mThread = std::thread([this, rom] { Run(rom); });
    }
    void Read(std::string& status, double& fraction, bool& failed, bool& succeeded) {
        std::lock_guard lock(mMutex);
        status = mStatus;
        fraction = mFraction;
        failed = mFailed;
        succeeded = mSucceeded;
    }

  private:
    void Report(std::string_view stage, double fraction) {
        for (const Stage& known : kStages) {
            if (stage != known.Name) continue;
            std::lock_guard lock(mMutex);
            mStatus = std::string(known.Text) + "...";
            mFraction = known.Start + known.Weight * fraction;
        }
    }
    void Run(RomFile rom) {
        Fast::OrbisRegisterThread("forge");
        const auto started = std::chrono::steady_clock::now();
        try {
            const forge::PrepareResult result = forge::PrepareTitle(
                {rom.Path, fs::path(kAppForge) / "recipes/oot3d.json", kAppForge, kWork, kGame},
                [this](std::string_view stage, double fraction) { Report(stage, fraction); });
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const nlohmann::json state{
                {"format", "triaevum_ps4_forge_state_v1"},
                {"state_format", kStateFormat},
                {"build", TRIAEVUM_PS4_BUILD_VERSION},
                {"rom", RomKey(rom.Path)},
                {"recipe", result.RecipeId},
                {"program_id", result.ProgramId},
                {"inputs", {{"code", result.Code.Json()}, {"exheader", result.Exheader.Json()},
                            {"romfs", result.Romfs.Json()}}},
                {"adaptation", result.Receipt},
                {"seconds", seconds}};
            forge::WriteFileAtomic(fs::path(kGame) / "forge-state.json", state.dump(2) + "\n");
            forge::RemoveFile(kError);
            InstallDefaults();
            char text[256];
            std::snprintf(text, sizeof(text),
                          "Installed %s in %.0f s. Select \"Launch game\" to play.",
                          result.RecipeId.c_str(), seconds);
            std::printf("[forge] %s\n", text);
            std::lock_guard lock(mMutex);
            mStatus = text;
            mFraction = 1.0;
            mSucceeded = true;
        } catch (const std::exception& error) {
            const std::string message = error.what();
            std::printf("[forge] preparation failed: %s\n", message.c_str());
            try {
                forge::WriteFileAtomic(kError, message + "\n");
                forge::RemoveTree(kWork);
            } catch (const std::exception& cleanup) {
                std::printf("[forge] %s\n", cleanup.what());
            }
            std::lock_guard lock(mMutex);
            mStatus = "The game was not prepared: " + message;
            mFailed = true;
        }
        mRunning = false;
    }
    std::thread mThread;
    std::atomic<bool> mRunning{false};
    std::mutex mMutex;
    std::string mStatus;
    double mFraction = 0.0;
    bool mFailed = false;
    bool mSucceeded = false;
};

// ---- the screen ----------------------------------------------------------------

PFN_vkVoidFunction ImGuiVulkanLoader(const char* name, void* user) {
    auto* vk = static_cast<Backend::Impl*>(user);
    PFN_vkVoidFunction function = vk->vk.vkGetDeviceProcAddr(vk->device, name);
    if (function == nullptr) function = vk_icdGetInstanceProcAddr(vk->instance, name);
    // No Vulkan WSI on the PS4: imgui's window helpers, never called, want them.
    if (function == nullptr && std::strstr(name, "KHR") != nullptr) {
        static const auto unavailable = +[]() -> VkResult { return VK_ERROR_EXTENSION_NOT_PRESENT; };
        function = reinterpret_cast<PFN_vkVoidFunction>(unavailable);
    }
    return function;
}

// The desktop Forge's look: a light dialog, sectioned, one column.
void ApplyStyle(float scale) {
    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(48, 40);
    style.FramePadding = ImVec2(14, 8);
    style.ItemSpacing = ImVec2(14, 12);
    style.FrameRounding = 3.0F;
    style.FrameBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.ChildRounding = 2.0F;
    style.ScaleAllSizes(scale);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.86F, 0.86F, 0.86F, 1.0F);
    colors[ImGuiCol_ChildBg] = ImVec4(0.86F, 0.86F, 0.86F, 1.0F);
    colors[ImGuiCol_Border] = ImVec4(0.62F, 0.62F, 0.62F, 1.0F);
    colors[ImGuiCol_FrameBg] = ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
    colors[ImGuiCol_Button] = ImVec4(0.90F, 0.90F, 0.90F, 1.0F);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.80F, 0.86F, 0.94F, 1.0F);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.70F, 0.78F, 0.90F, 1.0F);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.27F, 0.40F, 0.55F, 1.0F);
    colors[ImGuiCol_NavCursor] = ImVec4(0.20F, 0.45F, 0.85F, 1.0F);
}

void Section(const char* title, float height, const std::function<void()>& body) {
    ImGui::Spacing();
    ImGui::TextUnformatted(title);
    ImGui::BeginChild(title, ImVec2(0, height), ImGuiChildFlags_Borders);
    body();
    ImGui::EndChild();
}

struct ScreenState {
    std::optional<RomFile> Rom;
    std::chrono::steady_clock::time_point LastScan{};
    bool LaunchRequested = false;
    std::string LaunchError;
    bool FirstFrame = true;
    bool DeleteOffered = false;  // after a successful preparation, once
    std::string DeleteResult;
};

// The ROM is only needed to prepare the game: offer to free its space.
void DrawDeletePrompt(ScreenState& screen, float width) {
    constexpr const char* kTitle = "Delete the ROM?";
    if (!screen.DeleteOffered && screen.Rom) {
        screen.DeleteOffered = true;
        ImGui::OpenPopup(kTitle);
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always,
                            ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(width * 0.7F, 0));
    if (!ImGui::BeginPopupModal(kTitle, nullptr,
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextWrapped(
        "The game is installed. TriAevum does not need %s any more: delete it from "
        "/data/triaevum/forge/ to free %.0f MB? Keep it to prepare the game again later.",
        screen.Rom->Path.filename().c_str(), screen.Rom->Bytes / 1048576.0);
    ImGui::Spacing();
    const float button = width * 0.22F;
    if (ImGui::Button("Delete ROM", ImVec2(button, 0))) {
        try {
            forge::RemoveFile(screen.Rom->Path);
            screen.DeleteResult = "The ROM was deleted.";
            screen.Rom.reset();
        } catch (const std::exception& error) {
            screen.DeleteResult = error.what();
        }
        std::printf("[forge] delete ROM: %s\n", screen.DeleteResult.c_str());
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep ROM", ImVec2(button, 0))) ImGui::CloseCurrentPopup();
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
}

void DrawForge(ScreenState& screen, PrepareJob& job, ImFont* title) {
    const ImGuiIO& io = ImGui::GetIO();
    const float width = std::min(io.DisplaySize.x - 160.0F, 1500.0F);
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - width) / 2, 40));
    ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y - 80));
    // Gamepad navigation acts on the focused window: there is no mouse click.
    if (screen.FirstFrame) ImGui::SetNextWindowFocus();
    screen.FirstFrame = false;
    ImGui::Begin("TriAevum Forge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    const bool running = job.Running();
    std::string status;
    double fraction = 0.0;
    bool failed = false, succeeded = false;
    job.Read(status, fraction, failed, succeeded);

    // The folder is watched: the ROM may arrive over FTP while this is open.
    const auto now = std::chrono::steady_clock::now();
    if (!running && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        now - screen.LastScan > std::chrono::seconds(2)) {
        screen.Rom = FindRom();
        screen.LastScan = now;
    }

    ImGui::PushFont(title);
    ImGui::TextUnformatted("TriAevum Forge");
    ImGui::PopFont();
    ImGui::TextWrapped(
        "Send your own decrypted Nintendo 3DS cartridge image (.3ds or .cci) over FTP to "
        "/data/triaevum/forge/. Forge accepts .3ds and .cci ROMs, extracts the required native "
        "data, and does not handle encryption keys.");
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Decrypted ROM");
    ImGui::SameLine(width * 0.20F);
    std::string romText = screen.Rom ? screen.Rom->Path.filename().string() : std::string();
    if (screen.Rom) {
        char size[48];
        std::snprintf(size, sizeof(size), "  (%.0f MB)", screen.Rom->Bytes / 1048576.0);
        romText += size;
    }
    ImGui::SetNextItemWidth(width * 0.62F);
    ImGui::BeginDisabled();
    ImGui::InputText("##rom", romText.data(), romText.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Look again")) {
        screen.Rom = FindRom();
        screen.LastScan = now;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Private generated data: %s", kGame);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Game language");
    ImGui::SameLine(width * 0.20F);
    ImGui::SetNextItemWidth(width * 0.62F);
    ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##language", GameReady() ? "Chosen in game" : "Available after ROM preparation")) {
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    const float line = ImGui::GetTextLineHeightWithSpacing();
    Section("HD UI textures (recommended)", line * 6.6F, [&] {
        ImGui::TextWrapped(
            "OoT3D Vanilla UI/HUD 3.2 by chubbycubby (credits: HenrikoMagnifico, M-1): HD "
            "textures for the HUD, the menus and TopScreen. Licensed CC BY-NC-ND 4.0: TriAevum "
            "does not distribute it. Forge downloads the author's original archive from "
            "GameBanana, verifies it and keeps it in your private data folder.");
        ImGui::BeginDisabled();
        ImGui::Button("Download HD textures");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Not available yet on PS4: coming in a later version.");
    });
    Section("Optional mods", line * 6.6F, [&] {
        ImGui::TextWrapped(
            "Vanilla+ - Link Remodel And HD Item Models 1.9 by HugAMuffin: higher-fidelity models "
            "for Link and his items. Licensed CC BY-NC-ND 4.0: TriAevum does not distribute it. "
            "Forge downloads the author's original archive from GameBanana, verifies it and "
            "applies it to your local game data only.");
        bool vanillaPlus = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Use Vanilla+ - Link Remodel And HD Item Models", &vanillaPlus);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Not available yet on PS4.");
    });

    ImGui::Spacing();
    ImGui::Separator();
    if (status.empty()) {
        if (running) status = "Preparing...";
        else if (screen.Rom && RomPrepared(*screen.Rom)) status = "The game is installed from this ROM.";
        else if (screen.Rom) status = "Ready to prepare " + screen.Rom->Path.filename().string() + ".";
        else if (GameReady()) status = "A game is installed. Send a ROM to prepare it again.";
        else status = "No active title is installed. Send a decrypted ROM to /data/triaevum/forge/.";
    }
    if (succeeded && !screen.DeleteResult.empty()) status += " " + screen.DeleteResult;
    if (!screen.LaunchError.empty()) status = screen.LaunchError;
    if (failed || !screen.LaunchError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65F, 0.10F, 0.10F, 1.0F));
    }
    ImGui::TextWrapped("%s", status.c_str());
    if (failed || !screen.LaunchError.empty()) ImGui::PopStyleColor();
    ImGui::ProgressBar(static_cast<float>(fraction), ImVec2(-1, line * 0.7F), "");

    const float buttonWidth = width * 0.22F;
    ImGui::SetCursorPosX(width - 2 * buttonWidth - ImGui::GetStyle().ItemSpacing.x -
                         ImGui::GetStyle().WindowPadding.x);
    ImGui::BeginDisabled(running || !screen.Rom);
    if (ImGui::Button("Prepare and install", ImVec2(buttonWidth, 0))) {
        screen.LaunchError.clear();
        job.Start(*screen.Rom);
    }
    if (screen.Rom && !GameReady()) ImGui::SetItemDefaultFocus();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || !GameReady());
    if (ImGui::Button("Launch game", ImVec2(buttonWidth, 0))) screen.LaunchRequested = true;
    if (GameReady()) ImGui::SetItemDefaultFocus();
    ImGui::EndDisabled();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - line - ImGui::GetStyle().WindowPadding.y);
    ImGui::TextDisabled("Cross: select    D-pad: move    Hold L1+R1 when starting TriAevum to open Forge again");
    if (succeeded) DrawDeletePrompt(screen, width);
    ImGui::End();
}

// Draws the Forge until the user launches the game; then restarts the app.
void RunForgeScreen() {
    auto& display = triaevum::ps4::Ps4Display::Instance();
    display.Open();
    auto pica = Backend::Create([](const char* text) { std::printf("[forge] %s\n", text); },
                                display.Width(), display.Height());
    if (pica == nullptr) {
        std::printf("[forge] the Vulkan renderer did not start\n");
        return;
    }
    pica->SetImmediatePresentation(true);
    void* buffers[Backend::kScanoutCount];
    for (uint32_t index = 0; index < Backend::kScanoutCount; ++index) {
        buffers[index] = pica->ScanoutPixels(index);
    }
    display.Register(buffers, Backend::kScanoutCount, display.Width(), display.Height(),
                     pica->ScanoutPitchPixels());
    Backend::Impl* vk = pica->Internal();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const float scale = static_cast<float>(display.Height()) / 1080.0F;
    ImFont* body = io.Fonts->AddFontFromFileTTF("/app0/forge/DroidSans.ttf", 30.0F * scale);
    ImFont* title = io.Fonts->AddFontFromFileTTF("/app0/forge/DroidSans.ttf", 58.0F * scale);
    if (body == nullptr || title == nullptr) {
        io.Fonts->Clear();
        ImFontConfig config;
        config.SizePixels = 26.0F * scale;
        body = title = io.Fonts->AddFontDefault(&config);
    }
    io.FontDefault = body;
    ApplyStyle(scale);
    Fast::OrbisImGuiPlatformInit();
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, ImGuiVulkanLoader, vk)) {
        std::printf("[forge] ImGui Vulkan entry points unavailable\n");
        return;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = vk->instance;
    info.PhysicalDevice = vk->gpu;
    info.Device = vk->device;
    info.QueueFamily = 0;
    info.Queue = vk->queue;
    info.RenderPass = vk->scanoutPass;
    info.MinImageCount = 2;
    info.ImageCount = 2;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineCache = vk->pipelineCache;
    info.DescriptorPoolSize = 64;
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("[forge] ImGui_ImplVulkan_Init failed\n");
        return;
    }

    PrepareJob job;
    ScreenState screen;
    for (;;) {
        Fast::gOrbisFrameHeartbeat.fetch_add(1, std::memory_order_relaxed);
        // SDL finds the DualShock (and its hot-plug) while pumping events.
        SDL_PumpEvents();
        display.WaitPendingFlip();
        pica->BeginFrame(display.NextBuffer());
        vk->OpenScanoutPass();
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.clearValue.color = {{0.70F, 0.70F, 0.70F, 1.0F}};
        const VkClearRect rect = {{{0, 0}, {vk->scanoutWidth, vk->scanoutHeight}}, 0, 1};
        vk->vk.vkCmdClearAttachments(vk->mainCb, 1, &clear, 1, &rect);
        vk->workRecorded = true;

        ImGui_ImplVulkan_NewFrame();
        Fast::OrbisImGuiPlatformNewFrame();
        ImGui::NewFrame();
        DrawForge(screen, job, title);
        ImGui::Render();
        vk->OpenScanoutPass();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vk->mainCb);
        pica->EndFrame();
        display.Flip();

        if (screen.LaunchRequested && !job.Running()) {
            screen.LaunchRequested = false;
            std::printf("[forge] launching the game\n");
            std::fflush(stdout);
            // A fresh process: the game brings up its own renderer.
            const int result = sceSystemServiceLoadExec(kEboot, nullptr);
            char text[96];
            std::snprintf(text, sizeof(text),
                          "TriAevum could not restart itself (%#x): close and open it again.", result);
            screen.LaunchError = text;
        }
    }
}

bool ForgeRequestedByUser() {
    // L1+R1 held while TriAevum starts. SDL's PS4 joystick driver asks the
    // user service for the logged-in users (0x80960002 before it starts),
    // and it finds the pad while pumping events.
    sceUserServiceInitialize(nullptr);
    SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    for (int attempt = 0; attempt < 30 && SDL_NumJoysticks() == 0; ++attempt) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    // The first states after opening the pad are empty: watch it for up to
    // a second (the buttons are already held when the app starts).
    SDL_GameController* pad = nullptr;
    for (int index = 0; index < SDL_NumJoysticks() && pad == nullptr; ++index) {
        if (SDL_IsGameController(index)) pad = SDL_GameControllerOpen(index);
    }
    if (pad == nullptr) {
        std::printf("[forge] L1+R1: no controller\n");
        return false;
    }
    bool held = false, l1 = false, r1 = false;
    for (int attempt = 0; attempt < 50 && !held; ++attempt) {
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        l1 = l1 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        r1 = r1 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        held = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
               SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        if (!held) SDL_Delay(20);
    }
    std::printf("[forge] L1+R1: %s (L1 %s, R1 %s)\n", held ? "held" : "not held",
                l1 ? "seen" : "not seen", r1 ? "seen" : "not seen");
    SDL_GameControllerClose(pad);
    return held;
}

} // namespace

namespace Fast {

bool OrbisPrepareGame() {
    std::error_code ignored;
    fs::create_directories(kDrop, ignored);
    const std::optional<RomFile> rom = FindRom();
    const bool newRom = rom && !RomPrepared(*rom);
    const bool requested = ForgeRequestedByUser();
    std::printf("[forge] game %s, ROM %s%s\n", GameReady() ? "ready" : "missing",
                rom ? rom->Path.filename().c_str() : "none",
                newRom ? " (not prepared)" : (requested ? ", Forge requested (L1+R1)" : ""));
    if (!newRom && !requested && GameReady()) {
        InstallDefaults();
        return true;
    }
    RunForgeScreen();
    return false;
}

} // namespace Fast

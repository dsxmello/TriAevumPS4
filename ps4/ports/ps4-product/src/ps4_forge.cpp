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
#include "forge/forge_textures.h"
#include "ps4_forge_net.h"

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
constexpr char kData[] = "/data/triaevum";
constexpr char kDownloads[] = "/data/triaevum/downloads";
constexpr char kSettings[] = "/data/triaevum/config/forge.json";
constexpr char kTextureCatalog[] = "/app0/forge/ui_texture_pack.json";
// Written by the in-game menu's "Open TriAevum Forge" before it restarts.
constexpr char kOpenRequest[] = "/data/triaevum/forge/open-forge";
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
        triaevum::ps4::ForgeLog("%s -> %s: installed\n", from.c_str(), to.c_str());
    } catch (const std::exception& failure) {
        triaevum::ps4::ForgeLog("%s -> %s: %s\n", from.c_str(), to.c_str(), failure.what());
    }
}

void InstallDefaults() {
    CopyIfMissing(fs::path(kAppForge) / "topscreen/atlas_overrides.o3tu",
                  fs::path(kTopScreen) / "atlas_overrides.o3tu");
    CopyIfMissing(kAppLinkAnimations, fs::path(kTopScreen) / "zelda_link_ext.zar");
    CopyIfMissing(fs::path(kAppForge) / "topscreen_ui.example.json", kTopScreenConfig);
}

// ---- the HD UI texture pack -------------------------------------------------------

const forge::UiTexturePack* TexturePack() {
    static const std::optional<forge::UiTexturePack> pack = []() -> std::optional<forge::UiTexturePack> {
        try {
            return forge::UiTexturePack::Load(kTextureCatalog);
        } catch (const std::exception& error) {
            triaevum::ps4::ForgeLog("%s: %s\n", kTextureCatalog, error.what());
            return std::nullopt;
        }
    }();
    return pack ? &*pack : nullptr;
}

// An archive the user sent over FTP, or one Forge downloaded.
std::optional<fs::path> StoredTextureArchive(const forge::UiTexturePack& pack) {
    return forge::FindUiTextureArchive(pack, {kDrop, kDownloads});
}

// The desktop Forge's "Download the HD textures with Prepare and install
// when they are not stored yet" (on by default there too).
bool DownloadTexturesWithPrepare() {
    try {
        const auto bytes = forge::ReadFile(kSettings);
        return nlohmann::json::parse(bytes.begin(), bytes.end()).value("download_hd_textures_with_prepare", true);
    } catch (const std::exception&) {
        return true;
    }
}

void SetDownloadTexturesWithPrepare(bool enabled) {
    try {
        std::error_code error;
        fs::create_directories(fs::path(kSettings).parent_path(), error);
        forge::WriteFileAtomic(kSettings,
                               nlohmann::json{{"download_hd_textures_with_prepare", enabled}}.dump(2) + "\n");
    } catch (const std::exception& failure) {
        triaevum::ps4::ForgeLog("%s\n", failure.what());
    }
}

// ---- preparation on a worker thread ------------------------------------------

struct Stage {
    const char* Name;
    const char* Text;
};
constexpr Stage kStages[] = {
    {"extract", "Extracting the ROM"},
    {"check", "Checking the extracted data"},
    {"adapt", "Adapting the USA revision to the canonical EUR runtime"},
    {"verify", "Verifying the game data"},
    {"textures-download", "Downloading the HD textures"},
    {"textures-verify", "Verifying the HD texture archive"},
    {"textures-extract", "Installing the HD textures"},
};

// What one run does (the desktop Forge's "Prepare and install" and
// "Download HD textures").
struct Plan {
    std::optional<RomFile> Rom;   // prepare the game from it
    bool Textures = false;        // install the HD textures when stored
    bool DownloadTextures = false;  // ... downloading them when not stored
};

class ForgeJob final {
  public:
    ~ForgeJob() {
        if (mThread.joinable()) mThread.join();
    }
    bool Running() const { return mRunning.load(); }
    void Start(Plan plan) {
        if (mThread.joinable()) mThread.join();
        {
            std::lock_guard lock(mMutex);
            mStatus = "Starting...";
            mFailed = mSucceeded = mRomPrepared = false;
            mFraction = 0.0;
        }
        mRunning = true;
        mThread = std::thread([this, plan = std::move(plan)] { Run(plan); });
    }
    struct View {
        std::string Status;
        double Fraction = 0.0;
        bool Failed = false, Succeeded = false, RomPrepared = false;
    };
    View Read() {
        std::lock_guard lock(mMutex);
        return {mStatus, mFraction, mFailed, mSucceeded, mRomPrepared};
    }

  private:
    void Report(std::string_view stage, double fraction) {
        for (const Stage& known : kStages) {
            if (stage != known.Name) continue;
            std::lock_guard lock(mMutex);
            mStatus = mStep + std::string(known.Text) + "...";
            mFraction = fraction;
        }
    }
    void SetStep(int index, int count) {
        std::lock_guard lock(mMutex);
        mStep = count > 1 ? "Step " + std::to_string(index) + " of " + std::to_string(count) + ": " : "";
    }
    std::string PrepareRom(const RomFile& rom) {
        const auto started = std::chrono::steady_clock::now();
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
        InstallDefaults();
        {
            std::lock_guard lock(mMutex);
            mRomPrepared = true;
        }
        char text[160];
        std::snprintf(text, sizeof(text), "Installed %s in %.0f s.", result.RecipeId.c_str(), seconds);
        return text;
    }
    // The HD textures: "" when there was nothing to do.
    std::string InstallTextures(bool download) {
        const forge::UiTexturePack* pack = TexturePack();
        if (pack == nullptr) throw forge::ForgeError("the HD UI texture catalog is missing from the PKG");
        if (forge::UiTexturePackPrepared(*pack, kData, false)) return "";
        std::optional<fs::path> archive = StoredTextureArchive(*pack);
        if (!archive && !download) {
            return "The HD textures are not stored: the game uses the native textures.";
        }
        if (!archive) {
            const fs::path destination = fs::path(kDownloads) / pack->ArchiveName();
            std::string failure;
            for (const std::string& url : pack->Downloads()) {
                try {
                    triaevum::ps4::DownloadVerified(
                        url, destination, pack->Archive(),
                        [this](std::string_view stage, double fraction) { Report(stage, fraction); });
                    archive = destination;
                    break;
                } catch (const std::exception& error) {
                    failure = error.what();
                    triaevum::ps4::ForgeLog("%s: %s\n", url.c_str(), failure.c_str());
                }
            }
            if (!archive) throw forge::ForgeError("The HD textures were not downloaded: " + failure);
        }
        forge::ExtractUiTexturePack(*pack, *archive, kData,
                                    [this](std::string_view stage, double fraction) { Report(stage, fraction); });
        return pack->Title() + " installed: the game uses the HD textures.";
    }
    void Run(const Plan& plan) {
        Fast::OrbisRegisterThread("forge");
        std::string summary;
        try {
            const int count = (plan.Rom ? 1 : 0) + (plan.Textures ? 1 : 0);
            int step = 0;
            if (plan.Rom) {
                SetStep(++step, count);
                summary = PrepareRom(*plan.Rom);
                forge::RemoveFile(kError);
            }
            if (plan.Textures) {
                SetStep(++step, count);
                const std::string textures = InstallTextures(plan.DownloadTextures);
                if (!textures.empty()) summary += (summary.empty() ? "" : " ") + textures;
            }
            if (summary.empty()) summary = "Nothing to do: everything is installed.";
            if (GameReady()) summary += " Select \"Launch game\" to play.";
            triaevum::ps4::ForgeLog("%s\n", summary.c_str());
            std::lock_guard lock(mMutex);
            mStatus = summary;
            mFraction = 1.0;
            mSucceeded = true;
        } catch (const std::exception& error) {
            const std::string message = error.what();
            triaevum::ps4::ForgeLog("failed: %s\n", message.c_str());
            try {
                forge::WriteFileAtomic(kError, message + "\n");
                forge::RemoveTree(kWork);
            } catch (const std::exception& cleanup) {
                triaevum::ps4::ForgeLog("%s\n", cleanup.what());
            }
            std::lock_guard lock(mMutex);
            mStatus = (summary.empty() ? "" : summary + " ") + "Failed: " + message;
            mFailed = true;
        }
        mRunning = false;
    }

    std::thread mThread;
    std::atomic<bool> mRunning{false};
    std::mutex mMutex;
    std::string mStatus, mStep;
    double mFraction = 0.0;
    bool mFailed = false, mSucceeded = false, mRomPrepared = false;
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
    // The HD textures, refreshed with the folder scan.
    bool TexturesPrepared = false;
    std::optional<fs::path> TextureArchive;
    bool DownloadWithPrepare = DownloadTexturesWithPrepare();
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
        triaevum::ps4::ForgeLog("delete ROM: %s\n", screen.DeleteResult.c_str());
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep ROM", ImVec2(button, 0))) ImGui::CloseCurrentPopup();
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
}

void DrawForge(ScreenState& screen, ForgeJob& job, ImFont* title) {
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
    const ForgeJob::View view = job.Read();
    std::string status = view.Status;
    const double fraction = view.Fraction;
    const bool failed = view.Failed, succeeded = view.Succeeded;
    const forge::UiTexturePack* pack = TexturePack();

    // The folder is watched: the ROM may arrive over FTP while this is open.
    const auto now = std::chrono::steady_clock::now();
    if (!running && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        now - screen.LastScan > std::chrono::seconds(2)) {
        screen.Rom = FindRom();
        screen.LastScan = now;
        screen.TexturesPrepared = pack != nullptr && forge::UiTexturePackPrepared(*pack, kData, false);
        screen.TextureArchive = pack != nullptr ? StoredTextureArchive(*pack) : std::nullopt;
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
    Section("HD UI textures (recommended)", line * 8.2F, [&] {
        ImGui::TextWrapped(
            "OoT3D Vanilla UI/HUD 3.2 by chubbycubby (credits: HenrikoMagnifico, M-1): HD "
            "textures for the HUD, the menus and TopScreen, in the game language you choose here or "
            "in game. Licensed CC BY-NC-ND 4.0: TriAevum does not distribute it. Forge downloads the "
            "author's original archive from GameBanana, verifies it and keeps it in your private data "
            "folder. If the download does not work, send oot3d_vanilla_ui_hud_32.zip from the mod page "
            "to /data/triaevum/forge/ over FTP.");
        const bool needed = pack != nullptr && !screen.TexturesPrepared;
        ImGui::BeginDisabled(running || !needed || screen.TextureArchive.has_value());
        if (ImGui::Button("Download HD textures")) {
            screen.LaunchError.clear();
            job.Start({std::nullopt, true, true});
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(running);
        if (ImGui::Button("Look for the .zip")) screen.LastScan = {};
        ImGui::EndDisabled();
        ImGui::BeginDisabled(running || pack == nullptr);
        if (ImGui::Checkbox("Download the HD textures with \"Prepare and install\" when they are not stored yet",
                            &screen.DownloadWithPrepare)) {
            SetDownloadTexturesWithPrepare(screen.DownloadWithPrepare);
        }
        ImGui::EndDisabled();
        if (pack == nullptr) {
            ImGui::TextDisabled("The HD UI texture catalog is missing from this TriAevum PKG.");
        } else if (screen.TexturesPrepared) {
            ImGui::TextDisabled("Installed: the game uses the HD textures.");
        } else if (screen.TextureArchive) {
            ImGui::TextDisabled("Stored (%s): \"Prepare and install\" installs them.",
                                screen.TextureArchive->c_str());
        } else {
            ImGui::TextDisabled("Not stored yet: the HD textures (451 MB) are downloaded when you prepare "
                                "and install the title. Without them the game uses the native textures.");
        }
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
    // Prepare the ROM when it is new (or there is no game), and the HD
    // textures when they are stored or may be downloaded.
    const bool romWork = screen.Rom && (!GameReady() || !RomPrepared(*screen.Rom));
    const bool textureWork = pack != nullptr && !screen.TexturesPrepared &&
                             (screen.TextureArchive || screen.DownloadWithPrepare);
    ImGui::BeginDisabled(running || (!romWork && !(GameReady() && textureWork)));
    if (ImGui::Button("Prepare and install", ImVec2(buttonWidth, 0))) {
        screen.LaunchError.clear();
        screen.DeleteOffered = false;
        job.Start({romWork ? screen.Rom : std::nullopt, textureWork, screen.DownloadWithPrepare});
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
    if (succeeded && view.RomPrepared) DrawDeletePrompt(screen, width);
    ImGui::End();
}

// Draws the Forge until the user launches the game; then restarts the app.
void RunForgeScreen() {
    auto& display = triaevum::ps4::Ps4Display::Instance();
    display.Open();
    auto pica = Backend::Create([](const char* text) { std::printf("[forge] %s\n", text); },
                                display.Width(), display.Height());
    if (pica == nullptr) {
        triaevum::ps4::ForgeLog("the Vulkan renderer did not start\n");
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
        triaevum::ps4::ForgeLog("ImGui Vulkan entry points unavailable\n");
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
        triaevum::ps4::ForgeLog("ImGui_ImplVulkan_Init failed\n");
        return;
    }

    ForgeJob job;
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
            triaevum::ps4::ForgeLog("launching the game\n");
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
        triaevum::ps4::ForgeLog("L1+R1: no controller\n");
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
    triaevum::ps4::ForgeLog("L1+R1: %s (L1 %s, R1 %s)\n", held ? "held" : "not held",
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
    std::error_code requestError;
    const bool fromMenu = fs::is_regular_file(kOpenRequest, requestError);
    if (fromMenu) forge::RemoveFile(kOpenRequest);
    const bool requested = fromMenu || ForgeRequestedByUser();
    triaevum::ps4::ForgeLog("game %s, ROM %s%s\n", GameReady() ? "ready" : "missing",
                rom ? rom->Path.filename().c_str() : "none",
                newRom ? " (not prepared)" : (requested ? ", Forge requested (L1+R1)" : ""));
    if (!newRom && !requested && GameReady()) {
        InstallDefaults();
        return true;
    }
    RunForgeScreen();
    return false;
}

void OrbisOpenForge() {
    // A deliberate restart, not a failed launch: without its launch marker
    // the next start keeps the display settings (a marker younger than a
    // minute resets them, oot3d_demo_host_context.cpp ArmLaunchMarker).
    try {
        forge::RemoveFile("/data/triaevum/config/TriAevum.json.launching");
    } catch (const std::exception& error) {
        triaevum::ps4::ForgeLog("%s\n", error.what());
    }
    try {
        forge::WriteFileAtomic(kOpenRequest, "in-game menu\n");
    } catch (const std::exception& error) {
        triaevum::ps4::ForgeLog("%s\n", error.what());
        return;
    }
    triaevum::ps4::ForgeLog("restarting into Forge (in-game menu)\n");
    std::fflush(stdout);
    const int result = sceSystemServiceLoadExec(kEboot, nullptr);
    triaevum::ps4::ForgeLog("sceSystemServiceLoadExec = %#x\n", static_cast<unsigned>(result));
}

std::string OrbisForgeStatus() {
    const forge::UiTexturePack* pack = TexturePack();
    if (pack == nullptr) return "";
    return forge::UiTexturePackPrepared(*pack, kData, false)
               ? pack->Title() + " HD textures: installed."
               : "HD textures: not installed.";
}

std::vector<std::string> OrbisForgeLaunchArguments() {
    std::vector<std::string> arguments;
    const forge::UiTexturePack* pack = TexturePack();
    if (pack != nullptr && forge::UiTexturePackPrepared(*pack, kData, false)) {
        arguments = {"--ui-texture-pack", pack->Directory(kData).string(),
                     "--topscreen-ui-atlas-hd", pack->UiAtlasHd(kData).string()};
    }
    triaevum::ps4::ForgeLog("HD UI textures: %s\n", arguments.empty() ? "not installed" : "installed");
    return arguments;
}

} // namespace Fast

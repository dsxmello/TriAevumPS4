// TriAevum PS4 product - the console Forge at boot (fast/backends/gfx_orbis.h
// OrbisPrepareGame; ps4/docs/FASE6-FORGE.md).
//
// The user sends their own decrypted ROM (.3ds/.cci) to /data/triaevum/forge
// over FTP. When it is new, src/forge/forge_core.cpp prepares the game from it
// exactly like the desktop Forge (recipe match, USA -> canonical EUR
// adaptation, verification, process manifest) and replaces
// /data/triaevum/game only when everything verified. The TopScreen pack and
// configuration come from the PKG when missing. Progress and errors go to
// system notifications and the boot log.

#include "fast/backends/gfx_orbis.h"
#include "forge/forge_core.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>

#include <sys/stat.h>

#include <orbis/libkernel.h>

namespace {
namespace fs = std::filesystem;
namespace forge = triaevum::forge;

constexpr char kDrop[] = "/data/triaevum/forge";
constexpr char kWork[] = "/data/triaevum/forge/.work";
constexpr char kGame[] = "/data/triaevum/game";
constexpr char kError[] = "/data/triaevum/forge/forge-error.txt";
constexpr char kTopScreen[] = "/data/triaevum/mods/topscreen";
constexpr char kTopScreenConfig[] = "/data/triaevum/config/topscreen_ui.json";
constexpr char kAppForge[] = "/app0/forge";
constexpr char kAppLinkAnimations[] = "/app0/resources/topscreen/singlescreen-3.0/zelda_link_ext.zar";
constexpr int kStateFormat = 1;

void Notify(const std::string& text) {
    std::printf("[forge] %s\n", text.c_str());
    OrbisNotificationRequest request{};
    request.type = NotificationRequest;
    request.targetId = -1;
    request.useIconImageUri = 1;
    std::snprintf(request.message, sizeof(request.message), "TriAevum Forge\n%s", text.c_str());
    std::snprintf(request.iconUri, sizeof(request.iconUri), "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

// The newest .3ds/.cci in the drop folder.
std::optional<fs::path> FindRom() {
    std::error_code error;
    std::optional<fs::path> newest;
    fs::file_time_type newestTime{};
    for (const auto& entry : fs::directory_iterator(kDrop, error)) {
        if (!entry.is_regular_file(error)) continue;
        std::string extension = entry.path().extension().string();
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".3ds" && extension != ".cci") continue;
        const auto time = entry.last_write_time(error);
        if (!newest || time > newestTime) {
            newest = entry.path();
            newestTime = time;
        }
    }
    return newest;
}

// What the state remembers of the ROM: re-preparing it reads 2 GB, so a ROM
// already prepared is recognized by name, size and modification time.
nlohmann::json RomKey(const fs::path& rom) {
    struct stat info {};
    ::stat(rom.c_str(), &info);
    return {{"name", rom.filename().string()},
            {"bytes", static_cast<uint64_t>(info.st_size)},
            {"mtime", static_cast<int64_t>(info.st_mtime)}};
}

nlohmann::json LoadState() {
    try {
        const auto bytes = forge::ReadFile(fs::path(kGame) / "forge-state.json");
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

void CopyIfMissing(const fs::path& from, const fs::path& to) {
    std::error_code error;
    if (fs::is_regular_file(to, error)) return;
    fs::create_directories(to.parent_path(), error);
    fs::copy_file(from, to, error);
    std::printf("[forge] %s -> %s: %s\n", from.c_str(), to.c_str(),
                error ? error.message().c_str() : "installed");
}

const char* StageText(std::string_view stage) {
    if (stage == "extract") return "Extraindo a ROM";
    if (stage == "adapt") return "Adaptando a versao USA para a canonica EUR";
    if (stage == "verify") return "Verificando os arquivos do jogo";
    return nullptr;
}

void Prepare(const fs::path& rom) {
    Notify("Preparando o jogo a partir de " + rom.filename().string() +
           ". Isso leva alguns minutos; nao feche o app.");
    const auto started = std::chrono::steady_clock::now();
    std::string lastStage;
    int lastHalf = -1;
    const forge::ProgressFn progress = [&](std::string_view stage, double fraction) {
        // Notifications queue up on screen: one per stage, plus its midpoint.
        const int half = fraction >= 0.5 ? 1 : 0;
        if (stage == lastStage && half == lastHalf) return;
        const bool newStage = stage != lastStage;
        lastStage = stage;
        lastHalf = half;
        if (const char* text = StageText(stage)) {
            if (newStage) Notify(std::string(text) + "...");
            else if (half) Notify(std::string(text) + ": 50%");
        }
    };
    const forge::PrepareResult result = forge::PrepareTitle(
        {rom, fs::path(kAppForge) / "recipes/oot3d.json", kAppForge, kWork, kGame}, progress);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const nlohmann::json state{
        {"format", "triaevum_ps4_forge_state_v1"},
        {"state_format", kStateFormat},
        {"build", TRIAEVUM_PS4_BUILD_VERSION},
        {"rom", RomKey(rom)},
        {"recipe", result.RecipeId},
        {"program_id", result.ProgramId},
        {"inputs", {{"code", result.Code.Json()}, {"exheader", result.Exheader.Json()},
                    {"romfs", result.Romfs.Json()}}},
        {"adaptation", result.Receipt},
        {"seconds", seconds}};
    forge::WriteFileAtomic(fs::path(kGame) / "forge-state.json", state.dump(2) + "\n");
    std::error_code ignored;
    fs::remove(kError, ignored);
    char summary[128];
    std::snprintf(summary, sizeof(summary), "Jogo pronto (%s) em %.0f s.", result.RecipeId.c_str(), seconds);
    Notify(summary);
}

} // namespace

namespace Fast {

bool OrbisPrepareGame() {
    std::error_code ignored;
    fs::create_directories(kDrop, ignored);
    if (const auto rom = FindRom()) {
        const nlohmann::json state = LoadState();
        const bool prepared = GameReady() && state.is_object() &&
                              state.value("state_format", 0) == kStateFormat &&
                              state.value("rom", nlohmann::json()) == RomKey(*rom);
        if (!prepared) {
            try {
                Prepare(*rom);
            } catch (const std::exception& error) {
                const std::string message = error.what();
                forge::WriteFileAtomic(kError, message + "\n");
                Notify("Nao foi possivel preparar o jogo: " + message);
                fs::remove_all(kWork, ignored);
            }
        }
    }
    if (!GameReady()) {
        Notify("Nenhum jogo preparado. Envie sua ROM decriptada (.3ds ou .cci) por FTP para "
               "/data/triaevum/forge/ e abra o TriAevum de novo.");
        return false;
    }
    // The TopScreen pack and configuration: the PKG's, unless the user has
    // their own.
    CopyIfMissing(fs::path(kAppForge) / "topscreen/atlas_overrides.o3tu",
                  fs::path(kTopScreen) / "atlas_overrides.o3tu");
    CopyIfMissing(kAppLinkAnimations, fs::path(kTopScreen) / "zelda_link_ext.zar");
    CopyIfMissing(fs::path(kAppForge) / "topscreen_ui.example.json", kTopScreenConfig);
    return true;
}

} // namespace Fast

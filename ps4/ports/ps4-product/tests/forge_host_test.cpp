// Host check of the console Forge against the desktop Forge
// (ps4/tools/test-forge-host.sh builds and runs it).
//   forge_host_test <rom> <recipes/oot3d.json> <recipe root> <work> <game>
// Prepares the game like the console does and prints the identities, the
// RomFS fingerprint and the manifest path for the script to compare.
#include "forge/forge_core.h"

#include <chrono>
#include <cstdio>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: %s ROM CATALOG RECIPE_ROOT WORK GAME\n", argv[0]);
        return 2;
    }
    using namespace triaevum::forge;
    const auto started = std::chrono::steady_clock::now();
    std::string lastStage;
    int lastPercent = -1;
    const ProgressFn progress = [&](std::string_view stage, double fraction) {
        const int percent = static_cast<int>(fraction * 100);
        if (stage != lastStage || percent / 10 != lastPercent / 10) {
            std::fprintf(stderr, "  %s %d%%\n", std::string(stage).c_str(), percent);
            lastStage = stage;
            lastPercent = percent;
        }
    };
    try {
        const PrepareResult result = PrepareTitle(
            {argv[1], argv[2], argv[3], argv[4], argv[5]}, progress);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto print = FingerprintRomfs(std::filesystem::path(argv[5]) / "inputs/romfs.bin", {});
        nlohmann::json report{
            {"recipe", result.RecipeId},
            {"program_id", result.ProgramId},
            {"inputs", {{"code", result.Code.Json()}, {"exheader", result.Exheader.Json()},
                        {"romfs", result.Romfs.Json()}}},
            {"romfs_fingerprint", {{"sha256", print.Sha256}, {"files", print.Files},
                                   {"directories", print.Directories},
                                   {"payload_bytes", print.PayloadBytes}}},
            {"receipt", result.Receipt},
            {"seconds", seconds}};
        std::cout << report.dump(1) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "forge failed: %s\n", error.what());
        return 1;
    }
}

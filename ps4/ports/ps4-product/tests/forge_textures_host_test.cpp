// Host check of the console Forge's HD UI texture pack against the desktop
// Forge's (ps4/tools/test-forge-host.sh --textures).
//   forge_textures_host_test <ui_texture_pack.json> <archive.zip> <data root> [desktop pack]
// Extracts the pack into <data root>/mods/ui-textures and, with a desktop pack
// directory, compares every prepared file with it.
#include "forge/forge_textures.h"

#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: %s CATALOG ARCHIVE DATA_ROOT [DESKTOP_PACK]\n", argv[0]);
        return 2;
    }
    using namespace triaevum::forge;
    namespace fs = std::filesystem;
    try {
        const auto started = std::chrono::steady_clock::now();
        const UiTexturePack pack = UiTexturePack::Load(argv[1]);
        const fs::path prepared = ExtractUiTexturePack(pack, argv[2], argv[3], {});
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("prepared %s in %.1f s: %s\n", pack.Title().c_str(), seconds, prepared.c_str());
        std::printf("  %s prepared (full hashes)\n",
                    UiTexturePackPrepared(pack, argv[3], true) ? "ok  " : "FAIL");
        const auto reduceStarted = std::chrono::steady_clock::now();
        const fs::path reduced = ReduceUiTexturePack(pack, argv[3], {});
        std::printf("  %s reduced for PS4 in %.1f s: %s\n",
                    UiTexturePackReduced(pack, argv[3]) ? "ok  " : "FAIL",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - reduceStarted).count(),
                    reduced.c_str());
        if (argc == 5) {
            size_t same = 0, total = 0;
            for (const auto& member : pack.Catalog["members"]) {
                const std::string relative = member["prepared"];
                ++total;
                same += HashFile(prepared / relative) == HashFile(fs::path(argv[4]) / relative);
            }
            std::printf("  %s %zu of %zu files identical to the desktop Forge's pack\n",
                        same == total ? "ok  " : "FAIL", same, total);
            return same == total ? 0 : 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failed: %s\n", error.what());
        return 1;
    }
}

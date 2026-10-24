// The console Forge's HD UI texture pack (OoT3D Vanilla UI/HUD 3.2), a port
// of tools/triaevum_release/ui_texture_pack.py: the author's original archive
// is verified against the revision recipes' contract, exactly the catalogued
// textures are extracted and verified, and the prepared pack has the desktop
// layout (common/, languages/<folder>/) the runtime's --ui-texture-pack
// reads. The catalog (ui_texture_pack.json) is written at build time from the
// desktop Forge's own catalog by tools/forge_catalogs.py.
#pragma once

#include "forge_core.h"

namespace triaevum::forge {

struct UiTexturePack {
    nlohmann::json Catalog;

    static UiTexturePack Load(const std::filesystem::path& catalog);
    std::string Title() const;
    std::string ArchiveName() const;
    Identity Archive() const;
    std::vector<std::string> Downloads() const;
    // <data root>/mods/ui-textures/<mod>-<version>
    std::filesystem::path Directory(const std::filesystem::path& dataRoot) const;
    // The pack's HD SingleScreen UI atlas inside a prepared pack.
    std::filesystem::path UiAtlasHd(const std::filesystem::path& dataRoot) const;
};

// The prepared pack is complete: its receipt names this pack, and every
// member has its size (and, with `hashes`, its SHA-256).
bool UiTexturePackPrepared(const UiTexturePack& pack, const std::filesystem::path& dataRoot,
                           bool hashes);

// The archive in one of `directories` with the contract's name and size; the
// SHA-256 is checked when it is extracted.
std::optional<std::filesystem::path> FindUiTextureArchive(
    const UiTexturePack& pack, const std::vector<std::filesystem::path>& directories);

// Verifies the archive, extracts and verifies the catalogued textures, and
// replaces the prepared pack. Returns its directory.
std::filesystem::path ExtractUiTexturePack(const UiTexturePack& pack,
                                           const std::filesystem::path& archive,
                                           const std::filesystem::path& dataRoot,
                                           const ProgressFn& progress);

} // namespace triaevum::forge

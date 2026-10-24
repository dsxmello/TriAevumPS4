// The console Forge: the user's decrypted ROM in, the prepared game out.
//
// A C++ port of the desktop Forge steps the PS4 product needs, kept
// byte-for-byte compatible with them (tools/triaevum_release):
//   ctr_rom.py                 NCSD/NCCH, ExeFS .code (backwards LZSS), RomFS
//   forge_gui.py               revision recipe matching
//   input_copy_adapter.py      COPY-only code normalization
//   oot3d_region_assets.py     USA RomFS -> canonical EUR service view
//   romfs_identity.py          logical RomFS fingerprint (content families)
//   data_compatibility.py      content-family verification
//   build_process_manifest.py  CTR process layout from the ExHeader
// Translated code is not produced here: the title's AOT is in the eboot, so
// only revisions whose execution code is the compiled one are accepted.
// Portable C++ (POSIX file I/O) so the host test can compare every output
// with the desktop Forge.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace triaevum::forge {

class ForgeError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct Identity {
    uint64_t Bytes = 0;
    std::string Sha256;
    bool operator==(const Identity&) const = default;
    nlohmann::json Json() const { return {{"bytes", Bytes}, {"sha256", Sha256}}; }
};

// stage: a short label; fraction in [0, 1] within that stage.
using ProgressFn = std::function<void(std::string_view stage, double fraction)>;

// --- ctr_rom.py -----------------------------------------------------------

struct ExtractedInputs {
    std::string ContainerKind;
    int PartitionIndex = 0;
    uint64_t ProgramId = 0;
    std::filesystem::path Code, Exheader, Romfs;
    Identity CodeId, ExheaderId, RomfsId;
};

std::vector<uint8_t> DecompressExefsCode(const std::vector<uint8_t>& compressed);
// Writes code.bin, exheader.bin and romfs.bin (the raw NCCH RomFS, IVFC
// included) into the existing directory `output`.
ExtractedInputs ExtractDecryptedRom(const std::filesystem::path& rom,
                                    const std::filesystem::path& output,
                                    const ProgressFn& progress);

// --- RomFS ----------------------------------------------------------------

struct RomfsFingerprint {
    std::string Sha256;
    uint64_t Files = 0, Directories = 0, PayloadBytes = 0;
    bool operator==(const RomfsFingerprint&) const = default;
};
RomfsFingerprint FingerprintRomfs(const std::filesystem::path& romfs,
                                  const ProgressFn& progress);
// oot3d_usa_resources_to_eur_v2: a new Level-3 service view at destination.
nlohmann::json NormalizeUsaRomfs(const std::filesystem::path& source,
                                 const std::filesystem::path& destination,
                                 const ProgressFn& progress);

// --- recipes ----------------------------------------------------------------

struct Recipe {
    nlohmann::json Json;
    std::string Id() const { return Json.at("id").get<std::string>(); }
    bool HasAdapter() const { return Json.contains("input_adapter"); }
};

std::vector<Recipe> LoadRecipes(const std::filesystem::path& catalog);
// forge_gui.match_extracted_recipe.
Recipe MatchRecipe(const std::vector<Recipe>& recipes, const ExtractedInputs& extracted,
                   const ProgressFn& progress);

// --- process manifest --------------------------------------------------------

// build_process_manifest.build_manifest with the console's file-relative
// source paths (inputs/<name>).
nlohmann::json BuildProcessManifest(const std::vector<uint8_t>& exheader,
                                    const std::vector<uint8_t>& code,
                                    const std::filesystem::path& romfs,
                                    std::string_view exheaderPath,
                                    std::string_view codePath,
                                    std::string_view romfsPath);
// json.dumps(value, indent=2, sort_keys=True) + "\n".
std::string DumpPythonJson(const nlohmann::json& value);

// --- the whole preparation -----------------------------------------------------

struct PrepareOptions {
    std::filesystem::path Rom;
    std::filesystem::path Catalog;       // recipes/oot3d.json
    std::filesystem::path RecipeRoot;    // base of recipe-relative paths (adapters)
    std::filesystem::path Work;          // scratch directory, removed afterwards
    std::filesystem::path Game;          // inputs/ + process-manifest.json
};

struct PrepareResult {
    std::string RecipeId;
    uint64_t ProgramId = 0;
    Identity Code, Exheader, Romfs;      // execution inputs
    nlohmann::json Receipt;              // adaptation receipt or null
};

// Builds the game in Work and replaces Game only when everything verified.
PrepareResult PrepareTitle(const PrepareOptions& options, const ProgressFn& progress);

// Helpers shared with the PS4 glue.
std::vector<uint8_t> ReadFile(const std::filesystem::path& path);
void WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes);
Identity HashFile(const std::filesystem::path& path, const ProgressFn& progress = {},
                  std::string_view stage = {});
// File removal and copying with absolute paths only: the PS4 SDK has no
// openat/unlinkat, which std::filesystem::remove_all and copy_file need.
// RemoveTree refuses relative and shallow paths.
void RemoveFile(const std::filesystem::path& path);
void RemoveTree(const std::filesystem::path& root);
void CopyFile(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace triaevum::forge

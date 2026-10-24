#include "forge_textures.h"

#include "forge_sha256.h"

#include <zip.h>

#include <memory>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace triaevum::forge {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kCatalogFormat = "triaevum_ps4_ui_texture_pack_v1";
constexpr const char* kReceipt = "source.json";
constexpr size_t kChunk = 4 * 1024 * 1024;

uint64_t FileSize(const fs::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return UINT64_MAX;
    return static_cast<uint64_t>(info.st_size);
}

void CreateDirectories(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    if (error) throw ForgeError("cannot create " + path.string() + ": " + error.message());
}

// A member path from the catalog: relative, no "." or ".." parts.
fs::path SafeRelative(const std::string& text) {
    const fs::path path(text);
    if (text.empty() || path.is_absolute()) throw ForgeError("invalid pack path " + text);
    for (const auto& part : path) {
        if (part == "." || part == "..") throw ForgeError("invalid pack path " + text);
    }
    return path;
}

struct ZipCloser {
    void operator()(zip_t* archive) const { zip_discard(archive); }
};
struct ZipFileCloser {
    void operator()(zip_file_t* file) const { zip_fclose(file); }
};

} // namespace

UiTexturePack UiTexturePack::Load(const fs::path& catalog) {
    const auto bytes = ReadFile(catalog);
    UiTexturePack pack{json::parse(bytes.begin(), bytes.end())};
    if (pack.Catalog.value("format", "") != kCatalogFormat || !pack.Catalog["members"].is_array() ||
        pack.Catalog["members"].empty()) {
        throw ForgeError("unsupported HD UI texture pack catalog");
    }
    return pack;
}

std::string UiTexturePack::Title() const {
    return Catalog.at("title").get<std::string>() + " " + Catalog.at("version").get<std::string>();
}
std::string UiTexturePack::ArchiveName() const { return Catalog.at("archive").at("name"); }
Identity UiTexturePack::Archive() const {
    return {Catalog.at("archive").at("bytes").get<uint64_t>(),
            Catalog.at("archive").at("sha256").get<std::string>()};
}
std::vector<std::string> UiTexturePack::Downloads() const {
    return Catalog.at("downloads").get<std::vector<std::string>>();
}
fs::path UiTexturePack::Directory(const fs::path& dataRoot) const {
    return dataRoot / "mods" / "ui-textures" /
           (Catalog.at("mod_id").get<std::string>() + "-" + Catalog.at("version").get<std::string>());
}
fs::path UiTexturePack::UiAtlasHd(const fs::path& dataRoot) const {
    return Directory(dataRoot) / SafeRelative(Catalog.at("ui_atlas_hd"));
}

bool UiTexturePackPrepared(const UiTexturePack& pack, const fs::path& dataRoot, bool hashes) {
    const fs::path directory = pack.Directory(dataRoot);
    try {
        const auto bytes = ReadFile(directory / kReceipt);
        const json receipt = json::parse(bytes.begin(), bytes.end());
        if (receipt.value("mod", json()) != pack.Catalog["mod_id"] ||
            receipt.value("version", json()) != pack.Catalog["version"]) {
            return false;
        }
        for (const json& member : pack.Catalog["members"]) {
            const fs::path path = directory / SafeRelative(member.at("prepared"));
            if (FileSize(path) != member.at("bytes").get<uint64_t>()) return false;
            if (hashes && HashFile(path).Sha256 != member.at("sha256").get<std::string>()) return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<fs::path> FindUiTextureArchive(const UiTexturePack& pack,
                                             const std::vector<fs::path>& directories) {
    for (const fs::path& directory : directories) {
        const fs::path candidate = directory / pack.ArchiveName();
        if (FileSize(candidate) == pack.Archive().Bytes) return candidate;
    }
    return std::nullopt;
}

fs::path ExtractUiTexturePack(const UiTexturePack& pack, const fs::path& archivePath,
                              const fs::path& dataRoot, const ProgressFn& progress) {
    if (HashFile(archivePath, progress, "textures-verify") != pack.Archive()) {
        throw ForgeError(pack.Title() + " archive failed verification: " + archivePath.string());
    }
    const fs::path destination = pack.Directory(dataRoot);
    const fs::path staging = destination.string() + ".extract";
    RemoveTree(staging);
    CreateDirectories(staging);

    int error = 0;
    std::unique_ptr<zip_t, ZipCloser> archive(zip_open(archivePath.c_str(), ZIP_RDONLY, &error));
    if (!archive) {
        zip_error_t description;
        zip_error_init_with_code(&description, error);
        const std::string text = zip_error_strerror(&description);
        zip_error_fini(&description);
        throw ForgeError("cannot open " + archivePath.string() + ": " + text);
    }
    const std::string prefix = pack.Catalog.at("archive_root").get<std::string>() + "/" +
                               pack.Catalog.at("texture_folder").get<std::string>() + "/";
    uint64_t total = 0, done = 0;
    for (const json& member : pack.Catalog["members"]) total += member.at("bytes").get<uint64_t>();
    std::vector<uint8_t> buffer(kChunk);
    try {
        for (const json& member : pack.Catalog["members"]) {
            const std::string name = member.at("name");
            const uint64_t size = member.at("bytes").get<uint64_t>();
            const std::string entry = prefix + name;
            zip_stat_t info;
            zip_stat_init(&info);
            if (zip_stat(archive.get(), entry.c_str(), 0, &info) != 0) {
                throw ForgeError("The " + pack.Title() + " archive has no " + name);
            }
            if (!(info.valid & ZIP_STAT_SIZE) || info.size != size) {
                throw ForgeError(name + " differs from " + pack.Title());
            }
            const fs::path target = staging / SafeRelative(member.at("prepared"));
            CreateDirectories(target.parent_path());
            std::unique_ptr<zip_file_t, ZipFileCloser> input(zip_fopen(archive.get(), entry.c_str(), 0));
            if (!input) throw ForgeError("cannot read " + name + " from the archive");
            const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) throw ForgeError("cannot create " + target.string());
            Sha256 hash;
            uint64_t written = 0;
            for (;;) {
                const zip_int64_t got = zip_fread(input.get(), buffer.data(), buffer.size());
                if (got < 0) {
                    ::close(fd);
                    throw ForgeError("cannot decompress " + name);
                }
                if (got == 0) break;
                hash.Update(buffer.data(), static_cast<size_t>(got));
                for (zip_int64_t offset = 0; offset < got;) {
                    const ssize_t put = ::write(fd, buffer.data() + offset, static_cast<size_t>(got - offset));
                    if (put <= 0) {
                        ::close(fd);
                        throw ForgeError("cannot write " + target.string());
                    }
                    offset += put;
                }
                written += static_cast<uint64_t>(got);
                done += static_cast<uint64_t>(got);
                if (progress) progress("textures-extract", static_cast<double>(done) / static_cast<double>(total));
            }
            ::fsync(fd);
            ::close(fd);
            // What reached the disk, as in the ROM extraction.
            if (written != size || hash.FinishHex() != member.at("sha256").get<std::string>() ||
                HashFile(target) != Identity{size, member.at("sha256").get<std::string>()}) {
                throw ForgeError(name + " differs from " + pack.Title());
            }
        }
        const json receipt{{"mod", pack.Catalog["mod_id"]},
                           {"version", pack.Catalog["version"]},
                           {"origin", "archive:" + pack.Archive().Sha256},
                           {"attribution", pack.Catalog["attribution"]},
                           {"license", pack.Catalog["license"]},
                           {"license_url", pack.Catalog["license_url"]},
                           {"source_url", pack.Catalog["page"]},
                           {"private_local_artifact", true},
                           {"redistributable", false}};
        WriteFileAtomic(staging / kReceipt, receipt.dump(2) + "\n");
        RemoveTree(destination);
        CreateDirectories(destination.parent_path());
        fs::rename(staging, destination);
    } catch (...) {
        try {
            RemoveTree(staging);
        } catch (...) {
        }
        throw;
    }
    if (!UiTexturePackPrepared(pack, dataRoot, false)) {
        throw ForgeError(pack.Title() + ": the extracted files changed while they were stored");
    }
    return destination;
}

} // namespace triaevum::forge

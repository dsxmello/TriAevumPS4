#include "forge_core.h"

#include "forge_sha256.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace triaevum::forge {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr uint64_t kMediaUnit = 0x200;
constexpr size_t kNcsdHeaderSize = 0x200;
constexpr size_t kNcchHeaderSize = 0x200;
constexpr size_t kExheaderSize = 0x800;
constexpr size_t kExefsHeaderSize = 0x200;
constexpr size_t kChunk = 4 * 1024 * 1024;
constexpr uint64_t kMaxDecompressedCode = 512ULL * 1024 * 1024;
constexpr uint32_t kInvalid = 0xFFFFFFFFU;

// ---- POSIX file ------------------------------------------------------------

class File final {
  public:
    File(const fs::path& path, int flags, mode_t mode = 0644) {
        mFd = ::open(path.c_str(), flags, mode);
        if (mFd < 0) {
            throw ForgeError("cannot open " + path.string() + ": " + std::strerror(errno));
        }
        mPath = path.string();
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() {
        if (mFd >= 0) ::close(mFd);
    }

    uint64_t Size() const {
        struct stat info {};
        if (::fstat(mFd, &info) != 0) throw ForgeError("cannot stat " + mPath);
        return static_cast<uint64_t>(info.st_size);
    }
    // Reads exactly `size` bytes or throws (`label` names the truncated part).
    void ReadAt(uint64_t offset, void* data, size_t size, std::string_view label) const {
        auto* out = static_cast<uint8_t*>(data);
        while (size != 0) {
            const ssize_t got = ::pread(mFd, out, size, static_cast<off_t>(offset));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                throw ForgeError("the file is truncated while reading " + std::string(label));
            }
            out += got;
            offset += static_cast<uint64_t>(got);
            size -= static_cast<size_t>(got);
        }
    }
    std::vector<uint8_t> Read(uint64_t offset, size_t size, std::string_view label) const {
        std::vector<uint8_t> data(size);
        ReadAt(offset, data.data(), size, label);
        return data;
    }
    // Sequential writes: every output file is written front to back once
    // (1.33 on the console: rewriting a range with pwrite left heap bytes up
    // to the next 32 KiB boundary after it).
    void Append(const void* data, size_t size) {
        const auto* in = static_cast<const uint8_t*>(data);
        while (size != 0) {
            const ssize_t put = ::write(mFd, in, size);
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) throw ForgeError("cannot write " + mPath + ": " + std::strerror(errno));
            in += put;
            size -= static_cast<size_t>(put);
        }
    }
    void Sync() { ::fsync(mFd); }

  private:
    int mFd = -1;
    std::string mPath;
};

uint32_t U32(const uint8_t* data) {
    return uint32_t{data[0]} | uint32_t{data[1]} << 8 | uint32_t{data[2]} << 16 |
           uint32_t{data[3]} << 24;
}
uint64_t U64(const uint8_t* data) { return U32(data) | uint64_t{U32(data + 4)} << 32; }
void PutU32(uint8_t* data, uint32_t value) {
    for (int i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t U32At(const std::vector<uint8_t>& data, size_t offset, std::string_view what) {
    if (offset + 4 > data.size()) throw ForgeError("u32 outside " + std::string(what));
    return U32(data.data() + offset);
}

void CheckedRegion(uint64_t offset, uint64_t size, uint64_t lower, uint64_t upper,
                   const std::string& label) {
    if (offset < lower || size == 0 || offset > upper || size > upper - offset) {
        throw ForgeError(label + " lies outside the selected title partition");
    }
}

// Bytes that replace the copied ones at Offset of the output.
struct Patch {
    uint64_t Offset = 0;
    std::vector<uint8_t> Bytes;
};

// Copies [offset, offset + size) of `in` to a new file in one sequential
// pass, hashing every byte read and writing from `writeFrom` (an absolute
// offset of `in`), with `patches` applied to the written bytes.
Identity CopyRegion(const File& in, uint64_t offset, uint64_t size, const fs::path& destination,
                    uint64_t writeFrom, const ProgressFn& progress, std::string_view stage,
                    const std::vector<Patch>& patches = {}) {
    File out(destination, O_WRONLY | O_CREAT | O_EXCL);
    Sha256 hash;
    std::vector<uint8_t> buffer(kChunk);
    uint64_t done = 0;
    while (done < size) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(kChunk, size - done));
        in.ReadAt(offset + done, buffer.data(), take, stage);
        hash.Update(buffer.data(), take);
        const uint64_t chunkStart = offset + done, chunkEnd = chunkStart + take;
        if (chunkEnd > writeFrom) {
            const uint64_t skip = writeFrom > chunkStart ? writeFrom - chunkStart : 0;
            const uint64_t outStart = chunkStart + skip - writeFrom, outEnd = chunkEnd - writeFrom;
            for (const Patch& patch : patches) {
                const uint64_t from = std::max(outStart, patch.Offset);
                const uint64_t to = std::min(outEnd, patch.Offset + patch.Bytes.size());
                if (from < to) {
                    std::memcpy(buffer.data() + skip + (from - outStart),
                                patch.Bytes.data() + (from - patch.Offset), to - from);
                }
            }
            out.Append(buffer.data() + skip, take - skip);
        }
        done += take;
        if (progress) progress(stage, static_cast<double>(done) / static_cast<double>(size));
    }
    out.Sync();
    return {size, hash.FinishHex()};
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    File out(path, O_WRONLY | O_CREAT | O_TRUNC);
    out.Append(bytes.data(), bytes.size());
    out.Sync();
}

// ---- UTF-16 names ----------------------------------------------------------

// Python's bytes.decode('utf-16-le'): strict, so a lone surrogate is an error.
std::u32string DecodeUtf16(const uint8_t* data, size_t size) {
    if (size % 2) throw ForgeError("invalid RomFS record name");
    std::u32string text;
    for (size_t i = 0; i < size; i += 2) {
        const char32_t unit = data[i] | data[i + 1] << 8;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 4 > size) throw ForgeError("invalid UTF-16 RomFS name");
            const char32_t low = data[i + 2] | data[i + 3] << 8;
            if (low < 0xDC00 || low > 0xDFFF) throw ForgeError("invalid UTF-16 RomFS name");
            text.push_back(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
            i += 2;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            throw ForgeError("invalid UTF-16 RomFS name");
        } else {
            text.push_back(unit);
        }
    }
    return text;
}

std::vector<uint8_t> EncodeUtf16(std::u32string_view text) {
    std::vector<uint8_t> out;
    for (char32_t c : text) {
        auto unit = [&](char32_t u) {
            out.push_back(static_cast<uint8_t>(u));
            out.push_back(static_cast<uint8_t>(u >> 8));
        };
        if (c >= 0x10000) {
            c -= 0x10000;
            unit(0xD800 + (c >> 10));
            unit(0xDC00 + (c & 0x3FF));
        } else {
            unit(c);
        }
    }
    return out;
}

bool ValidName(const std::u32string& name) {
    return name.find(U'/') == std::u32string::npos && name.find(U'\\') == std::u32string::npos &&
           name.find(U'\0') == std::u32string::npos && name != U"." && name != U"..";
}

// romfs name hash (Azahar layered_fs.cpp).
uint32_t NameHash(uint32_t parent, const std::vector<uint8_t>& encoded) {
    uint32_t value = parent ^ 123456789U;
    for (size_t i = 0; i + 1 < encoded.size(); i += 2) {
        const uint32_t unit = encoded[i] | encoded[i + 1] << 8;
        value = ((value >> 5) | (value << 27)) ^ unit;
    }
    return value;
}

// ---- RomFS Level-3 ---------------------------------------------------------

struct DirRecord {
    uint32_t Parent = 0;
    std::vector<uint8_t> Name;
    uint32_t Chain = 0;
    std::u32string Path;
};
struct FileRecord {
    uint32_t Parent = 0;
    std::vector<uint8_t> Name;
    uint32_t Chain = 0;
    std::u32string Path;
    uint64_t Payload = 0, Length = 0;
};

struct Level3 {
    uint64_t Base = 0;     // 0x1000 for a raw NCCH RomFS (IVFC), else 0
    uint64_t Size = 0;     // image size after Base
    std::array<uint32_t, 10> H{};
    std::vector<uint8_t> Dirs, Files;
    std::map<uint32_t, DirRecord> DirRecords;    // by record offset
    std::map<uint32_t, FileRecord> FileRecords;

    void Read(const File& file, uint64_t offset, void* data, size_t count) const {
        if (offset + count > Size) throw ForgeError("RomFS extent outside image");
        file.ReadAt(Base + offset, data, count, "RomFS");
    }
};

// The union of the checks of oot3d_region_assets.normalize_romfs and
// romfs_identity.fingerprint; `fingerprintBounds` selects the latter's
// metadata limits.
Level3 ParseLevel3(const File& file, bool fingerprintBounds) {
    Level3 l3;
    const uint64_t imageSize = file.Size();
    uint8_t magic[4] = {};
    if (imageSize >= 4) file.ReadAt(0, magic, 4, "RomFS header");
    l3.Base = std::memcmp(magic, "IVFC", 4) == 0 ? 0x1000 : 0;
    if (imageSize < l3.Base) throw ForgeError("RomFS extent outside image");
    l3.Size = imageSize - l3.Base;
    uint8_t header[40];
    l3.Read(file, 0, header, sizeof(header));
    for (int i = 0; i < 10; ++i) l3.H[i] = U32(header + i * 4);
    const auto& h = l3.H;
    if (h[0] != 40) throw ForgeError("invalid Level-3 header");
    if (!fingerprintBounds) {
        if (h[2] % 4 || h[6] % 4 || !h[2] || !h[6]) throw ForgeError("invalid Level-3 header");
        if (std::max({h[2], h[4], h[6], h[8]}) > 16U * 1024 * 1024) {
            throw ForgeError("RomFS metadata exceeds limits");
        }
    }
    uint64_t end = 40;
    for (const auto& [start, length] : {std::pair{h[1], h[2]}, std::pair{h[3], h[4]},
                                        std::pair{h[5], h[6]}, std::pair{h[7], h[8]}}) {
        if (start < end || start % 4 || !length || uint64_t{start} + length > l3.Size ||
            (fingerprintBounds && length > 16U * 1024 * 1024)) {
            throw ForgeError("invalid or overlapping RomFS metadata extents");
        }
        end = uint64_t{start} + length;
    }
    if ((fingerprintBounds && (h[2] % 4 || h[6] % 4)) || h[9] < end || h[9] > l3.Size || h[9] % 16) {
        throw ForgeError("invalid RomFS data extent");
    }
    l3.Dirs.resize(h[4]);
    l3.Read(file, h[3], l3.Dirs.data(), h[4]);
    l3.Files.resize(h[8]);
    l3.Read(file, h[7], l3.Files.data(), h[8]);

    std::vector<std::pair<uint32_t, uint32_t>> dirExtents, fileExtents;
    auto record = [](std::vector<uint8_t>& table, uint32_t offset, uint32_t fixed,
                     std::vector<std::pair<uint32_t, uint32_t>>& extents,
                     std::u32string& text) {
        if (offset % 4 || uint64_t{offset} + fixed > table.size()) {
            throw ForgeError("invalid RomFS record offset");
        }
        const uint32_t nameSize = U32(table.data() + offset + fixed - 4);
        if (nameSize % 2 || uint64_t{offset} + fixed + nameSize > table.size()) {
            throw ForgeError("invalid RomFS record name");
        }
        std::vector<uint8_t> name(table.begin() + offset + fixed,
                                  table.begin() + offset + fixed + nameSize);
        text = DecodeUtf16(name.data(), name.size());
        extents.emplace_back(offset, (offset + fixed + nameSize + 3) & ~3U);
        return name;
    };

    struct Pending { uint32_t Offset; std::u32string Prefix; uint32_t Parent; };
    std::vector<Pending> pending{{0, U"", 0}};
    while (!pending.empty()) {
        const Pending item = std::move(pending.back());
        pending.pop_back();
        if (l3.DirRecords.count(item.Offset)) {
            throw ForgeError("cyclic or duplicate RomFS directory");
        }
        std::u32string name;
        auto encoded = record(l3.Dirs, item.Offset, 24, dirExtents, name);
        const uint8_t* r = l3.Dirs.data() + item.Offset;
        const uint32_t parent = U32(r), sibling = U32(r + 4), childDir = U32(r + 8);
        uint32_t childFile = U32(r + 12);
        const uint32_t chain = U32(r + 16);
        if (parent != item.Parent || !ValidName(name) ||
            (item.Offset == 0 && (!name.empty() || sibling != kInvalid)) ||
            (item.Offset != 0 && name.empty())) {
            throw ForgeError("invalid RomFS directory ancestry");
        }
        const std::u32string path = item.Prefix.empty() ? name : item.Prefix + U"/" + name;
        l3.DirRecords[item.Offset] = {parent, std::move(encoded), chain, path};
        if (sibling != kInvalid) pending.push_back({sibling, item.Prefix, item.Parent});
        if (childDir != kInvalid) pending.push_back({childDir, path, item.Offset});
        while (childFile != kInvalid) {
            if (l3.FileRecords.count(childFile)) {
                throw ForgeError("cyclic or duplicate RomFS file");
            }
            std::u32string text;
            auto fname = record(l3.Files, childFile, 32, fileExtents, text);
            const uint8_t* f = l3.Files.data() + childFile;
            const uint32_t owner = U32(f), following = U32(f + 4);
            const uint64_t payload = U64(f + 8), length = U64(f + 16);
            const uint32_t fileChain = U32(f + 24);
            if (owner != item.Offset || text.empty() || !ValidName(text)) {
                throw ForgeError("invalid RomFS file ancestry");
            }
            if (uint64_t{h[9]} + payload + length > l3.Size) {
                throw ForgeError("RomFS file outside image");
            }
            l3.FileRecords[childFile] = {owner, std::move(fname), fileChain,
                                         path.empty() ? text : path + U"/" + text, payload,
                                         length};
            childFile = following;
        }
    }
    for (auto* extents : {&dirExtents, &fileExtents}) {
        std::sort(extents->begin(), extents->end());
        uint32_t extentEnd = 0;
        for (const auto& [start, limit] : *extents) {
            if (start < extentEnd) throw ForgeError("overlapping RomFS records");
            extentEnd = limit;
        }
    }
    std::set<std::u32string> dirPaths, filePaths;
    for (const auto& [_, d] : l3.DirRecords) dirPaths.insert(d.Path);
    for (const auto& [_, f] : l3.FileRecords) filePaths.insert(f.Path);
    if (dirPaths.size() != l3.DirRecords.size() || filePaths.size() != l3.FileRecords.size()) {
        throw ForgeError("duplicate RomFS path");
    }
    return l3;
}

// json.dumps(..., ensure_ascii=True) of one string.
void AppendPythonString(std::string& out, const std::u32string& text) {
    static constexpr char kHex[] = "0123456789abcdef";
    auto escape = [&](uint32_t unit) {
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kHex[(unit >> shift) & 15]);
    };
    out.push_back('"');
    for (char32_t c : text) {
        switch (c) {
        case U'"': out += "\\\""; break;
        case U'\\': out += "\\\\"; break;
        case U'\n': out += "\\n"; break;
        case U'\r': out += "\\r"; break;
        case U'\t': out += "\\t"; break;
        case U'\b': out += "\\b"; break;
        case U'\f': out += "\\f"; break;
        default:
            if (c < 0x20 || (c >= 0x7F && c < 0x10000)) {
                if (c == 0x7F) { out.push_back(0x7F); break; }
                escape(c);
            } else if (c >= 0x10000) {
                const uint32_t v = c - 0x10000;
                escape(0xD800 + (v >> 10));
                escape(0xDC00 + (v & 0x3FF));
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

// ---- input_copy_adapter.py ---------------------------------------------------

constexpr const char* kCopyFormat = "triaevum_input_copy_adapter_v1";
constexpr uint64_t kCopyMaxBytes = 16ULL * 1024 * 1024;
constexpr size_t kCopyMaxOperations = 500000;

bool ValidDigest(const json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool IsPlainInt(const json& value) { return value.is_number_integer(); }

Identity IdentityOf(const json& record) {
    return {record.at("bytes").get<uint64_t>(), record.at("sha256").get<std::string>()};
}

void ValidateCopyProgram(const json& program) {
    static const std::set<std::string> kKeys{"format", "source", "canonical", "copies"};
    std::set<std::string> keys;
    if (!program.is_object()) throw ForgeError("invalid COPY-only normalization format");
    for (const auto& [key, _] : program.items()) keys.insert(key);
    if (keys != kKeys || program["format"] != kCopyFormat) {
        throw ForgeError("invalid COPY-only normalization format");
    }
    for (const char* field : {"source", "canonical"}) {
        const json& record = program[field];
        if (!record.is_object() || record.size() != 2 || !record.contains("bytes") ||
            !record.contains("sha256") || !IsPlainInt(record["bytes"]) ||
            record["bytes"].get<int64_t>() <= 0 ||
            record["bytes"].get<uint64_t>() > kCopyMaxBytes || !ValidDigest(record["sha256"])) {
            throw ForgeError("invalid normalization identity");
        }
    }
    const uint64_t size = program["canonical"]["bytes"].get<uint64_t>();
    const uint64_t sourceSize = program["source"]["bytes"].get<uint64_t>();
    const json& copies = program["copies"];
    if (!copies.is_array() || copies.empty() || copies.size() > kCopyMaxOperations) {
        throw ForgeError("invalid normalization operation list");
    }
    uint64_t written = 0;
    for (const json& operation : copies) {
        if (!operation.is_array() || operation.size() != 2 || !IsPlainInt(operation[0]) ||
            !IsPlainInt(operation[1])) {
            throw ForgeError("invalid COPY operation");
        }
        const int64_t offset = operation[0].get<int64_t>(), count = operation[1].get<int64_t>();
        if (offset < 0 || count <= 0 || static_cast<uint64_t>(offset + count) > sourceSize ||
            written + static_cast<uint64_t>(count) > size) {
            throw ForgeError("COPY operation exceeds input or output bounds");
        }
        written += static_cast<uint64_t>(count);
    }
    if (written != size) throw ForgeError("incomplete normalization output");
}

std::vector<uint8_t> ApplyCopyProgram(const std::vector<uint8_t>& source, const json& program) {
    ValidateCopyProgram(program);
    if (IdentityOf(program["source"]) != Identity{source.size(), Sha256::Of(source.data(), source.size())}) {
        throw ForgeError("normalization source identity mismatch");
    }
    std::vector<uint8_t> output;
    output.reserve(program["canonical"]["bytes"].get<size_t>());
    for (const json& operation : program["copies"]) {
        const size_t offset = operation[0].get<size_t>(), count = operation[1].get<size_t>();
        output.insert(output.end(), source.begin() + offset, source.begin() + offset + count);
    }
    if (IdentityOf(program["canonical"]) != Identity{output.size(), Sha256::Of(output.data(), output.size())}) {
        throw ForgeError("canonical output identity mismatch");
    }
    return output;
}

// ---- oot3d_region_assets.adapt_qm_table ---------------------------------------

std::vector<uint8_t> AdaptQmTable(const std::vector<uint8_t>& data, uint64_t fileSize) {
    if (data.size() < 16) throw ForgeError("truncated QM header");
    const uint32_t version = U32(data.data() + 4), count = U32(data.data() + 8),
                   reserved = U32(data.data() + 12);
    if (std::memcmp(data.data(), "QM\0\0", 4) != 0 || version != 4 || reserved != 0 ||
        count == 0 || count > 100000) {
        throw ForgeError("unsupported QM container");
    }
    const uint64_t tableEnd = 16 + uint64_t{count} * 96;
    if (data.size() != tableEnd || tableEnd > fileSize) throw ForgeError("invalid QM record table");
    std::vector<uint8_t> output = data;
    for (uint32_t index = 0; index < count; ++index) {
        const size_t entry = 16 + size_t{index} * 96;
        for (int slot = 0; slot < 9; ++slot) {
            const uint32_t offset = U32(data.data() + entry + 16 + slot * 8);
            const uint32_t size = U32(data.data() + entry + 20 + slot * 8);
            if ((offset != 0) != (size != 0) ||
                (offset && (offset < tableEnd || uint64_t{offset} + size > fileSize))) {
                throw ForgeError("QM payload is outside file");
            }
        }
        // US English 1 -> EU English 2, US French 5 -> EU French 4,
        // US Spanish 7 -> EU Spanish 6.
        for (const auto& [destination, source] : {std::pair{2, 1}, std::pair{4, 5}, std::pair{6, 7}}) {
            const size_t start = entry + 16 + destination * 8;
            const uint8_t* original = data.data() + start;
            const uint8_t* replacement = data.data() + entry + 16 + source * 8;
            static constexpr uint8_t kZero[8] = {};
            if (std::memcmp(original, kZero, 8) != 0 && std::memcmp(original, replacement, 8) != 0) {
                throw ForgeError("QM destination language is already populated");
            }
            std::memcpy(output.data() + start, replacement, 8);
        }
    }
    return output;
}

// ---- data_compatibility.py ----------------------------------------------------

constexpr const char* kFamilyFormat = "triaevum_content_family_v1";

json FamilyIdentity(const fs::path& code, const fs::path& exheader, const fs::path& romfs,
                    const ProgressFn& progress) {
    std::vector<uint8_t> header = ReadFile(exheader);
    if (header.size() != 2048) throw ForgeError("Content family requires a complete ExHeader");
    // ExeFS is already decompressed; SD storage and signature/key bytes do
    // not define process data.
    header[0x0d] &= static_cast<uint8_t>(~3);
    std::fill(header.begin() + 0x400, header.begin() + 0x600, 0);
    const Identity codeId = HashFile(code);
    const RomfsFingerprint print = FingerprintRomfs(romfs, progress);
    return {{"code", codeId.Json()},
            {"exheader", Sha256::Of(header.data(), header.size())},
            {"romfs", {{"sha256", print.Sha256}, {"files", print.Files},
                       {"directories", print.Directories}, {"payload_bytes", print.PayloadBytes}}}};
}

json ImportContract(const json& recipe) {
    if (!recipe.contains("input_adapter")) return recipe.value("inputs", json::object());
    const json& adapter = recipe["input_adapter"];
    if (!adapter.is_object() || adapter.value("format", "") != "triaevum_revision_input_adapter_v1") {
        throw ForgeError("Unsupported revision input adapter");
    }
    return adapter.value("source_inputs", json::object());
}

// data_compatibility.expected: the recipe's family for `phase`, or null.
json ExpectedFamily(const json& recipe, std::string_view phase) {
    if (!recipe.contains("data_compatibility") || recipe["data_compatibility"].is_null()) {
        return nullptr;
    }
    const json& contract = recipe["data_compatibility"];
    if (!contract.is_object() || contract.size() != 3 || !contract.contains("format") ||
        !contract.contains("source") || !contract.contains("execution") ||
        contract["format"] != kFamilyFormat) {
        throw ForgeError("Unsupported content-family contract");
    }
    for (const char* member : {"source", "execution"}) {
        const json& family = contract[member];
        if (!family.is_object() || family.size() != 3 || !family.contains("code") ||
            !family.contains("exheader") || !family.contains("romfs")) {
            throw ForgeError("Invalid content-family identity");
        }
        const json& code = family["code"];
        const json& romfs = family["romfs"];
        if (!code.is_object() || code.size() != 2 || !romfs.is_object() || romfs.size() != 4 ||
            !romfs.contains("sha256") || !romfs.contains("files") ||
            !romfs.contains("directories") || !romfs.contains("payload_bytes")) {
            throw ForgeError("Invalid content-family record");
        }
        for (const json* digest : {&family["exheader"], &code["sha256"], &romfs["sha256"]}) {
            if (!ValidDigest(*digest)) throw ForgeError("Invalid content-family digest");
        }
        for (const json* value : {&code["bytes"], &romfs["files"], &romfs["directories"],
                                  &romfs["payload_bytes"]}) {
            if (!IsPlainInt(*value) || value->get<int64_t>() <= 0) {
                throw ForgeError("Invalid content-family size");
            }
        }
        const json inputs = std::string_view(member) == "source"
                                ? (recipe.contains("input_adapter")
                                       ? recipe["input_adapter"].value("source_inputs", json::object())
                                       : recipe.value("inputs", json::object()))
                                : recipe.value("inputs", json::object());
        if (!inputs.contains("code") || code != inputs["code"]) {
            throw ForgeError("Content family cannot change the compiled code identity");
        }
    }
    return contract[std::string(phase)];
}

void VerifyFamily(const json& recipe, const fs::path& code, const fs::path& exheader,
                  const fs::path& romfs, std::string_view phase, const ProgressFn& progress) {
    const json contract = ExpectedFamily(recipe, phase);
    if (contract.is_null()) throw ForgeError("No content-family contract for this title");
    const json actual = FamilyIdentity(code, exheader, romfs, progress);
    if (actual != contract) {
        std::string different;
        for (const char* kind : {"code", "exheader", "romfs"}) {
            if (actual[kind] != contract[kind]) different += (different.empty() ? "" : ", ") + std::string(kind);
        }
        throw ForgeError("Incompatible " + std::string(phase) + " data: " + different +
                         "; no title was activated");
    }
}

json IdentitiesJson(const Identity& code, const Identity& exheader, const Identity& romfs) {
    return {{"code", code.Json()}, {"exheader", exheader.Json()}, {"romfs", romfs.Json()}};
}

// ---- build_process_manifest.py ----------------------------------------------

constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kHeapVaddrEnd = 0x10000000;
constexpr uint32_t kTlsAreaVaddr = 0x1FF82000;
constexpr uint32_t kTlsPageSize = 0x1000;
constexpr uint32_t kConfigMemoryVaddr = 0x1FF80000;
constexpr uint32_t kSharedPageVaddr = 0x1FF81000;
constexpr uint32_t kDspRamVaddr = 0x1FF00000;
constexpr uint32_t kDspRamSize = 0x00080000;
constexpr uint32_t kLegacyLinearHeapVaddr = 0x14000000;
constexpr uint32_t kHeapVaddr = 0x08000000;
constexpr uint32_t kHeapSize = 0x08000000;
constexpr uint32_t kUser32Cpsr = 0x10;
constexpr uint32_t kCtrMainFpscr = 0x03C00010;
constexpr uint64_t kRomfsSuperblockSize = 0x1000;
constexpr std::array<std::array<uint32_t, 3>, 8> kCtrMemoryRegionSizes{{
    {0x04000000, 0x02C00000, 0x01400000},
    {0, 0, 0},
    {0x06000000, 0x00C00000, 0x01400000},
    {0x05000000, 0x01C00000, 0x01400000},
    {0x04800000, 0x02400000, 0x01400000},
    {0x02000000, 0x04C00000, 0x01400000},
    {0x07C00000, 0x06400000, 0x02000000},
    {0x0B200000, 0x02E00000, 0x02000000},
}};

uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

json InitialValue(uint32_t offset, uint32_t size, uint64_t value) {
    return {{"offset", offset}, {"size", size}, {"value", value}};
}

std::array<uint32_t, 4> CtrMemoryLayout(const std::vector<uint8_t>& exheader) {
    const uint32_t mode = exheader.at(0x20D) & 0x0F;
    if (mode >= kCtrMemoryRegionSizes.size()) {
        throw ForgeError("unsupported CTR memory mode " + std::to_string(mode));
    }
    const auto& sizes = kCtrMemoryRegionSizes[mode];
    if (sizes[0] == 0) throw ForgeError("unused CTR memory mode " + std::to_string(mode));
    return {mode, sizes[0], sizes[1], sizes[2]};
}

} // namespace

// ---- public helpers ----------------------------------------------------------

std::vector<uint8_t> ReadFile(const fs::path& path) {
    File file(path, O_RDONLY);
    return file.Read(0, static_cast<size_t>(file.Size()), path.string());
}

void WriteFileAtomic(const fs::path& path, std::string_view bytes) {
    const fs::path temporary = path.string() + ".partial";
    {
        File out(temporary, O_WRONLY | O_CREAT | O_TRUNC);
        out.Append(bytes.data(), bytes.size());
        out.Sync();
    }
    fs::rename(temporary, path);
}

void RemoveFile(const fs::path& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw ForgeError("cannot remove " + path.string() + ": " + std::strerror(errno));
    }
}

void RemoveTree(const fs::path& root) {
    // std::filesystem::remove_all works through openat/fdopendir/unlinkat,
    // which the PS4 SDK does not implement: only absolute-path calls here.
    const std::string text = root.string();
    if (text.empty() || text[0] != '/' || root.lexically_normal() != root ||
        std::count(text.begin(), text.end(), '/') < 3) {
        throw ForgeError("refusing to remove " + text);
    }
    struct stat info {};
    if (::lstat(text.c_str(), &info) != 0) {
        if (errno == ENOENT) return;
        throw ForgeError("cannot inspect " + text + ": " + std::strerror(errno));
    }
    if (!S_ISDIR(info.st_mode)) {
        RemoveFile(root);
        return;
    }
    std::vector<std::string> names;
    if (DIR* directory = ::opendir(text.c_str())) {
        while (const dirent* entry = ::readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        ::closedir(directory);
    } else {
        throw ForgeError("cannot list " + text + ": " + std::strerror(errno));
    }
    for (const std::string& name : names) RemoveTree(root / name);
    if (::rmdir(text.c_str()) != 0 && errno != ENOENT) {
        throw ForgeError("cannot remove " + text + ": " + std::strerror(errno));
    }
}

void CopyFile(const fs::path& from, const fs::path& to) {
    File in(from, O_RDONLY);
    CopyRegion(in, 0, in.Size(), to, 0, {}, from.string());
}

Identity HashFile(const fs::path& path, const ProgressFn& progress, std::string_view stage) {
    File file(path, O_RDONLY);
    const uint64_t size = file.Size();
    Sha256 hash;
    std::vector<uint8_t> buffer(kChunk);
    for (uint64_t done = 0; done < size;) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(kChunk, size - done));
        file.ReadAt(done, buffer.data(), take, path.string());
        hash.Update(buffer.data(), take);
        done += take;
        if (progress) progress(stage, static_cast<double>(done) / static_cast<double>(size));
    }
    return {size, hash.FinishHex()};
}

// ---- ctr_rom.py ------------------------------------------------------------------

std::vector<uint8_t> DecompressExefsCode(const std::vector<uint8_t>& compressed) {
    if (compressed.size() < 8) throw ForgeError("compressed ExeFS .code is too small");
    const uint32_t bufferTopAndBottom = U32(compressed.data() + compressed.size() - 8);
    const uint32_t additionalSize = U32(compressed.data() + compressed.size() - 4);
    const uint64_t decompressedSize = compressed.size() + uint64_t{additionalSize};
    if (decompressedSize > kMaxDecompressedCode) {
        throw ForgeError("compressed ExeFS .code declares an invalid output size");
    }
    const uint32_t footerSize = (bufferTopAndBottom >> 24) & 0xFF;
    const uint32_t encodedSize = bufferTopAndBottom & 0xFFFFFF;
    if (footerSize < 8 || footerSize > compressed.size() || encodedSize < footerSize ||
        encodedSize > compressed.size()) {
        throw ForgeError("compressed ExeFS .code has an invalid footer");
    }
    size_t index = compressed.size() - footerSize;
    const size_t stopIndex = compressed.size() - encodedSize;
    size_t outputIndex = static_cast<size_t>(decompressedSize);
    std::vector<uint8_t> output(static_cast<size_t>(decompressedSize));
    std::copy(compressed.begin(), compressed.end(), output.begin());
    while (index > stopIndex) {
        --index;
        uint8_t control = compressed[index];
        for (int bit = 0; bit < 8; ++bit) {
            if (index <= stopIndex || outputIndex == 0) break;
            if (control & 0x80) {
                if (index < 2) throw ForgeError("compressed ExeFS .code back-reference is truncated");
                index -= 2;
                const uint32_t segment = compressed[index] | compressed[index + 1] << 8;
                const size_t segmentSize = ((segment >> 12) & 0xF) + 3;
                const size_t segmentOffset = (segment & 0xFFF) + 2;
                if (outputIndex < segmentSize) throw ForgeError("compressed ExeFS .code output underflow");
                for (size_t i = 0; i < segmentSize; ++i) {
                    const size_t source = outputIndex + segmentOffset;
                    if (source >= output.size()) {
                        throw ForgeError("compressed ExeFS .code back-reference is out of range");
                    }
                    --outputIndex;
                    output[outputIndex] = output[source];
                }
            } else {
                if (index <= stopIndex || outputIndex == 0) {
                    throw ForgeError("compressed ExeFS .code literal is truncated");
                }
                --index;
                --outputIndex;
                output[outputIndex] = compressed[index];
            }
            control = static_cast<uint8_t>(control << 1);
        }
    }
    return output;
}

namespace {

struct NcchLayout {
    std::string ContainerKind;
    int PartitionIndex = 0;
    uint64_t ProgramId = 0;
    bool CompressedCode = false;
    uint64_t CodeOffset = 0, CodeSize = 0, ExheaderOffset = 0, RomfsOffset = 0, RomfsSize = 0;
};

NcchLayout ParseNcch(const File& rom, uint64_t fileSize, uint64_t base, uint64_t size, int index,
                     const std::string& kind) {
    const std::string partition = "partition " + std::to_string(index);
    const uint64_t end = base + size;
    CheckedRegion(base, kNcchHeaderSize, 0, fileSize, partition + " NCCH header");
    if (end > fileSize) throw ForgeError(partition + " exceeds the ROM size");
    const auto header = rom.Read(base, kNcchHeaderSize, partition + " NCCH header");
    if (std::memcmp(header.data() + 0x100, "NCCH", 4) != 0) {
        throw ForgeError(partition + " is not an NCCH title");
    }
    if (!(header[0x18D] & 0x02)) throw ForgeError(partition + " is not executable");
    if (U32(header.data() + 0x180) == 0) throw ForgeError(partition + " has no ExHeader");

    const uint64_t exheaderOffset = base + kNcchHeaderSize;
    CheckedRegion(exheaderOffset, kExheaderSize, base, end, "ExHeader");
    const auto exheader = rom.Read(exheaderOffset, kExheaderSize, "ExHeader");

    const uint64_t exefsUnits = U32(header.data() + 0x1A0);
    const uint64_t exefsSizeUnits = U32(header.data() + 0x1A4);
    if (exefsUnits == 0 || exefsSizeUnits == 0) throw ForgeError(partition + " has no ExeFS");
    const uint64_t exefsOffset = base + exefsUnits * kMediaUnit;
    const uint64_t exefsSize = exefsSizeUnits * kMediaUnit;
    CheckedRegion(exefsOffset, exefsSize, base, end, "ExeFS");
    const uint64_t exefsEnd = exefsOffset + exefsSize;
    const auto exefsHeader = rom.Read(exefsOffset, kExefsHeaderSize, "ExeFS");

    std::optional<uint64_t> codeOffset;
    uint64_t codeSize = 0;
    for (int section = 0; section < 8; ++section) {
        const uint8_t* entry = exefsHeader.data() + section * 0x10;
        const std::string name(reinterpret_cast<const char*>(entry),
                               strnlen(reinterpret_cast<const char*>(entry), 8));
        if (name != ".code") continue;
        codeSize = U32(entry + 12);
        codeOffset = exefsOffset + kExefsHeaderSize + U32(entry + 8);
        CheckedRegion(*codeOffset, codeSize, exefsOffset + kExefsHeaderSize, exefsEnd, "ExeFS .code");
        break;
    }
    if (!codeOffset) {
        throw ForgeError(partition + " has no readable ExeFS .code section; the ROM may still be encrypted");
    }

    const uint64_t romfsUnits = U32(header.data() + 0x1B0);
    const uint64_t romfsSizeUnits = U32(header.data() + 0x1B4);
    if (romfsUnits == 0 || romfsSizeUnits == 0) throw ForgeError(partition + " has no RomFS");
    const uint64_t romfsOffset = base + romfsUnits * kMediaUnit;
    const uint64_t romfsSize = romfsSizeUnits * kMediaUnit;
    CheckedRegion(romfsOffset, romfsSize, base, end, "RomFS");
    if (std::memcmp(rom.Read(romfsOffset, 4, "RomFS header").data(), "IVFC", 4) != 0) {
        throw ForgeError("the selected title RomFS is not decrypted or has an invalid IVFC header");
    }
    return {kind, index, U64(header.data() + 0x118), (exheader[0x0D] & 1) != 0, *codeOffset,
            codeSize, exheaderOffset, romfsOffset, romfsSize};
}

NcchLayout FindTitleLayout(const File& rom, uint64_t fileSize) {
    const auto header = rom.Read(0, kNcsdHeaderSize, "container header");
    if (std::memcmp(header.data() + 0x100, "NCCH", 4) == 0) {
        return ParseNcch(rom, fileSize, 0, fileSize, 0, "NCCH");
    }
    if (std::memcmp(header.data() + 0x100, "NCSD", 4) != 0) {
        throw ForgeError("the selected file is not a Nintendo 3DS NCSD/NCCH image");
    }
    std::string failures;
    for (int index = 0; index < 8; ++index) {
        const uint8_t* entry = header.data() + 0x120 + index * 8;
        const uint64_t base = U32(entry) * kMediaUnit, size = U32(entry + 4) * kMediaUnit;
        if (base == 0 || size == 0) continue;
        try {
            return ParseNcch(rom, fileSize, base, size, index, "NCSD");
        } catch (const ForgeError& error) {
            failures += (failures.empty() ? "" : "; ") + std::string(error.what());
        }
    }
    throw ForgeError("no usable decrypted application partition was found: " +
                     (failures.empty() ? std::string("the partition table is empty") : failures));
}

} // namespace

ExtractedInputs ExtractDecryptedRom(const fs::path& romPath, const fs::path& output,
                                    const ProgressFn& progress) {
    File rom(romPath, O_RDONLY);
    const uint64_t fileSize = rom.Size();
    const NcchLayout layout = FindTitleLayout(rom, fileSize);
    const auto exheader = rom.Read(layout.ExheaderOffset, kExheaderSize, "ExHeader");
    auto code = rom.Read(layout.CodeOffset, static_cast<size_t>(layout.CodeSize), "ExeFS .code");
    if (layout.CompressedCode) code = DecompressExefsCode(code);

    ExtractedInputs extracted;
    extracted.ContainerKind = layout.ContainerKind;
    extracted.PartitionIndex = layout.PartitionIndex;
    extracted.ProgramId = layout.ProgramId;
    extracted.Exheader = output / "exheader.bin";
    extracted.Code = output / "code.bin";
    extracted.Romfs = output / "romfs.bin";
    WriteBytes(extracted.Exheader, exheader);
    WriteBytes(extracted.Code, code);
    extracted.ExheaderId = {exheader.size(), Sha256::Of(exheader.data(), exheader.size())};
    extracted.CodeId = {code.size(), Sha256::Of(code.data(), code.size())};
    extracted.RomfsId = CopyRegion(rom, layout.RomfsOffset, layout.RomfsSize, extracted.Romfs,
                                   layout.RomfsOffset, progress, "extract");
    // What reached the disk, not only what was read.
    if (HashFile(extracted.Romfs, progress, "check") != extracted.RomfsId) {
        throw ForgeError("the extracted RomFS on disk differs from the ROM (storage write error)");
    }
    return extracted;
}

// ---- romfs_identity.py ------------------------------------------------------------

RomfsFingerprint FingerprintRomfs(const fs::path& path, const ProgressFn& progress) {
    File file(path, O_RDONLY);
    const Level3 l3 = ParseLevel3(file, true);
    const auto& h = l3.H;
    // Both lookup tables must describe exactly the traversed records.
    auto checkChains = [&](auto& records, uint32_t offset, uint32_t length) {
        std::vector<uint8_t> table(length);
        l3.Read(file, offset, table.data(), length);
        const uint32_t buckets = length / 4;
        std::unordered_set<uint32_t> visited;
        for (uint32_t bucket = 0; bucket < buckets; ++bucket) {
            uint32_t link = U32(table.data() + bucket * 4);
            while (link != kInvalid) {
                const auto found = records.find(link);
                if (visited.count(link) || found == records.end()) {
                    throw ForgeError("Invalid RomFS hash chain");
                }
                visited.insert(link);
                if (NameHash(found->second.Parent, found->second.Name) % buckets != bucket) {
                    throw ForgeError("Incorrect RomFS hash bucket");
                }
                link = found->second.Chain;
            }
        }
        if (visited.size() != records.size()) throw ForgeError("RomFS traversal and lookup differ");
    };
    checkChains(l3.DirRecords, h[1], h[2]);
    checkChains(l3.FileRecords, h[5], h[6]);

    std::vector<std::pair<uint64_t, uint64_t>> payloads;
    for (const auto& [_, f] : l3.FileRecords) {
        if (f.Length) payloads.emplace_back(f.Payload, f.Length);
    }
    std::sort(payloads.begin(), payloads.end());
    uint64_t payloadEnd = 0, total = 0;
    for (const auto& [start, length] : payloads) {
        if (start < payloadEnd) throw ForgeError("Overlapping RomFS payloads");
        payloadEnd = start + length;
        total += length;
    }

    struct Entry { bool IsFile; std::u32string Path; uint64_t Length; std::string Digest; };
    std::vector<Entry> entries;
    for (const auto& [_, d] : l3.DirRecords) entries.push_back({false, d.Path, 0, {}});
    // Hash in payload order: one sequential pass over the image.
    std::vector<const FileRecord*> files;
    for (const auto& [_, f] : l3.FileRecords) files.push_back(&f);
    std::sort(files.begin(), files.end(),
              [](const FileRecord* a, const FileRecord* b) { return a->Payload < b->Payload; });
    std::vector<uint8_t> buffer(kChunk);
    uint64_t hashed = 0;
    for (const FileRecord* f : files) {
        Sha256 hash;
        for (uint64_t done = 0; done < f->Length;) {
            const size_t take = static_cast<size_t>(std::min<uint64_t>(kChunk, f->Length - done));
            l3.Read(file, h[9] + f->Payload + done, buffer.data(), take);
            hash.Update(buffer.data(), take);
            done += take;
            hashed += take;
            if (progress && total) progress("verify", static_cast<double>(hashed) / static_cast<double>(total));
        }
        entries.push_back({true, f->Path, f->Length, hash.FinishHex()});
    }
    // sorted() of ['directory', path] / ['file', path, length, digest]:
    // paths are unique within each kind.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.IsFile != b.IsFile) return !a.IsFile;
        return a.Path < b.Path;
    });
    std::string canonical = "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) canonical += ",";
        canonical += entries[i].IsFile ? "[\"file\"," : "[\"directory\",";
        AppendPythonString(canonical, entries[i].Path);
        if (entries[i].IsFile) {
            canonical += "," + std::to_string(entries[i].Length) + ",\"" + entries[i].Digest + "\"";
        }
        canonical += "]";
    }
    canonical += "]";
    Sha256 tree;
    static constexpr char kPrefix[] = "triaevum-romfs-tree-v1";
    tree.Update(kPrefix, sizeof(kPrefix));  // includes the NUL
    tree.Update(canonical.data(), canonical.size());
    return {tree.FinishHex(), l3.FileRecords.size(), l3.DirRecords.size(), total};
}

// ---- oot3d_region_assets.normalize_romfs --------------------------------------------

json NormalizeUsaRomfs(const fs::path& source, const fs::path& destination,
                       const ProgressFn& progress) {
    struct stat existing {};
    if (::stat(destination.c_str(), &existing) == 0) {
        throw ForgeError("normalization requires a new output");
    }
    File in(source, O_RDONLY);
    Level3 l3 = ParseLevel3(in, false);
    const auto& h = l3.H;

    // The EUR executable opens hint movie 181 + its language index (English 2,
    // French 4, Spanish 6); the USA ROM stores the same movies under 1, 5, 7.
    const std::map<std::u32string, std::u32string> kHintMovies{
        {U"misc/hint/movie/hint182.moflex", U"hint183.moflex"},
        {U"misc/hint/movie/hint186.moflex", U"hint185.moflex"},
        {U"misc/hint/movie/hint188.moflex", U"hint187.moflex"}};
    std::map<std::u32string, std::u32string> renames = kHintMovies;
    renames[U"message/us/us.qm"] = U"eu.qm";
    const std::set<std::u32string> kExpectedDirs{U"message/us", U"misc/us"};

    std::set<std::u32string> dirPaths, filePaths;
    for (const auto& [_, d] : l3.DirRecords) dirPaths.insert(d.Path);
    for (const auto& [_, f] : l3.FileRecords) filePaths.insert(f.Path);
    for (const auto& path : kExpectedDirs) {
        if (!dirPaths.count(path)) throw ForgeError("missing USA resource directories");
    }
    if (dirPaths.count(U"message/eu") || dirPaths.count(U"misc/eu")) {
        throw ForgeError("EU resource directories already exist");
    }
    for (const auto& [path, name] : kHintMovies) {
        if (!filePaths.count(path)) throw ForgeError("missing USA hint movies");
    }
    for (const auto& [path, name] : kHintMovies) {
        if (filePaths.count(path.substr(0, path.rfind(U'/')) + U"/" + name)) {
            throw ForgeError("EU hint movies already exist");
        }
    }
    const FileRecord* qm = nullptr;
    size_t qmCount = 0;
    for (const auto& [_, f] : l3.FileRecords) {
        if (f.Path == U"message/us/us.qm") { qm = &f; ++qmCount; }
    }
    if (qmCount != 1) throw ForgeError("missing USA message container");
    const uint64_t qmOffset = h[9] + qm->Payload, qmSize = qm->Length;
    uint8_t qmHead[16];
    l3.Read(in, qmOffset, qmHead, 16);
    const uint32_t count = U32(qmHead + 8);
    if (count == 0 || count > 100000 || 16 + uint64_t{count} * 96 > qmSize) {
        throw ForgeError("invalid QM count");
    }
    std::vector<uint8_t> qmTable(16 + size_t{count} * 96);
    l3.Read(in, qmOffset, qmTable.data(), qmTable.size());
    qmTable = AdaptQmTable(qmTable, qmSize);

    std::vector<uint32_t> directoryHashes(h[2] / 4, kInvalid), fileHashes(h[6] / 4, kInvalid);
    for (auto& [offset, d] : l3.DirRecords) {
        std::vector<uint8_t> name = d.Name;
        if (kExpectedDirs.count(d.Path)) {
            name = EncodeUtf16(U"eu");
            std::copy(name.begin(), name.end(), l3.Dirs.begin() + offset + 24);
        }
        const uint32_t bucket = NameHash(d.Parent, name) % directoryHashes.size();
        PutU32(l3.Dirs.data() + offset + 16, directoryHashes[bucket]);
        directoryHashes[bucket] = offset;
    }
    for (auto& [offset, f] : l3.FileRecords) {
        std::vector<uint8_t> name = f.Name;
        if (const auto found = renames.find(f.Path); found != renames.end()) {
            name = EncodeUtf16(found->second);
            std::copy(name.begin(), name.end(), l3.Files.begin() + offset + 32);
        }
        const uint32_t bucket = NameHash(f.Parent, name) % fileHashes.size();
        PutU32(l3.Files.data() + offset + 24, fileHashes[bucket]);
        fileHashes[bucket] = offset;
    }

    auto table = [](const std::vector<uint32_t>& values) {
        std::vector<uint8_t> bytes(values.size() * 4);
        for (size_t i = 0; i < values.size(); ++i) PutU32(bytes.data() + i * 4, values[i]);
        return bytes;
    };
    const std::vector<Patch> patches{{h[1], table(directoryHashes)}, {h[3], l3.Dirs},
                                     {h[5], table(fileHashes)}, {h[7], l3.Files},
                                     {qmOffset, qmTable}};
    const fs::path temporary = destination.string() + ".partial";
    RemoveFile(temporary);
    CopyRegion(in, l3.Base, l3.Size, temporary, l3.Base, progress, "adapt", patches);
    fs::rename(temporary, destination);
    return {{"files", l3.FileRecords.size()}, {"renamed_directories", 2},
            {"renamed_files", renames.size()}, {"qm_records", count},
            {"language_slots", {{"2", 1}, {"4", 5}, {"6", 7}}},
            {"hint_movies", {{"183", 182}, {"185", 186}, {"187", 188}}},
            {"output_format", "romfs_level3_service_view"}, {"payloads_replaced", 0}};
}

// ---- recipes ------------------------------------------------------------------------

std::vector<Recipe> LoadRecipes(const fs::path& catalog) {
    const auto bytes = ReadFile(catalog);
    const json payload = json::parse(bytes.begin(), bytes.end());
    if (payload.value("format", "") != "triaevum_supported_revisions_v1") {
        throw ForgeError("unsupported supported-revision recipe format");
    }
    if (!payload.contains("recipes") || !payload["recipes"].is_array()) {
        throw ForgeError("supported-revision recipe list is malformed");
    }
    std::vector<Recipe> recipes;
    for (const json& recipe : payload["recipes"]) {
        if (recipe.is_object()) recipes.push_back({recipe});
    }
    return recipes;
}

Recipe MatchRecipe(const std::vector<Recipe>& recipes, const ExtractedInputs& extracted,
                   const ProgressFn& progress) {
    const std::array<std::pair<const char*, const Identity*>, 3> actual{{
        {"code", &extracted.CodeId}, {"exheader", &extracted.ExheaderId}, {"romfs", &extracted.RomfsId}}};
    std::vector<Recipe> matches;
    for (const Recipe& recipe : recipes) {
        const json contracts = ImportContract(recipe.Json);
        if (!contracts.is_object()) continue;
        const bool all = std::all_of(actual.begin(), actual.end(), [&](const auto& item) {
            const auto& [kind, identity] = item;
            return contracts.contains(kind) && contracts[kind].is_object() &&
                   contracts[kind].value("bytes", uint64_t{0}) == identity->Bytes &&
                   contracts[kind].value("sha256", std::string()) == identity->Sha256;
        });
        if (all) matches.push_back(recipe);
    }
    if (matches.empty()) {
        std::vector<Recipe> candidates;
        for (const Recipe& recipe : recipes) {
            const json family = ExpectedFamily(recipe.Json, "source");
            if (!family.is_null() && family["code"] == extracted.CodeId.Json()) {
                candidates.push_back(recipe);
            }
        }
        if (!candidates.empty()) {
            const json family = FamilyIdentity(extracted.Code, extracted.Exheader, extracted.Romfs, progress);
            for (const Recipe& recipe : candidates) {
                if (ExpectedFamily(recipe.Json, "source") == family) matches.push_back(recipe);
            }
        }
    }
    if (matches.empty()) {
        char programId[17];
        std::snprintf(programId, sizeof(programId), "%016llX",
                      static_cast<unsigned long long>(extracted.ProgramId));
        throw ForgeError(std::string("the decrypted ROM does not match any supported game revision "
                                     "(program ID ") + programId + ")");
    }
    if (matches.size() != 1) throw ForgeError("the decrypted ROM matches multiple revision recipes");
    return matches.front();
}

// ---- process manifest ----------------------------------------------------------------

json BuildProcessManifest(const std::vector<uint8_t>& exheader, const std::vector<uint8_t>& code,
                          const fs::path& romfs, std::string_view exheaderPath,
                          std::string_view codePath, std::string_view romfsPath) {
    if (exheader.size() < 0x400) throw ForgeError("ExHeader is truncated");
    File romfsFile(romfs, O_RDONLY);
    const uint64_t romfsSize = romfsFile.Size();
    if (romfsSize == 0) throw ForgeError("RomFS image is empty");
    uint64_t serviceOffset = 0, serviceSize = 0;
    {
        uint8_t prefix[4] = {};
        romfsFile.ReadAt(0, prefix, std::min<uint64_t>(4, romfsSize), "RomFS header");
        if (std::memcmp(prefix, "IVFC", 4) == 0) {
            if (romfsSize <= kRomfsSuperblockSize) {
                throw ForgeError("raw NCCH RomFS is smaller than its IVFC superblock");
            }
            uint8_t level3[4] = {};
            romfsFile.ReadAt(kRomfsSuperblockSize, level3, 4, "RomFS Level-3 header");
            if (U32(level3) != 0x28) {
                throw ForgeError("raw NCCH RomFS has no Level-3 header after its IVFC superblock");
            }
            serviceOffset = kRomfsSuperblockSize;
            serviceSize = romfsSize - kRomfsSuperblockSize;
        } else if (U32(prefix) == 0x28) {
            serviceSize = romfsSize;
        } else {
            throw ForgeError("RomFS image is neither raw NCCH IVFC nor a Level-3 service view");
        }
    }
    struct Segment { uint32_t Address, Pages, CodeSize; };
    auto segment = [&](size_t offset) {
        return Segment{U32At(exheader, offset, "ExHeader"), U32At(exheader, offset + 4, "ExHeader"),
                       U32At(exheader, offset + 8, "ExHeader")};
    };
    const Segment text = segment(0x10), ro = segment(0x20), data = segment(0x30);
    for (const auto& [name, item] : {std::pair{"text", text}, std::pair{"ro", ro}, std::pair{"data", data}}) {
        if (item.Address % kPageSize) throw ForgeError(std::string(name) + " address is not page aligned");
        if (item.Pages == 0) throw ForgeError(std::string(name) + " has no physical pages");
        if (item.CodeSize > uint64_t{item.Pages} * kPageSize) {
            throw ForgeError(std::string(name) + " code size exceeds mapped pages");
        }
    }
    const uint64_t textPages = uint64_t{text.Pages} * kPageSize;
    const uint64_t roPages = uint64_t{ro.Pages} * kPageSize;
    const uint64_t dataPages = uint64_t{data.Pages} * kPageSize;
    if (code.size() != textPages + roPages + dataPages) {
        throw ForgeError("decompressed code size does not match ExHeader page total");
    }
    if (text.Address + textPages > ro.Address) throw ForgeError("text and ro virtual ranges overlap");
    if (ro.Address + roPages > data.Address) throw ForgeError("ro and data virtual ranges overlap");
    const uint32_t stackSize = U32At(exheader, 0x1C, "ExHeader");
    const uint32_t bssSize = U32At(exheader, 0x3C, "ExHeader");
    if (stackSize == 0 || stackSize % kPageSize) {
        throw ForgeError("main stack size is zero or not page aligned");
    }
    const uint32_t bssPages = AlignUp(bssSize, kPageSize);
    const uint64_t dataMappedSize = dataPages + bssPages;
    if (data.Address + dataMappedSize >= kHeapVaddrEnd - stackSize) {
        throw ForgeError("process image collides with the main stack");
    }
    std::string processName(reinterpret_cast<const char*>(exheader.data()),
                            strnlen(reinterpret_cast<const char*>(exheader.data()), 8));
    if (std::any_of(processName.begin(), processName.end(), [](char c) { return c & 0x80; })) {
        throw ForgeError("ExHeader process name is not ASCII");
    }
    const json segments = json::array({
        {{"name", "text"}, {"address", text.Address}, {"mapped_size", textPages},
         {"declared_code_size", text.CodeSize}, {"file_offset", 0}, {"file_size", textPages},
         {"writable", false}, {"executable", true}},
        {{"name", "rodata"}, {"address", ro.Address}, {"mapped_size", roPages},
         {"declared_code_size", ro.CodeSize}, {"file_offset", textPages}, {"file_size", roPages},
         {"writable", false}, {"executable", false}},
        {{"name", "data_bss"}, {"address", data.Address}, {"mapped_size", dataMappedSize},
         {"declared_code_size", data.CodeSize}, {"file_offset", textPages + roPages},
         {"file_size", dataPages}, {"writable", true}, {"executable", false}},
    });
    const auto [memoryMode, appMemory, systemMemory, baseMemory] = CtrMemoryLayout(exheader);
    const uint64_t initialCommit = textPages + roPages + dataMappedSize + stackSize;
    const json systemRegions = json::array({
        {{"name", "ctr_vram"}, {"address", 0x1F000000}, {"mapped_size", 0x00600000},
         {"writable", true}, {"executable", false}, {"initial_values", json::array()}},
        {{"name", "ctr_dsp_ram"}, {"address", kDspRamVaddr}, {"mapped_size", kDspRamSize},
         {"writable", true}, {"executable", false}, {"initial_values", json::array()}},
        {{"name", "ctr_config_memory"}, {"address", kConfigMemoryVaddr}, {"mapped_size", kPageSize},
         {"writable", false}, {"executable", false},
         {"initial_values", json::array({
              InitialValue(0x01, 1, 0x3A), InitialValue(0x03, 1, 0x02),
              InitialValue(0x08, 8, 0x0004013000008002ULL), InitialValue(0x10, 4, 0x02),
              InitialValue(0x14, 1, 0x01), InitialValue(0x16, 1, 0x01),
              InitialValue(0x18, 4, 0x0000F450), InitialValue(0x30, 4, memoryMode),
              InitialValue(0x40, 4, appMemory), InitialValue(0x44, 4, systemMemory),
              InitialValue(0x48, 4, baseMemory), InitialValue(0x62, 1, 0x3A),
              InitialValue(0x63, 1, 0x02), InitialValue(0x64, 4, 0x02),
              InitialValue(0x68, 4, 0x0000F450)})}},
        {{"name", "ctr_shared_page"}, {"address", kSharedPageVaddr}, {"mapped_size", kPageSize},
         {"writable", false}, {"executable", false},
         {"initial_values", json::array({
              InitialValue(0x04, 1, 0x01), InitialValue(0x66, 1, 0x03), InitialValue(0x67, 1, 0x02),
              InitialValue(0x85, 1, 0x17), InitialValue(0x86, 1, 0x01)})}},
    });
    return {
        {"format", "oot3d_native_process_manifest_v1"},
        {"source",
         {{"exheader_path", exheaderPath},
          {"exheader_sha256", Sha256::Of(exheader.data(), exheader.size())},
          {"code_bin_path", codePath},
          {"code_bin_sha256", Sha256::Of(code.data(), code.size())},
          {"code_bin_size", code.size()},
          {"romfs_image_path", romfsPath},
          {"romfs_image_file_size", romfsSize},
          {"romfs_service_offset", serviceOffset},
          {"romfs_service_size", serviceSize},
          {"layout_authority",
           "CTR ExHeader; page/BSS/stack/TLS semantics verified against Azahar NCCH loader"}}},
        {"process",
         {{"name", processName},
          {"entrypoint", text.Address},
          {"page_size", kPageSize},
          {"bss_declared_size", bssSize},
          {"bss_mapped_size", bssPages},
          {"segments", segments},
          {"system_regions", systemRegions},
          {"resource_limit",
           {{"limit_values", {0x18, appMemory, 0x20, 0x20, 0x20, 0x08, 0x08, 0x10, 0x02, 0}},
            {"initial_values", {0, initialCommit, 1, 0, 0, 0, 0, 0, 0, 0}}}},
          {"linear_heap", {{"base_address", kLegacyLinearHeapVaddr}, {"size", appMemory}}},
          {"heap", {{"base_address", kHeapVaddr}, {"size", kHeapSize}}}}},
        {"primary_thread",
         {{"stack_base_address", kHeapVaddrEnd - stackSize},
          {"stack_size", stackSize},
          {"tls_base_address", kTlsAreaVaddr},
          {"tls_size", kTlsPageSize},
          {"thread_pointer", kTlsAreaVaddr},
          {"argument0", 0},
          {"initial_cpsr", kUser32Cpsr},
          {"initial_fpscr", kCtrMainFpscr},
          {"priority", exheader.at(0x20F)}}},
    };
}

std::string DumpPythonJson(const json& value) {
    // nlohmann keeps object keys sorted; with ASCII-only content its indent-2
    // form is Python's json.dumps(indent=2, sort_keys=True).
    return value.dump(2, ' ', true) + "\n";
}

// ---- PrepareTitle ------------------------------------------------------------------

PrepareResult PrepareTitle(const PrepareOptions& options, const ProgressFn& progress) {
    RemoveTree(options.Work);
    const fs::path extract = options.Work / "extract";
    const fs::path inputs = options.Work / "inputs";
    fs::create_directories(extract);
    fs::create_directories(inputs);

    if (progress) progress("extract", 0.0);
    const ExtractedInputs extracted = ExtractDecryptedRom(options.Rom, extract, progress);
    const std::vector<Recipe> recipes = LoadRecipes(options.Catalog);
    const Recipe recipe = MatchRecipe(recipes, extracted, progress);
    const json& r = recipe.Json;

    PrepareResult result;
    result.RecipeId = recipe.Id();
    result.ProgramId = extracted.ProgramId;
    const fs::path code = inputs / "code.bin", exheader = inputs / "exheader.bin",
                   romfs = inputs / "romfs.bin";
    const json recipeInputs = r.at("inputs");

    if (recipe.HasAdapter()) {
        // input_adapters.validate_adapter + adapt_extracted_inputs.
        const json& adapter = r["input_adapter"];
        static const std::set<std::string> kKeys{"format", "source_inputs", "code_copies",
                                                 "resource_algorithm"};
        std::set<std::string> keys;
        for (const auto& [key, _] : adapter.items()) keys.insert(key);
        if (keys != kKeys || adapter["format"] != "triaevum_revision_input_adapter_v1" ||
            adapter["resource_algorithm"] != "oot3d_usa_resources_to_eur_v2") {
            throw ForgeError("Unsupported revision input adapter");
        }
        if (adapter["source_inputs"]["exheader"] != recipeInputs["exheader"]) {
            throw ForgeError("This adapter must preserve the original ExHeader");
        }
        const json& record = adapter["code_copies"];
        const fs::path programPath = options.RecipeRoot / record.at("path").get<std::string>();
        const Identity programId = HashFile(programPath);
        if (programId != IdentityOf(record)) throw ForgeError("adapter COPY artifact is modified");
        const auto programBytes = ReadFile(programPath);
        const json program = json::parse(programBytes.begin(), programBytes.end());
        ValidateCopyProgram(program);
        if (program["source"] != adapter["source_inputs"]["code"] ||
            program["canonical"] != recipeInputs["code"]) {
            throw ForgeError("Code COPY artifact does not match revision contracts");
        }
        const json original = IdentitiesJson(extracted.CodeId, extracted.ExheaderId, extracted.RomfsId);
        if (original != adapter["source_inputs"]) {
            if (!r.contains("data_compatibility") || r["data_compatibility"].is_null()) {
                throw ForgeError("Adapter source identity mismatch");
            }
            VerifyFamily(r, extracted.Code, extracted.Exheader, extracted.Romfs, "source", progress);
        }
        const auto canonical = ApplyCopyProgram(ReadFile(extracted.Code), program);
        WriteBytes(code, canonical);
        CopyFile(extracted.Exheader, exheader);
        const json resources = NormalizeUsaRomfs(extracted.Romfs, romfs, progress);
        RemoveFile(extracted.Romfs);
        result.Code = {canonical.size(), Sha256::Of(canonical.data(), canonical.size())};
        result.Exheader = extracted.ExheaderId;
        result.Romfs = HashFile(romfs, progress, "verify");
        const json actual = IdentitiesJson(result.Code, result.Exheader, result.Romfs);
        if (actual != recipeInputs) {
            if (!r.contains("data_compatibility") || r["data_compatibility"].is_null()) {
                throw ForgeError("Normalized input identity mismatch; title was not activated");
            }
            VerifyFamily(r, code, exheader, romfs, "execution", progress);
        }
        result.Receipt = {{"format", "triaevum_input_adaptation_receipt_v1"},
                          {"recipe", result.RecipeId},
                          {"source_inputs", original},
                          {"execution_inputs", actual},
                          {"adapter", adapter},
                          {"resources", resources},
                          {"objects_compiled", 0}};
    } else {
        fs::rename(extracted.Code, code);
        fs::rename(extracted.Exheader, exheader);
        fs::rename(extracted.Romfs, romfs);
        result.Code = extracted.CodeId;
        result.Exheader = extracted.ExheaderId;
        result.Romfs = extracted.RomfsId;
        // forge.verify_sources.
        const json actual = IdentitiesJson(result.Code, result.Exheader, result.Romfs);
        if (actual != recipeInputs) {
            if (!r.contains("data_compatibility") || r["data_compatibility"].is_null()) {
                throw ForgeError("the extracted inputs do not match the revision recipe");
            }
            VerifyFamily(r, code, exheader, romfs, "execution", progress);
        }
    }
    RemoveTree(extract);

    const auto codeBytes = ReadFile(code), exheaderBytes = ReadFile(exheader);
    const json manifest = BuildProcessManifest(exheaderBytes, codeBytes, romfs,
                                               "inputs/exheader.bin", "inputs/code.bin",
                                               "inputs/romfs.bin");
    if (r.contains("process") && r["process"].contains("entrypoint") &&
        manifest["process"]["entrypoint"] != r["process"]["entrypoint"]) {
        throw ForgeError("process manifest entrypoint does not match recipe");
    }
    WriteFileAtomic(options.Work / "process-manifest.json", DumpPythonJson(manifest));

    // Replace the game's inputs only now that everything verified.
    fs::create_directories(options.Game);
    const fs::path previous = options.Game / "inputs.previous";
    RemoveTree(previous);
    if (fs::exists(options.Game / "inputs")) fs::rename(options.Game / "inputs", previous);
    fs::rename(inputs, options.Game / "inputs");
    fs::rename(options.Work / "process-manifest.json", options.Game / "process-manifest.json");
    RemoveTree(previous);
    RemoveTree(options.Work);
    return result;
}

} // namespace triaevum::forge

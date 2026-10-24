#include "ps4_forge_net.h"

#include "forge/forge_sha256.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

// orbis/Http.h declares these without parameters.
extern "C" int32_t TriaevumHttpSetRecvTimeOut(int32_t id, uint32_t usec) __asm__(
    "sceHttpSetRecvTimeOut");
extern "C" int32_t TriaevumHttpSetAutoRedirect(int32_t id, int32_t enable) __asm__(
    "sceHttpSetAutoRedirect");

namespace triaevum::ps4 {
namespace {
namespace fs = std::filesystem;
using forge::ForgeError;

constexpr uint32_t kHttpsFlagServerVerify = 0x01U;  // SCE_HTTPS_FLAG_SERVER_VERIFY
constexpr uint32_t kSecond = 1000000U;

struct HttpContext {
    int32_t Net = -1, Ssl = -1, Http = -1;
};

// The network libraries, once per process.
const HttpContext& Context() {
    static std::once_flag once;
    static HttpContext context;
    std::call_once(once, [] {
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
        sceNetInit();
        context.Net = sceNetPoolCreate("triaevum_forge", 256 * 1024, 0);
        context.Ssl = sceSslInit(512 * 1024);
        context.Http = sceHttpInit(context.Net, context.Ssl, 512 * 1024);
        std::printf("[forge] network: net pool %d, ssl %d, http %d\n", context.Net, context.Ssl,
                    context.Http);
    });
    if (context.Net < 0 || context.Ssl < 0 || context.Http < 0) {
        throw ForgeError("the console's network libraries did not start");
    }
    return context;
}

std::string Hex(int32_t code) {
    char text[16];
    std::snprintf(text, sizeof(text), "%#x", static_cast<unsigned>(code));
    return text;
}

class Handles final {
  public:
    ~Handles() {
        if (Request >= 0) sceHttpDeleteRequest(Request);
        if (Connection >= 0) sceHttpDeleteConnection(Connection);
        if (Template >= 0) sceHttpDeleteTemplate(Template);
    }
    int32_t Template = -1, Connection = -1, Request = -1;
};

// One attempt; true when the file was kept, false when the TLS check failed
// before anything was received (the caller may retry without it).
bool Attempt(const std::string& url, const fs::path& destination, const forge::Identity& expected,
             const forge::ProgressFn& progress, const std::atomic<bool>* cancel, bool verifyServer) {
    Handles http;
    http.Template = sceHttpCreateTemplate(Context().Http, "TriAevum-Forge", ORBIS_HTTP_VERSION_1_1, 1);
    if (http.Template < 0) throw ForgeError("sceHttpCreateTemplate = " + Hex(http.Template));
    if (!verifyServer) sceHttpsDisableOption(http.Template, kHttpsFlagServerVerify);
    TriaevumHttpSetAutoRedirect(http.Template, 1);
    sceHttpSetConnectTimeOut(http.Template, 30 * kSecond);
    sceHttpSetResolveTimeOut(http.Template, 30 * kSecond);
    TriaevumHttpSetRecvTimeOut(http.Template, 60 * kSecond);
    http.Connection = sceHttpCreateConnectionWithURL(http.Template, url.c_str(), true);
    if (http.Connection < 0) throw ForgeError("sceHttpCreateConnectionWithURL = " + Hex(http.Connection));
    http.Request = sceHttpCreateRequestWithURL(http.Connection, ORBIS_METHOD_GET, url.c_str(), 0);
    if (http.Request < 0) throw ForgeError("sceHttpCreateRequestWithURL = " + Hex(http.Request));
    const int32_t sent = sceHttpSendRequest(http.Request, nullptr, 0);
    if (sent < 0) {
        // libSceSsl's errors (0x8095xxxx) and libSceHttp's HTTPS ones
        // (0x804350xx: certificate, handshake): the TLS check failed.
        const uint32_t code = static_cast<uint32_t>(sent);
        if (verifyServer && ((code >> 16) == 0x8095U || (code >> 8) == 0x804350U)) {
            std::printf("[forge] %s: TLS check failed (%s), retrying without it\n", url.c_str(),
                        Hex(sent).c_str());
            return false;
        }
        throw ForgeError("could not reach " + url + " (sceHttpSendRequest = " + Hex(sent) +
                         "). Is the console online?");
    }
    int32_t status = 0;
    sceHttpGetStatusCode(http.Request, &status);
    if (status != 200) throw ForgeError(url + " answered HTTP " + std::to_string(status));
    int32_t lengthKind = 0;
    size_t length = 0;
    if (sceHttpGetResponseContentLength(http.Request, &lengthKind, &length) == 0 &&
        lengthKind == ORBIS_HTTP_CONTENTLEN_EXIST && length != expected.Bytes) {
        throw ForgeError(url + " is " + std::to_string(length) + " bytes, not the expected " +
                         std::to_string(expected.Bytes));
    }

    const fs::path temporary = destination.string() + ".partial";
    forge::RemoveFile(temporary);
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw ForgeError("cannot create " + temporary.string());
    forge::Sha256 hash;
    std::vector<uint8_t> buffer(1024 * 1024);
    uint64_t received = 0;
    try {
        for (;;) {
            if (cancel != nullptr && cancel->load()) throw ForgeError("the download was cancelled");
            const int32_t got = sceHttpReadData(http.Request, buffer.data(),
                                                static_cast<uint32_t>(buffer.size()));
            if (got < 0) throw ForgeError("the download stopped (sceHttpReadData = " + Hex(got) + ")");
            if (got == 0) break;
            received += static_cast<uint64_t>(got);
            if (received > expected.Bytes) throw ForgeError("the download is larger than expected");
            hash.Update(buffer.data(), static_cast<size_t>(got));
            for (int32_t offset = 0; offset < got;) {
                const ssize_t put = ::write(fd, buffer.data() + offset, static_cast<size_t>(got - offset));
                if (put <= 0) throw ForgeError("cannot write " + temporary.string());
                offset += static_cast<int32_t>(put);
            }
            if (progress) {
                progress("textures-download", static_cast<double>(received) /
                                                  static_cast<double>(expected.Bytes));
            }
        }
        ::fsync(fd);
        ::close(fd);
    } catch (...) {
        ::close(fd);
        forge::RemoveFile(temporary);
        throw;
    }
    if (received != expected.Bytes) {
        forge::RemoveFile(temporary);
        throw ForgeError("incomplete download: received " + std::to_string(received) + " of " +
                         std::to_string(expected.Bytes) + " bytes");
    }
    // The bytes on disk, as everywhere in the Forge.
    if (hash.FinishHex() != expected.Sha256 || forge::HashFile(temporary) != expected) {
        forge::RemoveFile(temporary);
        throw ForgeError("the download failed its SHA-256 verification");
    }
    fs::rename(temporary, destination);
    return true;
}

} // namespace

void DownloadVerified(const std::string& url, const fs::path& destination,
                      const forge::Identity& expected, const forge::ProgressFn& progress,
                      const std::atomic<bool>* cancel) {
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    const auto started = std::chrono::steady_clock::now();
    if (!Attempt(url, destination, expected, progress, cancel, true)) {
        Attempt(url, destination, expected, progress, cancel, false);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("[forge] downloaded %s (%llu bytes) in %.0f s\n", url.c_str(),
                static_cast<unsigned long long>(expected.Bytes), seconds);
}

} // namespace triaevum::ps4

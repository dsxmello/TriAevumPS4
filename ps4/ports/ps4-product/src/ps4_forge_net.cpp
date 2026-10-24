#include "ps4_forge_net.h"

#include "forge/forge_sha256.h"

#include <chrono>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

// orbis/Http.h declares these without parameters.
extern "C" int32_t TriaevumHttpSetRecvTimeOut(int32_t id, uint32_t usec) __asm__(
    "sceHttpSetRecvTimeOut");
extern "C" int32_t TriaevumHttpSetAutoRedirect(int32_t id, int32_t enable) __asm__(
    "sceHttpSetAutoRedirect");

namespace triaevum::ps4 {

void ForgeLog(const char* format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::printf("[forge] %s", text);
    std::fflush(stdout);
    // Seconds since the epoch: localtime needs the libc's time zone data,
    // which crashed TriAevum at start on the console (1.40).
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%lld ", static_cast<long long>(std::time(nullptr)));
    const int fd = ::open("/data/triaevum/forge/forge.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    (void)!::write(fd, stamp, std::strlen(stamp));
    (void)!::write(fd, text, std::strlen(text));
    ::close(fd);
}

namespace {
namespace fs = std::filesystem;
using forge::ForgeError;

constexpr uint32_t kHttpsFlagServerVerify = 0x01U;  // SCE_HTTPS_FLAG_SERVER_VERIFY
constexpr uint32_t kSecond = 1000000U;

std::string Hex(int32_t code) {
    char text[16];
    std::snprintf(text, sizeof(text), "%#x", static_cast<unsigned>(code));
    return text;
}

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
        ForgeLog("network: net pool %d, ssl %d, http %d\n", context.Net, context.Ssl,
                    context.Http);
        // The system's network configuration, for the log.
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
        sceNetCtlInit();
        auto text = [](int32_t code) {
            OrbisNetCtlInfo info{};
            const int32_t result = sceNetCtlGetInfo(code, &info);
            return result < 0 ? "(" + Hex(result) + ")" : std::string(info.ip_address);
        };
        OrbisNetCtlInfo proxy{};
        sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_HTTP_PROXY_CONFIG, &proxy);
        const uint32_t proxyConfig = proxy.http_proxy_config;
        std::string proxyServer;
        if (proxyConfig != 0) {
            OrbisNetCtlInfo server{}, port{};
            sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_HTTP_PROXY_SERVER, &server);
            sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_HTTP_PROXY_PORT, &port);
            proxyServer = std::string(server.http_proxy_server) + ":" + std::to_string(port.http_proxy_port);
        }
        ForgeLog("network: ip %s, netmask %s, gateway %s, dns %s / %s, http proxy %s\n",
                    text(ORBIS_NET_CTL_INFO_IP_ADDRESS).c_str(), text(ORBIS_NET_CTL_INFO_NETMASK).c_str(),
                    text(ORBIS_NET_CTL_INFO_DEFAULT_ROUTE).c_str(),
                    text(ORBIS_NET_CTL_INFO_PRIMARY_DNS).c_str(),
                    text(ORBIS_NET_CTL_INFO_SECONDARY_DNS).c_str(),
                    proxyConfig != 0 ? proxyServer.c_str() : "off");
    });
    if (context.Net < 0 || context.Ssl < 0 || context.Http < 0) {
        throw ForgeError("the console's network libraries did not start");
    }
    return context;
}


// The host's IPv4 address from the console's DNS, for the log and to tell a
// DNS that blocks the internet (common on consoles kept off PSN: every name
// resolves to 0.0.0.0 or a local address) from a slow connection.
std::string Resolve(const std::string& host) {
    const OrbisNetId resolver = sceNetResolverCreate("triaevum_forge", Context().Net, 0);
    if (resolver < 0) throw ForgeError("sceNetResolverCreate = " + Hex(resolver));
    OrbisNetInAddr address{};
    const auto started = std::chrono::steady_clock::now();
    const int32_t resolved = sceNetResolverStartNtoa(resolver, host.c_str(), &address,
                                                     10 * kSecond, 2, 0);
    sceNetResolverDestroy(resolver);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (resolved < 0) {
        throw ForgeError("the console's DNS could not resolve " + host + " (" + Hex(resolved) +
                         "). Check the console's Internet connection and DNS settings.");
    }
    const uint32_t ip = address.s_addr;  // network byte order
    const uint32_t a = ip & 0xFF, b = (ip >> 8) & 0xFF, c = (ip >> 16) & 0xFF, d = ip >> 24;
    char text[64];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", a, b, c, d);
    ForgeLog("%s -> %s (%.1f s)\n", host.c_str(), text, seconds);
    if (ip == 0 || a == 127 || a == 10 || (a == 192 && b == 168) || (a == 172 && (b & 0xF0) == 16)) {
        throw ForgeError(host + " resolves to " + text +
                         ": the console's DNS blocks the Internet (a setting often used to stay off "
                         "PSN). Use a public DNS in the console's network settings, or send "
                         "the archive over FTP to /data/triaevum/forge/.");
    }
    return text;
}

std::string HostOf(const std::string& url) {
    const size_t start = url.find("://");
    const size_t from = start == std::string::npos ? 0 : start + 3;
    return url.substr(from, url.find_first_of(":/", from) - from);
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
    const std::string address = Resolve(HostOf(url));
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
    const auto sendStarted = std::chrono::steady_clock::now();
    const int32_t sent = sceHttpSendRequest(http.Request, nullptr, 0);
    const double sendSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - sendStarted).count();
    ForgeLog("%s: send = %s after %.1f s\n", url.c_str(), Hex(sent).c_str(), sendSeconds);
    if (sent < 0) {
        // libSceSsl's errors (0x8095xxxx) and libSceHttp's HTTPS ones
        // (0x804350xx: certificate, handshake): the TLS check failed.
        const uint32_t code = static_cast<uint32_t>(sent);
        // A stalled handshake ends as SCE_HTTP_ERROR_TIMEOUT (0x80431068).
        if (verifyServer &&
            ((code >> 16) == 0x8095U || (code >> 8) == 0x804350U || code == 0x80431068U)) {
            ForgeLog("%s: TLS check failed (%s), retrying without it\n", url.c_str(),
                        Hex(sent).c_str());
            return false;
        }
        // 0x80431068 SCE_HTTP_ERROR_TIMEOUT: nothing answered at that address.
        throw ForgeError("could not reach " + HostOf(url) + " at " + address + " (" + Hex(sent) +
                         (code == 0x80431068U ? ", timed out" : "") +
                         "). Check the console's Internet connection (Settings > Network > Test "
                         "Internet Connection), or send the archive over FTP to /data/triaevum/forge/.");
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

// After a failed download: does plain HTTP to the same host get an answer?
// Tells "no Internet" (it times out too) from an HTTPS-only problem.
std::string Probe(const std::string& url) {
    const std::string probe = "http://" + HostOf(url) + "/";
    try {
        Handles http;
        http.Template = sceHttpCreateTemplate(Context().Http, "TriAevum-Forge", ORBIS_HTTP_VERSION_1_1, 1);
        if (http.Template < 0) return "probe template " + Hex(http.Template);
        TriaevumHttpSetAutoRedirect(http.Template, 0);
        sceHttpSetConnectTimeOut(http.Template, 10 * kSecond);
        TriaevumHttpSetRecvTimeOut(http.Template, 10 * kSecond);
        http.Connection = sceHttpCreateConnectionWithURL(http.Template, probe.c_str(), false);
        if (http.Connection < 0) return "probe connection " + Hex(http.Connection);
        http.Request = sceHttpCreateRequestWithURL(http.Connection, ORBIS_METHOD_GET, probe.c_str(), 0);
        if (http.Request < 0) return "probe request " + Hex(http.Request);
        const auto started = std::chrono::steady_clock::now();
        const int32_t sent = sceHttpSendRequest(http.Request, nullptr, 0);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        char text[160];
        if (sent < 0) {
            std::snprintf(text, sizeof(text), "plain HTTP to %s also failed (%s after %.0f s)",
                          probe.c_str(), Hex(sent).c_str(), seconds);
        } else {
            int32_t status = 0;
            sceHttpGetStatusCode(http.Request, &status);
            std::snprintf(text, sizeof(text), "plain HTTP to %s answered %d in %.1f s",
                          probe.c_str(), status, seconds);
        }
        ForgeLog("probe: %s\n", text);
        return text;
    } catch (const std::exception& error) {
        return error.what();
    }
}

} // namespace

void DownloadVerified(const std::string& url, const fs::path& destination,
                      const forge::Identity& expected, const forge::ProgressFn& progress,
                      const std::atomic<bool>* cancel) {
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    const auto started = std::chrono::steady_clock::now();
    try {
        if (!Attempt(url, destination, expected, progress, cancel, true)) {
            Attempt(url, destination, expected, progress, cancel, false);
        }
    } catch (const ForgeError& error) {
        const std::string probe = Probe(url);
        const bool online = probe.find("answered") != std::string::npos;
        throw ForgeError(std::string(error.what()) + " [" + probe + (online
            ? ": the console reaches the Internet, only HTTPS failed"
            : ": the console does not reach the Internet") + "]");
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ForgeLog("downloaded %s (%llu bytes) in %.0f s, %.1f MB/s\n", url.c_str(),
             static_cast<unsigned long long>(expected.Bytes), seconds,
             seconds > 0.0 ? expected.Bytes / 1048576.0 / seconds : 0.0);
}

} // namespace triaevum::ps4

// TriAevum PS4 product - the console Forge's verified HTTPS download
// (tools/triaevum_release/verified_download.py) over the system's libSceHttp.
#pragma once

#include "forge/forge_core.h"

#include <atomic>

namespace triaevum::ps4 {

// Downloads `url` (redirects followed) to `destination`, keeping it only when
// its size and SHA-256 are `expected`. The certificate chain is checked
// first; when the console's CA store rejects the server, the download is
// retried without that check, since the SHA-256 is what proves the file.
// `cancel` stops it between reads.
void DownloadVerified(const std::string& url, const std::filesystem::path& destination,
                      const forge::Identity& expected, const forge::ProgressFn& progress,
                      const std::atomic<bool>* cancel = nullptr);

} // namespace triaevum::ps4

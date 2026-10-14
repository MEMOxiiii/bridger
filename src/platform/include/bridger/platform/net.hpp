#pragma once

// HTTPS downloads and SHA-1 hashing, through the operating system (WinHTTP and BCrypt on Windows).
// Bridger uses the network for one thing only: downloading the Minecraft client jar from Mojang's
// servers when the user asks for it (docs/safety.md, "Network access"). Callers decide which hosts
// are allowed; this layer only refuses anything that is not HTTPS.

#include <bridger/core/result.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::platform {

// Called while downloading with the bytes received so far and the total (0 if the server did not
// say). Returning false cancels the download (ErrorCode::Cancelled).
using DownloadProgress = std::function<bool(u64 received, u64 total)>;

// GET `url` (https only; redirects are followed but never to plain http). Fails with
// LimitExceeded beyond `max_bytes`, IoError on network or HTTP errors (status other than 200).
[[nodiscard]] Result<std::vector<u8>> https_get(std::string_view url, u64 max_bytes, const DownloadProgress& progress = {});

// SHA-1 of `data` as 40 lowercase hex digits.
[[nodiscard]] Result<std::string> sha1_hex(std::span<const u8> data);

} // namespace bridger::platform

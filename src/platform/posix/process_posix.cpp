// POSIX: the executable's path (/proc/self/exe on Linux, _NSGetExecutablePath on macOS) and posix_spawn.

#include <bridger/platform/process.hpp>

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace bridger::platform {

Result<std::filesystem::path> executable_path() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return make_error(ErrorCode::IoError, "cannot get the executable path");
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    auto p = std::filesystem::canonical(buf, ec);
    if (ec) return std::filesystem::path(buf);
    return p;
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot get the executable path: " + ec.message());
    return p;
#endif
}

Status launch_detached(const std::filesystem::path& exe, std::span<const std::string> args) {
    std::vector<std::string> owned;
    owned.push_back(exe.string());
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) return make_error(ErrorCode::IoError, "could not start " + exe.string() + ": " + std::strerror(rc));
    return {};
}

} // namespace bridger::platform

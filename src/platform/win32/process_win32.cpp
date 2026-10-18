// Windows: GetModuleFileNameW and CreateProcessW.

#include <bridger/platform/paths.hpp>
#include <bridger/platform/process.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace bridger::platform {

Result<std::filesystem::path> executable_path() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return make_error(ErrorCode::IoError, "GetModuleFileNameW failed");
        if (n < buf.size()) return std::filesystem::path(std::wstring(buf.data(), n));
        if (buf.size() >= 32768) return make_error(ErrorCode::IoError, "executable path too long");
        buf.resize(buf.size() * 2);
    }
}

namespace {
// Command-line quoting for CommandLineToArgvW / the MSVC runtime: backslashes before a quote are doubled.
void append_quoted(std::wstring& out, const std::wstring& arg) {
    out += L'"';
    usize backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
}
} // namespace

Status launch_detached(const std::filesystem::path& exe, std::span<const std::string> args) {
    std::wstring cmd;
    append_quoted(cmd, exe.native());
    for (const std::string& a : args) {
        BRIDGER_TRY(const std::filesystem::path wide, path_from_utf8(a));
        cmd += L' ';
        append_quoted(cmd, wide.native());
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = exe.parent_path().native();
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                        dir.c_str(), &si, &pi))
        return make_error(ErrorCode::IoError, "could not start " + path_to_utf8(exe).value_or("the program") + " (error " +
                                                  std::to_string(GetLastError()) + ")");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

} // namespace bridger::platform

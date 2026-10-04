#include "Util.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#else
#include <unistd.h>
#include <climits>
#endif

namespace immidi {

bool readWholeFile(const std::string& utf8Path, std::vector<uint8_t>& out) {
    std::ifstream f(pathFromUtf8(utf8Path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeWholeFile(const std::string& utf8Path, const std::string& content) {
    std::ofstream f(pathFromUtf8(utf8Path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), std::streamsize(content.size()));
    return bool(f);
}

bool writeFileAtomic(const std::string& utf8Path, const std::string& content) {
    std::string tmp = utf8Path + ".tmp";
    if (!writeWholeFile(tmp, content)) return false;
    std::error_code ec;
    std::filesystem::rename(pathFromUtf8(tmp), pathFromUtf8(utf8Path), ec);
    if (ec) {
        // Some file systems refuse to replace; fall back to a direct write.
        std::filesystem::remove(pathFromUtf8(tmp), ec);
        return writeWholeFile(utf8Path, content);
    }
    return true;
}

std::string fileNameOf(const std::string& utf8Path) {
    return pathToUtf8(pathFromUtf8(utf8Path).filename());
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return lowerAscii(s.substr(s.size() - suffix.size())) == lowerAscii(suffix);
}

bool isMidiFileName(const std::string& p) {
    return endsWithNoCase(p, ".mid") || endsWithNoCase(p, ".midi") || endsWithNoCase(p, ".kar") ||
           endsWithNoCase(p, ".rmi") || endsWithNoCase(p, ".smf") || endsWithNoCase(p, ".mds") ||
           endsWithNoCase(p, ".rcp") ||
           endsWithNoCase(p, ".xmi");
}

std::string formatTime(double seconds, bool withMillis) {
    if (seconds < 0) seconds = 0;
    int total = int(seconds);
    int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    char buf[64];
    if (withMillis) {
        int ms = int((seconds - total) * 1000.0);
        if (h > 0) snprintf(buf, sizeof buf, "%d:%02d:%02d.%03d", h, m, s, ms);
        else snprintf(buf, sizeof buf, "%d:%02d.%03d", m, s, ms);
    } else {
        if (h > 0) snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, s);
        else snprintf(buf, sizeof buf, "%d:%02d", m, s);
    }
    return buf;
}

std::string noteName(int note) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (note < 0 || note > 127) return "?";
    return std::string(names[note % 12]) + std::to_string(note / 12 - 1);
}

std::string configDirectory() {
    std::filesystem::path dir;
#if defined(_WIN32)
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata) dir = std::filesystem::path(appdata) / "ImMidi";
#elif defined(__APPLE__)
    if (const char* home = getenv("HOME")) dir = std::filesystem::path(home) / "Library" / "Application Support" / "ImMidi";
#else
    if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg) dir = std::filesystem::path(xdg) / "ImMidi";
    else if (const char* home = getenv("HOME")) dir = std::filesystem::path(home) / ".config" / "ImMidi";
#endif
    if (dir.empty()) dir = std::filesystem::current_path() / "ImMidi-config";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return pathToUtf8(dir);
}

std::string executableDirectory() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n > 0) return pathToUtf8(std::filesystem::path(std::wstring(buf, n)).parent_path());
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = std::filesystem::canonical(buf, ec);
        if (!ec) return pathToUtf8(p.parent_path());
    }
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return pathToUtf8(std::filesystem::path(std::string(buf, size_t(n))).parent_path());
#endif
    return pathToUtf8(std::filesystem::current_path());
}

std::string resourceDirectory() {
    std::string exe = executableDirectory();
#if defined(__APPLE__)
    std::filesystem::path p = pathFromUtf8(exe);
    if (p.filename() == "MacOS" && p.parent_path().filename() == "Contents") {
        std::filesystem::path res = p.parent_path() / "Resources";
        std::error_code ec;
        if (std::filesystem::is_directory(res, ec)) return pathToUtf8(res);
    }
#endif
    return exe;
}

} // namespace immidi

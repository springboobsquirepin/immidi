#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace immidi {

// Paths are passed around as UTF-8 strings; these convert at the OS boundary.
inline std::filesystem::path pathFromUtf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
#if defined(__cpp_char8_t)
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

bool readWholeFile(const std::string& utf8Path, std::vector<uint8_t>& out);
bool writeWholeFile(const std::string& utf8Path, const std::string& content);
// Writes to "<path>.tmp" and renames it over the target, so a crash never leaves a truncated file.
bool writeFileAtomic(const std::string& utf8Path, const std::string& content);
std::string fileNameOf(const std::string& utf8Path);
std::string lowerAscii(std::string s);
bool endsWithNoCase(const std::string& s, const std::string& suffix);
bool isMidiFileName(const std::string& utf8Path);

std::string formatTime(double seconds, bool withMillis = false);
std::string noteName(int note);  // "C4" style, middle C = C4 (note 60)

// Directory for configuration files (created when missing).
std::string configDirectory();
// Directory holding the executable.
std::string executableDirectory();
// Folder with the program's data (insdef/, conversion/, fonts/): Contents/Resources inside a macOS
// app bundle, otherwise the executable's folder.
std::string resourceDirectory();

} // namespace immidi

#include "SongModules.h"
#include "Util.h"

#include <filesystem>

namespace immidi {

std::string SongModules::absolute(const std::string& songPath) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(pathFromUtf8(songPath), ec);
    return ec ? songPath : pathToUtf8(p.lexically_normal());
}

std::string SongModules::key(const std::string& songPath) {
#if defined(_WIN32)
    std::string k = lowerAscii(absolute(songPath));
    for (char& c : k)
        if (c == '/') c = '\\';
    return k;
#elif defined(__APPLE__)
    return lowerAscii(absolute(songPath));
#else
    return absolute(songPath);
#endif
}

void SongModules::parse(const std::string& text) {
    songs_.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos || tab + 1 >= line.size()) continue;
        DeviceKind d;
        if (deviceFromId(line.substr(0, tab), d)) set(line.substr(tab + 1), d);
    }
}

std::string SongModules::serialize() const {
    std::string s = "# ImMidi: the module each song is made for, as set in the Device menu or the playlist.\n"
                    "# One song per line: the module (gm, gm2, sc55, sc88, sc88pro, sc8850, xg), a tab, the file.\n";
    for (auto& [k, v] : songs_) s += std::string(deviceId(v.first)) + "\t" + v.second + "\n";
    return s;
}

bool SongModules::load(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) return false;
    parse(std::string(bytes.begin(), bytes.end()));
    return true;
}

bool SongModules::save(const std::string& path) const { return writeFileAtomic(path, serialize()); }

bool SongModules::get(const std::string& songPath, DeviceKind& out) const {
    auto it = songs_.find(key(songPath));
    if (it == songs_.end()) return false;
    out = it->second.first;
    return true;
}

void SongModules::set(const std::string& songPath, DeviceKind module) { songs_[key(songPath)] = {module, absolute(songPath)}; }

void SongModules::clear(const std::string& songPath) { songs_.erase(key(songPath)); }

} // namespace immidi

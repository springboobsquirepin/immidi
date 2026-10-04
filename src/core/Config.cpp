#include "Config.h"
#include "Util.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace immidi {

bool Config::load(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) return false;
    std::string text(bytes.begin(), bytes.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return true;
}

std::string Config::serialize() const {
    std::string s = "# ImMidi settings\n";
    for (auto& [k, v] : values_) s += k + "=" + v + "\n";
    return s;
}

bool Config::save(const std::string& path) const { return writeFileAtomic(path, serialize()); }

std::string Config::get(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

int Config::getInt(const std::string& key, int def) const {
    auto it = values_.find(key);
    return it == values_.end() || it->second.empty() ? def : atoi(it->second.c_str());
}

double Config::getDouble(const std::string& key, double def) const {
    auto it = values_.find(key);
    return it == values_.end() || it->second.empty() ? def : atof(it->second.c_str());
}

bool Config::getBool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return def;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

void Config::setDouble(const std::string& key, double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.6g", v);
    values_[key] = buf;
}

} // namespace immidi

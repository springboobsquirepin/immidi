#pragma once
#include <map>
#include <string>

namespace immidi {

// Flat "key=value" settings file (UTF-8).
class Config {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    std::string serialize() const;

    std::string get(const std::string& key, const std::string& def = {}) const;
    int getInt(const std::string& key, int def) const;
    double getDouble(const std::string& key, double def) const;
    bool getBool(const std::string& key, bool def) const;

    void set(const std::string& key, const std::string& v) { values_[key] = v; }
    void setInt(const std::string& key, int v) { values_[key] = std::to_string(v); }
    void setDouble(const std::string& key, double v);
    void setBool(const std::string& key, bool v) { values_[key] = v ? "1" : "0"; }

private:
    std::map<std::string, std::string> values_;
};

} // namespace immidi

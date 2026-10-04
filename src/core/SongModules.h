#pragma once
#include "Standards.h"

#include <map>
#include <string>
#include <utility>

namespace immidi {

// The module each song is made for, as the user set it: for songs whose messages do not tell (an
// SC-8850 song without a GS reset is otherwise taken for a General MIDI song). Kept in a text file,
// one song per line: "<module id><TAB><path>" (ids as deviceId(): gm, gm2, sc55, ..., xg).
class SongModules {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;  // writes the file whole, atomically
    std::string serialize() const;
    void parse(const std::string& text);

    // False when the user did not set one for the song.
    bool get(const std::string& songPath, DeviceKind& out) const;
    void set(const std::string& songPath, DeviceKind module);
    void clear(const std::string& songPath);
    size_t size() const { return songs_.size(); }

private:
    // Songs are known by their absolute paths; Windows and macOS file names ignore case (and
    // Windows takes / for \).
    static std::string absolute(const std::string& songPath);
    static std::string key(const std::string& songPath);
    std::map<std::string, std::pair<DeviceKind, std::string>> songs_;  // key -> (module, absolute path)
};

} // namespace immidi

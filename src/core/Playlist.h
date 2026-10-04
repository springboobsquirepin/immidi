#pragma once
#include "SongInfo.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace immidi {

enum class PlayMode : uint8_t { Sequential = 0, RepeatAll, RepeatOne, Shuffle, Single, Count };
const char* playModeName(PlayMode m);

enum class InfoState : uint8_t { Unknown, Known, Unreadable };

struct PlaylistEntry {
    std::string path;
    std::string name;       // file name
    SongInfo info;          // read in the background after adding, again when the file is opened; saved with the playlist
    InfoState infoState = InfoState::Unknown;
    std::string infoError;  // why the file could not be read
    bool selected = false;  // selection in the playlist view (not saved)
    // The song's title, or the file name when the file names none.
    const std::string& shownTitle() const { return info.title.empty() ? name : info.title; }
};

enum class PlaylistSortKey : uint8_t { Title, FileName, Duration, Path };

class Playlist {
public:
    std::vector<PlaylistEntry> entries;
    int current = -1;
    PlayMode mode = PlayMode::Sequential;
    uint64_t revision = 0;       // bumped by every change to the list, the current entry or an entry's details
    uint64_t orderRevision = 0;  // bumped whenever entries are added, removed or reordered

    void add(const std::string& path);
    void add(PlaylistEntry entry);  // with the details it has (e.g. from another playlist)
    void addDirectory(const std::string& dir, bool recursive);
    void remove(int index);
    void clear();
    void move(int from, int to);
    // Moves the given entries (in list order) so they sit before the entry that was at `insertBefore`
    // (entries.size() = to the end). The current entry stays current.
    void moveEntries(std::vector<int> indices, int insertBefore);
    void removeEntries(std::vector<int> indices);
    std::vector<int> selectedIndices() const;
    // Moves the entries from `firstNew` to the end (just added) so they start at `index`.
    void placeAddedAt(int firstNew, int index);
    // Sorts the entries: text in natural order ("Track 2" before "Track 10"), ignoring case; unknown
    // durations last; equal entries keep their order. Returns the new index of each old index.
    std::vector<int> sortBy(PlaylistSortKey key, bool ascending);

    // Index of the track to play after the current one finished (-1 = stop).
    int nextAfterFinish();
    // Index for the "next" / "previous" buttons (always moves, wraps around).
    int nextManual();
    int previousManual();
    void setCurrent(int index);

    // .m3u8 with each entry's details on an "#IMMIDI-INFO:" line (other players ignore it), so
    // they need not be read again after a restart.
    bool save(const std::string& path) const;
    std::string serialize() const;
    bool load(const std::string& path);
    // Export for other players. ".m3u" files get a UTF-8 byte order mark so Windows players do not
    // read them in the ANSI code page; ".m3u8" is plain UTF-8. Relative paths are written relative
    // to the playlist's folder when possible.
    bool exportTo(const std::string& path, bool relativePaths) const;

private:
    void rebuildShuffle();
    int shufflePos() const;
    std::vector<int> shuffleOrder_;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace immidi

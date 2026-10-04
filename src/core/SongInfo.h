#pragma once
#include "TextCodec.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace immidi {

class MidiFile;

// What the playlist shows about a song, read from the file. Like foobar2000's tags, it is read
// once when the file is added (in the background) and again whenever the file is opened, and it
// is kept with the saved playlist in between.
struct SongInfo {
    std::string title;        // "" when the file names none (the playlist shows the file name)
    std::string copyright;
    std::string format;       // "SMF type 1", "Recomposer 2.0 (RCP)", ...
    int64_t durationUs = -1;  // -1 = unknown
    int tracks = 0;
    int ports = 0;
    uint64_t fileSize = 0;
};

SongInfo songInfoOf(const MidiFile& f);
// Reads a file's SongInfo (a light parse: the events are not kept). False with `error` if unreadable.
bool readSongInfo(const std::string& path, TextEncoding encoding, SongInfo& out, std::string& error);

// Reads SongInfo on a background thread, one file at a time, in the order asked for.
class SongInfoReader {
public:
    struct Result {
        std::string path;
        SongInfo info;
        bool ok = false;
        std::string error;
    };

    // `wake` runs on the reader thread when results are ready (at most every 100 ms, and when the
    // last queued file is done).
    explicit SongInfoReader(std::function<void()> wake);
    ~SongInfoReader();
    // The files still to read, in order; replaces the previous list (a file being read finishes).
    void setQueue(std::vector<std::string> paths);
    void setEncoding(TextEncoding encoding);
    std::vector<Result> takeResults();
    size_t pending() const;  // queued files, including the one being read

private:
    void run();

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::string> queue_;  // front = next
    size_t next_ = 0;
    std::string reading_;
    std::vector<Result> results_;
    TextEncoding encoding_ = TextEncoding::Auto;
    bool quit_ = false;
    std::function<void()> wake_;
    std::thread thread_;
};

} // namespace immidi

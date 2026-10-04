#include "SongInfo.h"
#include "MidiFile.h"

#include <algorithm>
#include <chrono>

namespace immidi {

// One line of text: track names may contain line breaks or tabs.
static std::string oneLine(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

SongInfo songInfoOf(const MidiFile& f) {
    SongInfo s;
    s.title = oneLine(f.title);
    s.copyright = oneLine(f.copyright);
    s.format = f.containerFormat == "SMF" ? "SMF type " + std::to_string(f.format) : f.containerFormat;
    s.durationUs = f.lengthUs;
    s.tracks = f.numTracks;
    s.ports = f.numPorts;
    s.fileSize = f.fileSize;
    return s;
}

bool readSongInfo(const std::string& path, TextEncoding encoding, SongInfo& out, std::string& error) {
    MidiFile f;
    if (!f.loadSummary(path, error)) return false;
    if (encoding != TextEncoding::Auto) f.setEncoding(encoding);
    out = songInfoOf(f);
    return true;
}

SongInfoReader::SongInfoReader(std::function<void()> wake) : wake_(std::move(wake)) {
    thread_ = std::thread([this] { run(); });
}

SongInfoReader::~SongInfoReader() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void SongInfoReader::setQueue(std::vector<std::string> paths) {
    std::lock_guard<std::mutex> lk(m_);
    // Not again: the file being read, and files whose result is not taken yet.
    paths.erase(std::remove_if(paths.begin(), paths.end(),
                               [&](const std::string& p) {
                                   return p == reading_ ||
                                          std::any_of(results_.begin(), results_.end(), [&](const Result& r) { return r.path == p; });
                               }),
                paths.end());
    queue_ = std::move(paths);
    next_ = 0;
    cv_.notify_all();
}

void SongInfoReader::setEncoding(TextEncoding encoding) {
    std::lock_guard<std::mutex> lk(m_);
    encoding_ = encoding;
}

std::vector<SongInfoReader::Result> SongInfoReader::takeResults() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Result> r;
    r.swap(results_);
    return r;
}

size_t SongInfoReader::pending() const {
    std::lock_guard<std::mutex> lk(m_);
    return queue_.size() - next_ + (reading_.empty() ? 0 : 1);
}

void SongInfoReader::run() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastWake;
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [&] { return quit_ || next_ < queue_.size(); });
        if (quit_) return;
        reading_ = queue_[next_++];
        Result r;
        r.path = reading_;
        TextEncoding enc = encoding_;
        lk.unlock();
        r.ok = readSongInfo(r.path, enc, r.info, r.error);
        lk.lock();
        reading_.clear();
        results_.push_back(std::move(r));
        if (wake_ && (next_ >= queue_.size() || Clock::now() - lastWake >= std::chrono::milliseconds(100))) {
            lastWake = Clock::now();
            lk.unlock();
            wake_();
            lk.lock();
        }
    }
}

} // namespace immidi

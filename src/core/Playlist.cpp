#include "Playlist.h"
#include "TextCodec.h"
#include "Util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace immidi {

const char* playModeName(PlayMode m) {
    switch (m) {
    case PlayMode::Sequential: return "Sequential";
    case PlayMode::RepeatAll: return "Repeat all";
    case PlayMode::RepeatOne: return "Repeat one";
    case PlayMode::Shuffle: return "Shuffle";
    case PlayMode::Single: return "Single (stop after track)";
    default: return "?";
    }
}

void Playlist::add(const std::string& path) {
    PlaylistEntry e;
    e.path = path;
    add(std::move(e));
}

void Playlist::add(PlaylistEntry e) {
    revision++;
    orderRevision++;
    if (e.name.empty()) e.name = fileNameOf(e.path);
    e.selected = false;
    entries.push_back(std::move(e));
    rebuildShuffle();
}

void Playlist::addDirectory(const std::string& dir, bool recursive) {
    revision++;
    orderRevision++;
    std::vector<std::string> found;
    std::error_code ec;
    auto visit = [&](const std::filesystem::directory_entry& de) {
        if (de.is_regular_file(ec)) {
            std::string p = pathToUtf8(de.path());
            if (isMidiFileName(p)) found.push_back(p);
        }
    };
    if (recursive) {
        for (auto it = std::filesystem::recursive_directory_iterator(pathFromUtf8(dir), std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            visit(*it);
        }
    } else {
        for (auto& de : std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) visit(de);
    }
    std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) { return lowerAscii(a) < lowerAscii(b); });
    for (const std::string& p : found) {
        PlaylistEntry e;
        e.path = p;
        e.name = fileNameOf(p);
        entries.push_back(std::move(e));
    }
    rebuildShuffle();
}

void Playlist::remove(int index) {
    revision++;
    orderRevision++;
    if (index < 0 || index >= int(entries.size())) return;
    entries.erase(entries.begin() + index);
    if (current == index) current = -1;
    else if (current > index) current--;
    rebuildShuffle();
}

void Playlist::moveEntries(std::vector<int> indices, int insertBefore) {
    int n = int(entries.size());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    indices.erase(std::remove_if(indices.begin(), indices.end(), [n](int i) { return i < 0 || i >= n; }), indices.end());
    if (indices.empty()) return;
    insertBefore = std::max(0, std::min(insertBefore, n));
    std::vector<bool> moving(size_t(n), false);
    for (int i : indices) moving[size_t(i)] = true;
    std::vector<int> order;  // old indices in their new order
    order.reserve(size_t(n));
    for (int i = 0; i < insertBefore; i++)
        if (!moving[size_t(i)]) order.push_back(i);
    order.insert(order.end(), indices.begin(), indices.end());
    for (int i = insertBefore; i < n; i++)
        if (!moving[size_t(i)]) order.push_back(i);
    bool same = true;
    for (int i = 0; i < n && same; i++) same = order[size_t(i)] == i;
    if (same) return;
    revision++;
    orderRevision++;
    std::vector<PlaylistEntry> moved;
    moved.reserve(size_t(n));
    int newCurrent = -1;
    for (int k = 0; k < n; k++) {
        if (order[size_t(k)] == current) newCurrent = k;
        moved.push_back(std::move(entries[size_t(order[size_t(k)])]));
    }
    entries.swap(moved);
    current = newCurrent;
    rebuildShuffle();
}

void Playlist::removeEntries(std::vector<int> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    int removedBeforeCurrent = 0;
    bool currentRemoved = false;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        int i = *it;
        if (i < 0 || i >= int(entries.size())) continue;
        entries.erase(entries.begin() + i);
        if (i == current) currentRemoved = true;
        else if (i < current) removedBeforeCurrent++;
    }
    current = currentRemoved ? -1 : (current >= 0 ? current - removedBeforeCurrent : -1);
    revision++;
    orderRevision++;
    rebuildShuffle();
}

std::vector<int> Playlist::selectedIndices() const {
    std::vector<int> v;
    for (int i = 0; i < int(entries.size()); i++)
        if (entries[size_t(i)].selected) v.push_back(i);
    return v;
}

void Playlist::clear() {
    revision++;
    orderRevision++;
    entries.clear();
    current = -1;
    shuffleOrder_.clear();
}

void Playlist::move(int from, int to) {
    revision++;
    orderRevision++;
    if (from < 0 || from >= int(entries.size()) || to < 0 || to >= int(entries.size()) || from == to) return;
    PlaylistEntry e = entries[from];
    entries.erase(entries.begin() + from);
    entries.insert(entries.begin() + to, e);
    if (current == from) current = to;
    else if (from < current && to >= current) current--;
    else if (from > current && to <= current) current++;
    rebuildShuffle();
}

void Playlist::placeAddedAt(int firstNew, int index) {
    int n = int(entries.size());
    if (firstNew < 0 || firstNew >= n || index < 0 || index >= firstNew) return;
    revision++;
    orderRevision++;
    std::rotate(entries.begin() + index, entries.begin() + firstNew, entries.end());
    if (current >= index && current < firstNew) current += n - firstNew;
    rebuildShuffle();
}

// Natural order, ignoring ASCII case: runs of digits compare by their value ("2" < "10").
static int naturalCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t i0 = i, j0 = j;
            while (i0 < a.size() && a[i0] == '0') i0++;
            while (j0 < b.size() && b[j0] == '0') j0++;
            size_t i1 = i0, j1 = j0;
            while (i1 < a.size() && digit(a[i1])) i1++;
            while (j1 < b.size() && digit(b[j1])) j1++;
            if (i1 - i0 != j1 - j0) return i1 - i0 < j1 - j0 ? -1 : 1;
            if (int c = a.compare(i0, i1 - i0, b, j0, j1 - j0)) return c;
            i = i1;
            j = j1;
            continue;
        }
        unsigned char x = static_cast<unsigned char>(lower(a[i])), y = static_cast<unsigned char>(lower(b[j]));
        if (x != y) return x < y ? -1 : 1;
        i++;
        j++;
    }
    return (i < a.size()) - (j < b.size());
}

std::vector<int> Playlist::sortBy(PlaylistSortKey key, bool ascending) {
    int n = int(entries.size());
    std::vector<int> order(static_cast<size_t>(n));  // old indices in their new order
    for (int i = 0; i < n; i++) order[size_t(i)] = i;
    auto text = [&](const PlaylistEntry& e) -> const std::string& {
        switch (key) {
        case PlaylistSortKey::FileName: return e.name;
        case PlaylistSortKey::Path: return e.path;
        default: return e.shownTitle();
        }
    };
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
        const PlaylistEntry &a = entries[size_t(x)], &b = entries[size_t(y)];
        if (key == PlaylistSortKey::Duration) {
            int64_t da = a.info.durationUs, db = b.info.durationUs;
            if ((da < 0) != (db < 0)) return db < 0;  // unknown last, either way round
            return ascending ? da < db : db < da;
        }
        int c = naturalCompare(text(a), text(b));
        return ascending ? c < 0 : c > 0;
    });
    std::vector<int> newIndex(static_cast<size_t>(n));
    std::vector<PlaylistEntry> sorted;
    sorted.reserve(size_t(n));
    for (int k = 0; k < n; k++) {
        newIndex[size_t(order[size_t(k)])] = k;
        sorted.push_back(std::move(entries[size_t(order[size_t(k)])]));
    }
    entries.swap(sorted);
    if (current >= 0 && current < n) current = newIndex[size_t(current)];
    revision++;
    orderRevision++;
    rebuildShuffle();
    return newIndex;
}

void Playlist::rebuildShuffle() {
    shuffleOrder_.resize(entries.size());
    for (size_t i = 0; i < entries.size(); i++) shuffleOrder_[i] = int(i);
    std::shuffle(shuffleOrder_.begin(), shuffleOrder_.end(), rng_);
    // Keep the current track first so the whole list plays before repeating.
    auto it = std::find(shuffleOrder_.begin(), shuffleOrder_.end(), current);
    if (it != shuffleOrder_.end()) std::iter_swap(shuffleOrder_.begin(), it);
}

int Playlist::shufflePos() const {
    auto it = std::find(shuffleOrder_.begin(), shuffleOrder_.end(), current);
    return it == shuffleOrder_.end() ? -1 : int(it - shuffleOrder_.begin());
}

void Playlist::setCurrent(int index) {
    revision++;
    current = (index >= 0 && index < int(entries.size())) ? index : -1;
}

int Playlist::nextAfterFinish() {
    int n = int(entries.size());
    if (n == 0) return -1;
    switch (mode) {
    case PlayMode::RepeatOne: return current >= 0 ? current : 0;
    case PlayMode::Single: return -1;
    case PlayMode::Sequential: return current + 1 < n ? current + 1 : -1;
    case PlayMode::RepeatAll: return (current + 1) % n;
    case PlayMode::Shuffle: {
        int pos = shufflePos();
        if (pos < 0 || pos + 1 >= int(shuffleOrder_.size())) {
            int last = current;
            rebuildShuffle();
            if (shuffleOrder_.size() > 1 && shuffleOrder_[0] == last) std::swap(shuffleOrder_[0], shuffleOrder_[1]);
            return shuffleOrder_.empty() ? -1 : shuffleOrder_[0];
        }
        return shuffleOrder_[size_t(pos + 1)];
    }
    default: return -1;
    }
}

int Playlist::nextManual() {
    int n = int(entries.size());
    if (n == 0) return -1;
    if (mode == PlayMode::Shuffle) {
        int pos = shufflePos();
        if (pos < 0 || pos + 1 >= int(shuffleOrder_.size())) {
            rebuildShuffle();
            return shuffleOrder_[0] == current && n > 1 ? shuffleOrder_[1] : shuffleOrder_[0];
        }
        return shuffleOrder_[size_t(pos + 1)];
    }
    return (current + 1 + n) % n;
}

int Playlist::previousManual() {
    int n = int(entries.size());
    if (n == 0) return -1;
    if (mode == PlayMode::Shuffle) {
        int pos = shufflePos();
        if (pos > 0) return shuffleOrder_[size_t(pos - 1)];
        return current >= 0 ? current : 0;
    }
    return current <= 0 ? n - 1 : current - 1;
}

// Details and titles on one line, without the tabs that separate them.
static std::string infoField(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

static std::string extInf(const PlaylistEntry& e) {
    if (e.infoState != InfoState::Known || e.info.durationUs < 0) return {};
    return "#EXTINF:" + std::to_string((e.info.durationUs + 500000) / 1000000) + "," + infoField(e.shownTitle()) + "\n";
}

static const char kInfoTag[] = "#IMMIDI-INFO:";

std::string Playlist::serialize() const {
    std::string s = "#EXTM3U\n";
    for (const PlaylistEntry& e : entries) {
        s += extInf(e);
        if (e.infoState == InfoState::Known) {
            const SongInfo& i = e.info;
            s += kInfoTag;
            s += "length=" + std::to_string(i.durationUs) + "\tsize=" + std::to_string(i.fileSize) + "\ttracks=" + std::to_string(i.tracks) +
                 "\tports=" + std::to_string(i.ports) + "\tformat=" + infoField(i.format) + "\ttitle=" + infoField(i.title) +
                 "\tcopyright=" + infoField(i.copyright) + "\n";
        }
        s += e.path + "\n";
    }
    return s;
}

static SongInfo parseInfo(const std::string& text) {
    SongInfo i;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t tab = text.find('\t', pos);
        if (tab == std::string::npos) tab = text.size();
        std::string field = text.substr(pos, tab - pos);
        pos = tab + 1;
        size_t eq = field.find('=');
        if (eq == std::string::npos) continue;
        std::string key = field.substr(0, eq), value = field.substr(eq + 1);
        if (key == "length") i.durationUs = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "size") i.fileSize = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "tracks") i.tracks = std::atoi(value.c_str());
        else if (key == "ports") i.ports = std::atoi(value.c_str());
        else if (key == "format") i.format = value;
        else if (key == "title") i.title = value;
        else if (key == "copyright") i.copyright = value;
    }
    return i;
}

bool Playlist::save(const std::string& path) const { return writeFileAtomic(path, serialize()); }

bool Playlist::exportTo(const std::string& path, bool relativePaths) const {
    std::filesystem::path base = pathFromUtf8(path).parent_path();
    std::string s;
    if (endsWithNoCase(path, ".m3u")) s = "\xEF\xBB\xBF";
    s += "#EXTM3U\n";
    for (const PlaylistEntry& e : entries) {
        s += extInf(e);
        std::filesystem::path p = pathFromUtf8(e.path);
        if (relativePaths) {
            std::error_code ec;
            std::filesystem::path rel = std::filesystem::relative(p, base, ec);
            if (!ec && !rel.empty()) p = rel;
        }
        s += pathToUtf8(p.make_preferred()) + "\n";
    }
    return writeFileAtomic(path, s);
}

bool Playlist::load(const std::string& path) {
    revision++;
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) return false;
    std::string text = decodeText(bytes.data(), bytes.size(), TextEncoding::Auto);
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) text.erase(0, 3);
    std::filesystem::path base = pathFromUtf8(path).parent_path();
    entries.clear();
    current = -1;
    orderRevision++;
    size_t pos = 0;
    PlaylistEntry next;  // collects the details line of the entry that follows
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, sizeof kInfoTag - 1, kInfoTag) == 0) {
            next.info = parseInfo(line.substr(sizeof kInfoTag - 1));
            next.infoState = InfoState::Known;
            continue;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::filesystem::path p = pathFromUtf8(line);
        if (p.is_relative()) p = base / p;
        next.path = pathToUtf8(p.lexically_normal());
        next.name = fileNameOf(next.path);
        entries.push_back(std::move(next));
        next = PlaylistEntry();
    }
    rebuildShuffle();
    return true;
}

} // namespace immidi

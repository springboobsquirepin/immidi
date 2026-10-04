#include "SmfExport.h"

#include "Emulation.h"
#include "MidiFile.h"
#include "Util.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace immidi {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

void putVlq(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = uint8_t(x & 0x7F);
    while (x >>= 7) buf[n++] = uint8_t(0x80 | (x & 0x7F));
    while (n) v.push_back(buf[--n]);
}

bool readVlq(const uint8_t* p, size_t end, size_t& i, uint32_t& v) {
    v = 0;
    for (int n = 0; n < 4; n++) {
        if (i >= end) return false;
        uint8_t c = p[i++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) return true;
    }
    return true;
}

struct Ev {
    int64_t tick;
    std::vector<uint8_t> bytes;  // complete event with its status byte (no running status)
};

struct Track {
    std::vector<Ev> events;
    int64_t endTick = 0;  // End Of Track position
};

bool isNoteOn(const Ev& e) { return e.bytes.size() >= 3 && (e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0; }
bool isNoteOff(const Ev& e) {
    return e.bytes.size() >= 3 && ((e.bytes[0] & 0xF0) == 0x80 || ((e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] == 0));
}
bool isChannel(const Ev& e) { return !e.bytes.empty() && e.bytes[0] >= 0x80 && e.bytes[0] < 0xF0; }

void writeSmf(uint16_t format, uint16_t division, const std::vector<Track>& tracks, std::vector<uint8_t>& out) {
    out.clear();
    out.insert(out.end(), {'M', 'T', 'h', 'd', 0, 0, 0, 6, uint8_t(format >> 8), uint8_t(format), uint8_t(tracks.size() >> 8),
                           uint8_t(tracks.size()), uint8_t(division >> 8), uint8_t(division)});
    for (const Track& t : tracks) {
        std::vector<uint8_t> body;
        int64_t last = 0;
        for (const Ev& e : t.events) {
            putVlq(body, uint32_t(e.tick - last));
            last = e.tick;
            body.insert(body.end(), e.bytes.begin(), e.bytes.end());
        }
        putVlq(body, uint32_t(std::max<int64_t>(0, t.endTick - last)));
        body.insert(body.end(), {0xFF, 0x2F, 0x00});
        uint32_t n = uint32_t(body.size());
        out.insert(out.end(), {'M', 'T', 'r', 'k', uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
        out.insert(out.end(), body.begin(), body.end());
    }
}

} // namespace

bool smfBytesForFile(const std::string& path, std::vector<uint8_t>& smf, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) {
        error = "Cannot read " + path;
        return false;
    }
    std::string convError;
    if (MidiFile::convertToSmf(bytes.data(), bytes.size(), smf, convError)) return true;
    size_t start = 0, size = bytes.size();
    if (size >= 20 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "RMID", 4) == 0) {
        for (size_t pos = 12; pos + 8 <= size;) {
            uint32_t len = le32(bytes.data() + pos + 4);
            if (std::memcmp(bytes.data() + pos, "data", 4) == 0) {
                start = pos + 8;
                size = std::min<size_t>(start + len, size);
                break;
            }
            pos += 8 + len + (len & 1);
        }
    }
    for (size_t i = start; i + 4 <= size; i++)
        if (std::memcmp(bytes.data() + i, "MThd", 4) == 0) {
            smf.assign(bytes.begin() + long(i), bytes.begin() + long(size));
            return true;
        }
    error = "Not a MIDI file";
    return false;
}

bool applyRpgMakerLoop(const std::vector<uint8_t>& in, int64_t loopStartTick, int64_t loopEndTick, std::vector<uint8_t>& out,
                       std::string& error, bool emidi, const std::vector<bool>& excludedTracks) {
    const uint8_t* d = in.data();
    size_t size = in.size();
    if (size < 14 || std::memcmp(d, "MThd", 4) != 0) {
        error = "Not a Standard MIDI File";
        return false;
    }
    uint32_t hdrLen = be32(d + 4);
    uint16_t format = uint16_t((d[8] << 8) | d[9]);
    uint16_t division = uint16_t((d[12] << 8) | d[13]);
    std::vector<Track> tracks;
    for (size_t pos = 8 + size_t(hdrLen); pos + 8 <= size;) {
        uint32_t len = be32(d + pos + 4);
        size_t body = pos + 8, end = std::min<size_t>(body + len, size);
        bool isTrack = std::memcmp(d + pos, "MTrk", 4) == 0;
        pos = body + len;
        if (!isTrack) continue;
        Track t;
        int64_t tick = 0;
        uint8_t running = 0;
        size_t i = body;
        while (i < end) {
            uint32_t delta = 0;
            if (!readVlq(d, end, i, delta) || i >= end) break;
            tick += delta;
            uint8_t st = d[i];
            if (st >= 0x80) i++;
            else if (running) st = running;
            else break;  // data byte without a status: stop reading this track
            Ev e{tick, {st}};
            if (st == 0xFF) {
                if (i >= end) break;
                uint8_t type = d[i++];
                uint32_t n = 0;
                if (!readVlq(d, end, i, n) || i + n > end) break;
                if (type == 0x2F) {
                    t.endTick = tick;
                    break;
                }
                e.bytes.push_back(type);
                putVlq(e.bytes, n);
                e.bytes.insert(e.bytes.end(), d + i, d + i + n);
                i += n;
                running = 0;
            } else if (st == 0xF0 || st == 0xF7) {
                uint32_t n = 0;
                if (!readVlq(d, end, i, n) || i + n > end) break;
                putVlq(e.bytes, n);
                e.bytes.insert(e.bytes.end(), d + i, d + i + n);
                i += n;
                running = 0;
            } else if (st >= 0xF1) {
                continue;  // system common / realtime bytes do not belong in files
            } else {
                running = st;
                int n = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
                if (i + size_t(n) > end) break;
                for (int k = 0; k < n; k++) e.bytes.push_back(d[i++] & 0x7F);
            }
            t.events.push_back(std::move(e));
        }
        t.endTick = std::max(t.endTick, tick);
        tracks.push_back(std::move(t));
    }
    if (tracks.empty()) {
        error = "MIDI file without tracks";
        return false;
    }
    if (emidi) {
        std::vector<Track> kept;
        for (size_t k = 0; k < tracks.size(); k++) {
            if (k < excludedTracks.size() && excludedTracks[k]) continue;
            Track& t = tracks[k];
            t.events.erase(std::remove_if(t.events.begin(), t.events.end(),
                                          [](const Ev& e) {
                                              return (e.bytes[0] & 0xF0) == 0xB0 && e.bytes.size() >= 3 && (e.bytes[1] == 110 || e.bytes[1] == 111);
                                          }),
                           t.events.end());
            kept.push_back(std::move(t));
        }
        if (!kept.empty()) tracks.swap(kept);
    }

    bool loop = format != 2 && loopStartTick >= 0 && loopEndTick > loopStartTick;
    if (loop) {
        // Cut every track at the loop end.
        bool haveCc111 = false;
        for (Track& t : tracks) {
            std::map<int, int> sounding;  // (channel << 7 | key) -> open note-ons
            std::vector<Ev> kept;
            for (Ev& e : t.events) {
                if (e.tick > loopEndTick) break;
                if (e.tick == loopEndTick) {
                    // Only what closes the loop: note-offs, meta events (markers) and the EMIDI /
                    // XMIDI loop end (CC#117). Note-ons and state changes belong to the next pass.
                    bool keep = isNoteOff(e) || e.bytes[0] == 0xFF ||
                                ((e.bytes[0] & 0xF0) == 0xB0 && e.bytes.size() >= 3 && e.bytes[1] == 117);
                    if (!keep) continue;
                }
                if (isNoteOn(e)) sounding[((e.bytes[0] & 0x0F) << 7) | e.bytes[1]]++;
                else if (isNoteOff(e)) {
                    auto it = sounding.find(((e.bytes[0] & 0x0F) << 7) | e.bytes[1]);
                    if (it != sounding.end() && it->second > 0) it->second--;
                }
                if ((e.bytes[0] & 0xF0) == 0xB0 && e.bytes.size() >= 3 && e.bytes[1] == 111 && e.tick == loopStartTick) haveCc111 = true;
                kept.push_back(std::move(e));
            }
            for (const auto& s : sounding)
                for (int k = 0; k < s.second; k++)
                    kept.push_back(Ev{loopEndTick, {uint8_t(0x80 | (s.first >> 7)), uint8_t(s.first & 0x7F), 0x40}});
            t.events.swap(kept);
            t.endTick = loopEndTick;
        }
        // CC#111 at the loop start, first among the events of that tick, in the first track that
        // plays on a channel (on that track's first channel).
        if (!haveCc111) {
            for (Track& t : tracks) {
                auto first = std::find_if(t.events.begin(), t.events.end(), isChannel);
                if (first == t.events.end()) continue;
                uint8_t ch = first->bytes[0] & 0x0F;
                auto at = std::find_if(t.events.begin(), t.events.end(), [&](const Ev& e) { return e.tick >= loopStartTick; });
                t.events.insert(at, Ev{loopStartTick, {uint8_t(0xB0 | ch), 111, 0}});
                break;
            }
        }
    }

    writeSmf(format, division, tracks, out);
    return true;
}

bool applyEmulation(const std::vector<uint8_t>& in, Emulator& emu, std::vector<uint8_t>& out, std::string& error) {
    int format = 1;
    uint16_t division = 96;
    std::vector<uint64_t> ends;
    std::vector<Track> tracks;
    std::vector<Bytes> msgs;
    emu.reset();
    auto add = [&](int track, uint64_t tick, const uint8_t* b, size_t n) {
        if (size_t(track) >= tracks.size()) tracks.resize(size_t(track) + 1);
        Ev e{int64_t(tick), {}};
        if (n && b[0] == 0xF0) {  // SysEx: F0, length, the rest
            e.bytes.push_back(0xF0);
            putVlq(e.bytes, uint32_t(n - 1));
            e.bytes.insert(e.bytes.end(), b + 1, b + n);
        } else {
            e.bytes.assign(b, b + n);
        }
        tracks[size_t(track)].events.push_back(std::move(e));
    };
    bool ok = MidiFile::forEachSmfEvent(
        in.data(), in.size(),
        [&](const SmfEventRef& e) {
            switch (e.kind) {
            case SmfEventRef::Meta: {
                std::vector<uint8_t> m = {0xFF, e.d1};
                putVlq(m, e.len);
                m.insert(m.end(), e.data, e.data + e.len);
                add(e.track, e.tick, m.data(), m.size());
                break;
            }
            case SmfEventRef::Raw: {  // escaped bytes (F7), sent unchanged
                std::vector<uint8_t> m = {0xF7};
                putVlq(m, e.len);
                m.insert(m.end(), e.data, e.data + e.len);
                add(e.track, e.tick, m.data(), m.size());
                break;
            }
            case SmfEventRef::Sysex:
                msgs.clear();
                emu.convert(e.port, e.data, e.len, msgs);
                for (const Bytes& b : msgs) add(e.track, e.tick, b.data(), b.size());
                break;
            case SmfEventRef::Channel: {
                uint8_t m[3] = {e.status, e.d1, e.d2};
                const uint8_t ty = e.status & 0xF0;
                const size_t n = (ty == 0xC0 || ty == 0xD0) ? 2 : 3;
                if (ty == 0xB0 || ty == 0xC0) {
                    msgs.clear();
                    emu.convert(e.port, m, n, msgs);
                    for (const Bytes& b : msgs) add(e.track, e.tick, b.data(), b.size());
                } else {
                    // Notes, pressure and bend are the same in every standard: as the player sends them,
                    // only the drum notes and velocities of a conversion table change.
                    if ((ty == 0x80 || ty == 0x90) && emu.mapsNotes() && !emu.mapNote(e.port, m)) break;
                    add(e.track, e.tick, m, n);
                }
                break;
            }
            }
        },
        format, division, ends, error);
    if (!ok) return false;
    tracks.resize(std::max(tracks.size(), ends.size()));
    for (size_t i = 0; i < tracks.size(); i++) {
        Track& t = tracks[i];
        t.endTick = i < ends.size() ? int64_t(ends[i]) : 0;
        if (!t.events.empty()) t.endTick = std::max(t.endTick, t.events.back().tick);
    }
    writeSmf(uint16_t(format), division, tracks, out);
    return true;
}

} // namespace immidi

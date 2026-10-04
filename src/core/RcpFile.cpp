#include "RcpFile.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace immidi {

// Recomposer 2.0 (RCM-PC98V2.0) layout:
//   0x000  32  signature "RCM-PC98V2.0(C)COME ON MUSIC\r\n"
//   0x020  64  title
//   0x060 336  comment, 12 lines of 28 characters
//   0x1C0   1  timebase (low byte)        0x1C1  tempo (BPM)
//   0x1C2   1  time signature numerator   0x1C3  denominator
//   0x1C4   1  key signature              0x1C5  play bias (global transpose, signed)
//   0x1C6  16  CM-64 setup file name      0x1D6  GS setup file name
//   0x1E6   1  track count (18 or 36)     0x1E7  timebase (high byte)
//   0x206 512  32 rhythm note definitions (14 name + key + gate)
//   0x406 384  8 user exclusives (24 name + 24 data)
//   0x586      tracks
// Track: u16 size (including this header), track number, rhythm mode, MIDI channel (0-15 port A,
// 16-31 port B, 0xFF off), key offset (bit 7 = not transposed), step offset, mute, 36-byte comment,
// then 4-byte events: command, step, gate, velocity (the last two are the parameters of non-notes).

namespace {

constexpr size_t kHeaderSize = 0x586;
constexpr size_t kTrackHeader = 44;

struct Ev {
    uint32_t tick;
    uint8_t prio;  // 0 = note off (goes first at equal ticks), 1 = everything else
    std::vector<uint8_t> bytes;  // SMF event without delta time
};

void putVlq(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = uint8_t(x & 0x7F);
    while (x >>= 7) buf[n++] = uint8_t(0x80 | (x & 0x7F));
    while (n) v.push_back(buf[--n]);
}

void putBe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

std::vector<uint8_t> meta(uint8_t type, const std::string& s) {
    std::vector<uint8_t> b = {0xFF, type};
    putVlq(b, uint32_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
    return b;
}

std::string fixedText(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// Builds a SysEx from Recomposer exclusive data: 0x80/0x81 insert the event's two values, 0x82 the
// channel, 0x83 starts the Roland checksum, 0x84 writes it, 0xF7 ends the message.
std::vector<uint8_t> buildSysex(const std::vector<uint8_t>& data, uint8_t v1, uint8_t v2, uint8_t channel) {
    std::vector<uint8_t> msg;
    unsigned sum = 0;
    for (uint8_t x : data) {
        if (x == 0xF7) break;
        switch (x) {
        case 0x80: x = v1; break;
        case 0x81: x = v2; break;
        case 0x82: x = channel & 0x0F; break;
        case 0x83: sum = 0; continue;
        case 0x84: x = uint8_t((0x80 - (sum & 0x7F)) & 0x7F); break;
        default: break;
        }
        msg.push_back(x & 0x7F);
        sum += x;
    }
    if (msg.empty()) return {};
    std::vector<uint8_t> b = {0xF0};
    putVlq(b, uint32_t(msg.size() + 1));
    b.insert(b.end(), msg.begin(), msg.end());
    b.push_back(0xF7);
    return b;
}

struct TrackOut {
    std::string name;
    std::vector<Ev> port[2];
    bool hasLoop = false;
    uint32_t loopStart = 0, loopEnd = 0;
};

} // namespace

bool isRcpData(const uint8_t* d, size_t size) {
    return size >= 28 && (std::memcmp(d, "RCM-PC98V2.0(C)COME ON MUSIC", 28) == 0 ||
                          std::memcmp(d, "COME ON MUSIC RECOMPOSER RCP3.0", 28) == 0);
}

bool rcpToSmf(const uint8_t* d, size_t size, std::vector<uint8_t>& smf, std::string& error) {
    if (size >= 28 && std::memcmp(d, "COME ON MUSIC RECOMPOSER RCP3.0", 28) == 0) {
        error = "Recomposer 3 files (.G36/.R36) are not supported yet";
        return false;
    }
    if (size < kHeaderSize || std::memcmp(d, "RCM-PC98V2.0(C)COME ON MUSIC", 28) != 0) {
        error = "not a Recomposer 2.0 file";
        return false;
    }
    uint16_t timebase = uint16_t(d[0x1C0] | (d[0x1E7] << 8));
    if (!timebase || timebase > 0x7FFF) timebase = 48;
    int baseTempo = d[0x1C1] ? d[0x1C1] : 120;
    int bias = int8_t(d[0x1C5]);
    int trackCount = d[0x1E6] == 36 ? 36 : 18;

    std::vector<TrackOut> tracks;
    std::vector<Ev> tempoEvents;
    size_t p = kHeaderSize;
    for (int tr = 0; tr < trackCount && p + kTrackHeader <= size; tr++) {
        size_t trackSize = size_t(d[p] | (d[p + 1] << 8));
        if (trackSize < kTrackHeader) break;
        const size_t base = p, end = std::min(size, p + trackSize);
        p += trackSize;
        uint8_t chan = d[base + 4], key = d[base + 5], mute = d[base + 7];
        int stepOffset = int8_t(d[base + 6]);
        if (mute) continue;
        int transpose = (key & 0x80) ? 0 : ((key & 0x40) ? int(key & 0x7F) - 128 : int(key & 0x7F)) + bias;

        TrackOut out;
        out.name = fixedText(d + base + 8, 36);
        uint32_t tick = 0;
        auto put = [&](uint8_t prio, std::vector<uint8_t> bytes, uint32_t at) {
            if (bytes.empty()) return;
            int port = (chan != 0xFF && chan >= 16) ? 1 : 0;
            out.port[port].push_back(Ev{at, prio, std::move(bytes)});
        };
        auto channelMsg = [&](uint8_t status, uint8_t d1, int d2 = -1) {
            if (chan == 0xFF) return;
            Ev e{tick, 1, {}};
            e.bytes.reserve(3);
            e.bytes.push_back(uint8_t(status | (chan & 0x0F)));
            e.bytes.push_back(uint8_t(d1 & 0x7F));
            if (d2 >= 0) e.bytes.push_back(uint8_t(d2 & 0x7F));
            out.port[chan >= 16 ? 1 : 0].push_back(std::move(e));
        };
        // A note that hits a key which is still sounding on the same track only extends it.
        std::map<int, size_t> sounding;  // (port, channel, key) -> index of its note off in out.port[]
        struct Loop {
            size_t pos;
            int done;
            uint32_t startTick;
        };
        std::vector<Loop> loops;
        size_t returnPos = 0;  // after a "same measure" reference, 0 = none
        uint8_t rolDev = 0x10, rolModel = 0x42, rolAddr[2] = {0, 0};
        uint8_t yamDev = 0x10, yamModel = 0x4C, yamAddr[2] = {0, 0};
        size_t i = base + kTrackHeader;
        for (uint32_t guard = 0; i + 4 <= end && guard < 4000000; guard++) {
            uint8_t cmd = d[i], step = d[i + 1], gate = d[i + 2], vel = d[i + 3];
            i += 4;
            if (cmd < 0x80) {
                int k = cmd + transpose;
                if (gate && chan != 0xFF && k >= 0 && k < 128) {
                    int port = chan >= 16 ? 1 : 0;
                    int id = (port << 11) | ((chan & 0x0F) << 7) | k;
                    auto s = sounding.find(id);
                    uint32_t off = tick + gate;
                    if (s != sounding.end() && out.port[port][s->second].tick > tick) {
                        Ev& e = out.port[port][s->second];
                        e.tick = std::max(e.tick, off);
                    } else {
                        channelMsg(0x90, k, vel);
                        out.port[port].push_back(Ev{off, 0, {uint8_t(0x80 | (chan & 0x0F)), uint8_t(k), 0}});
                        sounding[id] = out.port[port].size() - 1;
                    }
                }
                tick += step;
                continue;
            }
            switch (cmd) {
            case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
                const uint8_t* def = d + 0x406 + (cmd - 0x90) * 48 + 24;
                put(1, buildSysex(std::vector<uint8_t>(def, def + 24), gate, vel, chan), tick);
                break;
            }
            case 0x98: {  // track exclusive; the data follows in 0xF7 continuation events
                std::vector<uint8_t> data;
                while (i + 4 <= end && d[i] == 0xF7) {
                    data.push_back(d[i + 2]);
                    data.push_back(d[i + 3]);
                    i += 4;
                }
                put(1, buildSysex(data, gate, vel, chan), tick);
                break;
            }
            case 0xD0: yamAddr[0] = gate; yamAddr[1] = vel; break;
            case 0xD1: yamDev = gate; yamModel = vel; break;
            case 0xD2: put(1, {0xF0, 8, 0x43, uint8_t(yamDev & 0x7F), uint8_t(yamModel & 0x7F), yamAddr[0], yamAddr[1], gate, vel, 0xF7}, tick); break;
            case 0xD3: put(1, {0xF0, 8, 0x43, 0x10, 0x4C, yamAddr[0], yamAddr[1], gate, vel, 0xF7}, tick); break;
            case 0xDD: rolAddr[0] = gate; rolAddr[1] = vel; break;
            case 0xDF: rolDev = gate; rolModel = vel; break;
            case 0xDE: {  // Roland DT1 to base address + gate
                uint8_t body[4] = {uint8_t(rolAddr[0] & 0x7F), uint8_t(rolAddr[1] & 0x7F), uint8_t(gate & 0x7F), uint8_t(vel & 0x7F)};
                unsigned sum = body[0] + body[1] + body[2] + body[3];
                put(1, {0xF0, 10, 0x41, uint8_t(rolDev & 0x7F), uint8_t(rolModel & 0x7F), 0x12, body[0], body[1], body[2], body[3],
                        uint8_t((0x80 - (sum & 0x7F)) & 0x7F), 0xF7},
                    tick);
                break;
            }
            case 0xE2:  // bank select + program
                channelMsg(0xB0, 0, vel);
                channelMsg(0xC0, gate);
                break;
            case 0xE6: chan = gate ? uint8_t(gate - 1) : 0xFF; break;
            case 0xE7:
                if (gate) {
                    uint32_t bpmTimes64 = uint32_t(baseTempo) * gate;
                    uint32_t us = uint32_t(60000000ull * 64 / bpmTimes64);
                    tempoEvents.push_back(Ev{uint32_t(std::max<int64_t>(0, int64_t(tick) + stepOffset)), 1, {0xFF, 0x51, 3, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)}});
                }
                break;
            case 0xEA: channelMsg(0xD0, gate); break;
            case 0xEB: channelMsg(0xB0, gate, vel); break;
            case 0xEC: channelMsg(0xC0, gate); break;
            case 0xED: channelMsg(0xA0, gate, vel); break;
            case 0xEE: channelMsg(0xE0, gate, vel); break;
            case 0xF6: {  // comment, continued by 0xF7 events
                std::string text;
                text += char(gate);
                text += char(vel);
                while (i + 4 <= end && d[i] == 0xF7) {
                    text += char(d[i + 2]);
                    text += char(d[i + 3]);
                    i += 4;
                }
                while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
                if (!text.empty()) put(1, meta(0x01, text), tick);
                break;
            }
            case 0xF7: break;
            case 0xF9:  // loop start
                if (loops.size() < 16) loops.push_back(Loop{i, 0, tick});
                continue;
            case 0xF8: {  // loop end, step = count (0 = endless)
                if (loops.empty()) continue;
                Loop& l = loops.back();
                l.done++;
                int count = step;
                if (!count) {
                    // Endless: play the section twice, remember it for the loop markers.
                    if (l.done == 1 && !out.hasLoop) {
                        out.hasLoop = true;
                        out.loopStart = l.startTick;
                        out.loopEnd = tick;
                    }
                    count = 2;
                }
                if (l.done < count) i = l.pos;
                else loops.pop_back();
                continue;
            }
            case 0xFC: {  // same measure: play the measure at byte offset (vel << 8 | gate)
                if (returnPos) continue;
                size_t target = base + ((size_t(vel) << 8) | gate);
                for (int n = 0; n < 16 && target + 4 <= end && d[target] == 0xFC; n++)
                    target = base + ((size_t(d[target + 3]) << 8) | d[target + 2]);
                if (target < base + kTrackHeader || target + 4 > end) continue;
                returnPos = i;
                i = target;
                continue;
            }
            case 0xFD:  // measure end
                if (returnPos) {
                    i = returnPos;
                    returnPos = 0;
                }
                continue;
            case 0xFE: i = end; continue;
            default: break;  // DX7/TX/FB-01 parameters, key scan, key signature, ...
            }
            tick += step;
        }
        if (stepOffset) {
            out.loopStart = uint32_t(std::max<int64_t>(0, int64_t(out.loopStart) + stepOffset));
            out.loopEnd = uint32_t(std::max<int64_t>(0, int64_t(out.loopEnd) + stepOffset));
            for (auto& v : out.port)
                for (Ev& e : v) e.tick = uint32_t(std::max<int64_t>(0, int64_t(e.tick) + stepOffset));
        }
        tracks.push_back(std::move(out));
    }

    // Conductor track: title, comment lines, tempo, time signature, loop markers.
    std::vector<Ev> conductor;
    std::string title = fixedText(d + 0x20, 64);
    if (!title.empty()) conductor.push_back(Ev{0, 1, meta(0x03, title)});
    std::vector<std::string> lines;
    for (int l = 0; l < 12; l++) lines.push_back(fixedText(d + 0x60 + l * 28, 28));
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    for (const std::string& l : lines) conductor.push_back(Ev{0, 1, meta(0x01, l)});
    uint32_t us = uint32_t(60000000 / baseTempo);
    conductor.push_back(Ev{0, 1, {0xFF, 0x51, 3, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)}});
    int num = d[0x1C2], den = d[0x1C3], denPow = 0;
    while (denPow < 7 && (1 << denPow) < den) denPow++;
    if (num >= 1 && num <= 32 && den >= 1 && (1 << denPow) == den)
        conductor.push_back(Ev{0, 1, {0xFF, 0x58, 4, uint8_t(num), uint8_t(denPow), 24, 8}});
    bool anyLoop = false, loopsAgree = true;
    uint32_t ls = 0, le = 0;
    for (const TrackOut& t : tracks) {
        if (!t.hasLoop) continue;
        if (!anyLoop) {
            anyLoop = true;
            ls = t.loopStart;
            le = t.loopEnd;
        } else if (t.loopStart != ls || t.loopEnd != le) {
            loopsAgree = false;
        }
    }
    loopsAgree = loopsAgree && anyLoop && le > ls;
    if (loopsAgree) {
        conductor.push_back(Ev{ls, 1, meta(0x06, "loopStart")});
        conductor.push_back(Ev{le, 1, meta(0x06, "loopEnd")});
    }
    conductor.insert(conductor.end(), tempoEvents.begin(), tempoEvents.end());

    auto writeTrack = [&](std::vector<Ev>& evs, const std::vector<std::vector<uint8_t>>& head, std::vector<uint8_t>& outSmf) {
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.tick != b.tick ? a.tick < b.tick : a.prio < b.prio; });
        std::vector<uint8_t> trk;
        for (const auto& h : head) {
            trk.push_back(0);
            trk.insert(trk.end(), h.begin(), h.end());
        }
        uint32_t last = 0;
        for (const Ev& e : evs) {
            putVlq(trk, e.tick - last);
            last = e.tick;
            trk.insert(trk.end(), e.bytes.begin(), e.bytes.end());
        }
        trk.insert(trk.end(), {0x00, 0xFF, 0x2F, 0x00});
        outSmf.insert(outSmf.end(), {'M', 'T', 'r', 'k'});
        putBe32(outSmf, uint32_t(trk.size()));
        outSmf.insert(outSmf.end(), trk.begin(), trk.end());
    };

    std::vector<uint8_t> body;
    int ntracks = 1;
    writeTrack(conductor, {}, body);
    for (TrackOut& t : tracks) {
        for (int port = 0; port < 2; port++) {
            if (t.port[port].empty()) continue;
            std::vector<std::vector<uint8_t>> head;
            if (!t.name.empty()) head.push_back(meta(0x03, t.name));
            if (port) head.push_back({0xFF, 0x21, 1, uint8_t(port)});
            writeTrack(t.port[port], head, body);
            ntracks++;
        }
    }
    smf.clear();
    smf.insert(smf.end(), {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1});
    smf.push_back(uint8_t(ntracks >> 8));
    smf.push_back(uint8_t(ntracks));
    smf.push_back(uint8_t(timebase >> 8));
    smf.push_back(uint8_t(timebase));
    smf.insert(smf.end(), body.begin(), body.end());
    return true;
}

} // namespace immidi

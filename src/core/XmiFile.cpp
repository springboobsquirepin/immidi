#include "XmiFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace immidi {

namespace {

constexpr int kPpqn = 960;
constexpr double kXmiTicksPerSecond = 120.0;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

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

// Collects the EVNT chunk of every FORM XMID, walking FORM/CAT containers.
void findEvents(const uint8_t* d, size_t begin, size_t end, std::vector<std::pair<const uint8_t*, size_t>>& out) {
    size_t off = begin;
    while (off + 8 <= end) {
        uint32_t len = be32(d + off + 4);
        size_t body = off + 8;
        size_t next = body + len + (len & 1);
        if (body + len > end) len = uint32_t(end - body);
        if ((std::memcmp(d + off, "FORM", 4) == 0 || std::memcmp(d + off, "CAT ", 4) == 0) && len >= 4)
            findEvents(d, body + 4, body + len, out);
        else if (std::memcmp(d + off, "EVNT", 4) == 0)
            out.push_back({d + body, len});
        off = next;
    }
}

struct Ev {
    uint32_t xmiTick;
    uint8_t prio;  // 0 = note off (first at equal ticks), 1 = other
    uint32_t order;
    std::vector<uint8_t> bytes;
    uint64_t smfTick = 0;
};

} // namespace

bool isXmiData(const uint8_t* d, size_t size) {
    if (size < 12) return false;
    if (std::memcmp(d, "FORM", 4) == 0 && (std::memcmp(d + 8, "XDIR", 4) == 0 || std::memcmp(d + 8, "XMID", 4) == 0)) return true;
    return std::memcmp(d, "CAT ", 4) == 0 && std::memcmp(d + 8, "XMID", 4) == 0;
}

bool xmiToSmf(const uint8_t* d, size_t size, std::vector<uint8_t>& smf, std::string& error, int sequence, int* sequenceCount) {
    std::vector<std::pair<const uint8_t*, size_t>> seqs;
    findEvents(d, 0, size, seqs);
    if (sequenceCount) *sequenceCount = int(seqs.size());
    if (seqs.empty()) {
        error = "XMIDI file without event data";
        return false;
    }
    if (sequence < 0 || sequence >= int(seqs.size())) sequence = 0;
    const uint8_t* p = seqs[size_t(sequence)].first;
    const size_t end = seqs[size_t(sequence)].second;

    std::vector<Ev> evs;
    std::vector<std::pair<uint32_t, uint32_t>> tempos;  // (xmi tick, microseconds per quarter note)
    uint32_t tick = 0, order = 0;
    size_t i = 0;
    while (i < end) {
        // Delay: the sum of the bytes below 0x80 before the next status byte.
        while (i < end && p[i] < 0x80) tick += p[i++];
        if (i >= end) break;
        uint8_t st = p[i++];
        if (st == 0xFF) {
            if (i >= end) break;
            uint8_t type = p[i++];
            uint32_t len = 0;
            if (!readVlq(p, end, i, len) || i + len > end) break;
            if (type == 0x2F) break;
            if (type == 0x51 && len == 3) {
                tempos.push_back({tick, (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2]});
            } else {
                Ev e{tick, 1, order++, {0xFF, type}};
                putVlq(e.bytes, len);
                e.bytes.insert(e.bytes.end(), p + i, p + i + len);
                evs.push_back(std::move(e));
            }
            i += len;
        } else if (st == 0xF0 || st == 0xF7) {
            uint32_t len = 0;
            if (!readVlq(p, end, i, len) || i + len > end) break;
            Ev e{tick, 1, order++, {st}};
            putVlq(e.bytes, len);
            e.bytes.insert(e.bytes.end(), p + i, p + i + len);
            evs.push_back(std::move(e));
            i += len;
        } else if (st >= 0x80) {
            uint8_t type = st & 0xF0;
            int n = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (i + size_t(n) > end) break;
            uint8_t d1 = p[i] & 0x7F, d2 = n == 2 ? (p[i + 1] & 0x7F) : 0;
            i += size_t(n);
            if (type == 0x90) {
                uint32_t dur = 0;
                if (!readVlq(p, end, i, dur)) break;
                evs.push_back(Ev{tick, 1, order++, {st, d1, d2}});
                if (d2) evs.push_back(Ev{tick + dur, 0, order++, {uint8_t(0x80 | (st & 0x0F)), d1, 0}});
                continue;
            }
            // Miles driver directives (channel lock, voice protect, timbre bank, callbacks, ...)
            // mean other things in EMIDI / RPG Maker files; the FOR / NEXT loop controllers 116 /
            // 117 are kept, they match the EMIDI loop controllers.
            if (type == 0xB0 && d1 >= 110 && d1 <= 120 && d1 != 116 && d1 != 117) continue;
            Ev e{tick, 1, order++, {st, d1}};
            if (n == 2) e.bytes.push_back(d2);
            evs.push_back(std::move(e));
        }
    }
    if (evs.empty()) {
        error = "XMIDI sequence without events";
        return false;
    }

    // Tempo map: XMIDI time -> SMF ticks at kPpqn.
    std::sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (tempos.empty() || tempos.front().first > 0) tempos.insert(tempos.begin(), {0, tempos.empty() ? 500000u : tempos.front().second});
    for (auto& t : tempos)
        if (t.second == 0) t.second = 500000;
    std::vector<double> segStart(tempos.size(), 0.0);
    for (size_t k = 1; k < tempos.size(); k++)
        segStart[k] = segStart[k - 1] + double(tempos[k].first - tempos[k - 1].first) / kXmiTicksPerSecond * 1e6 / tempos[k - 1].second * kPpqn;
    auto toSmf = [&](uint32_t t) -> uint64_t {
        size_t k = std::upper_bound(tempos.begin(), tempos.end(), t, [](uint32_t v, const auto& e) { return v < e.first; }) - tempos.begin();
        k = k ? k - 1 : 0;
        double x = segStart[k] + double(t - tempos[k].first) / kXmiTicksPerSecond * 1e6 / tempos[k].second * kPpqn;
        return uint64_t(std::llround(x));
    };
    for (Ev& e : evs) e.smfTick = toSmf(e.xmiTick);
    // A note of zero length still sounds: its note-off follows one tick later.
    for (size_t k = 0; k < evs.size(); k++)
        if (evs[k].prio == 0 && k > 0 && evs[k - 1].prio == 1 && evs[k - 1].xmiTick == evs[k].xmiTick && evs[k - 1].order + 1 == evs[k].order)
            evs[k].smfTick++;
    for (const auto& t : tempos)
        evs.push_back(Ev{t.first, 1, 0, {0xFF, 0x51, 3, uint8_t(t.second >> 16), uint8_t(t.second >> 8), uint8_t(t.second)}, toSmf(t.first)});
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
        if (a.smfTick != b.smfTick) return a.smfTick < b.smfTick;
        if (a.prio != b.prio) return a.prio < b.prio;
        return a.order < b.order;
    });

    std::vector<uint8_t> trk;
    uint64_t last = 0;
    for (const Ev& e : evs) {
        putVlq(trk, uint32_t(e.smfTick - last));
        last = e.smfTick;
        trk.insert(trk.end(), e.bytes.begin(), e.bytes.end());
    }
    trk.insert(trk.end(), {0x00, 0xFF, 0x2F, 0x00});
    smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, uint8_t(kPpqn >> 8), uint8_t(kPpqn & 0xFF), 'M', 'T', 'r', 'k'};
    uint32_t n = uint32_t(trk.size());
    smf.insert(smf.end(), {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
    smf.insert(smf.end(), trk.begin(), trk.end());
    return true;
}

} // namespace immidi

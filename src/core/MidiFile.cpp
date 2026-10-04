#include "MidiFile.h"
#include "RcpFile.h"
#include "XmiFile.h"
#include "Util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace immidi {

namespace {

struct Reader {
    const uint8_t* p;
    size_t pos, end;
    bool ok() const { return pos < end; }
    uint8_t u8() { return pos < end ? p[pos++] : 0; }
    uint8_t peek() const { return pos < end ? p[pos] : 0; }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4 && pos < end; i++) {
            uint8_t b = p[pos++];
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return v;
    }
};

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

int channelDataLength(uint8_t status) {
    uint8_t t = status & 0xF0;
    return (t == 0xC0 || t == 0xD0) ? 1 : 2;
}

} // namespace

std::string KeySignature::name() const {
    static const char* majors[15] = {"Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#"};
    static const char* minors[15] = {"Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#"};
    int i = std::clamp(int(sf) + 7, 0, 14);
    return std::string(minor ? minors[i] : majors[i]) + (minor ? " minor" : " major");
}

bool MidiFile::load(const std::string& p, std::string& error, std::atomic<float>* progress) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(p, bytes)) {
        error = "Cannot read file";
        return false;
    }
    path = p;
    fileName = fileNameOf(p);
    return loadFromMemory(bytes.data(), bytes.size(), error, progress);
}

bool MidiFile::loadSummary(const std::string& p, std::string& error) {
    summaryOnly_ = true;
    bool ok = load(p, error);
    summaryOnly_ = false;
    return ok;
}

namespace {

void putBe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void putVlq(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = uint8_t(x & 0x7F);
    while (x >>= 7) buf[n++] = uint8_t((x & 0x7F) | 0x80);
    while (n) v.push_back(buf[--n]);
}

// Converts a RIFF "MIDS" MIDI stream (Windows midiStream format) into a format 0 SMF.
bool mdsToSmf(const uint8_t* d, size_t size, std::vector<uint8_t>& smf, std::string& error) {
    uint32_t timeFormat = 0, flags = 0;
    const uint8_t* dataChunk = nullptr;
    size_t dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        uint32_t len = le32(d + pos + 4);
        const uint8_t* body = d + pos + 8;
        size_t avail = std::min<size_t>(len, size - pos - 8);
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && avail >= 12) {
            timeFormat = le32(body);
            flags = le32(body + 8);
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            dataChunk = body;
            dataLen = avail;
        }
        pos += 8 + len + (len & 1);
    }
    if (!dataChunk || dataLen < 4) {
        error = "MIDI stream file without data chunk";
        return false;
    }
    if (timeFormat == 0 || timeFormat > 0x7FFF) timeFormat = 96;  // SMPTE streams are not used in practice
    bool noStreamId = flags & 1;
    std::vector<uint8_t> trk;
    trk.reserve(dataLen);
    uint32_t blocks = le32(dataChunk);
    size_t p = 4;
    uint32_t pendingDelta = 0;
    uint64_t tick = 0;       // absolute tick of the last event written
    uint64_t cursor = 0;     // absolute tick while reading
    for (uint32_t b = 0; b < blocks && p + 8 <= dataLen; b++) {
        // Block header: absolute start tick (informational) and byte count. Windows' midiStream plays
        // the buffers back to back and only uses the deltas, so tkStart is ignored here too: some
        // converters write a tkStart that doesn't match the previous block's deltas, which would
        // otherwise open a gap in the middle of the song.
        uint32_t cb = le32(dataChunk + p + 4);
        p += 8;
        pendingDelta = uint32_t(cursor - tick);
        size_t end = std::min(dataLen, p + cb);
        while (p + (noStreamId ? 8 : 12) <= end) {
            uint32_t delta = le32(dataChunk + p);
            uint32_t ev = le32(dataChunk + p + (noStreamId ? 4 : 8));
            p += noStreamId ? 8 : 12;
            uint8_t type = uint8_t((ev >> 24) & 0x3F);  // strip MEVT_F_CALLBACK
            bool isLong = (ev >> 24) & 0x80;
            uint32_t param = ev & 0xFFFFFF;
            cursor += delta;
            pendingDelta = uint32_t(cursor - tick);
            if (isLong) {
                size_t n = std::min<size_t>(param, end - p);
                const uint8_t* ld = dataChunk + p;
                p += (param + 3) & ~size_t(3);
                if (type == 0x00 && n > 0) {  // MEVT_LONGMSG: SysEx
                    putVlq(trk, pendingDelta);
                    tick = cursor;
                    if (ld[0] == 0xF0) {
                        trk.push_back(0xF0);
                        putVlq(trk, uint32_t(n - 1));
                        trk.insert(trk.end(), ld + 1, ld + n);
                    } else {
                        trk.push_back(0xF7);
                        putVlq(trk, uint32_t(n));
                        trk.insert(trk.end(), ld, ld + n);
                    }
                }
                continue;
            }
            if (type == 0x00) {  // MEVT_SHORTMSG
                uint8_t st = uint8_t(param & 0xFF);
                if (st < 0x80 || st >= 0xF0) continue;
                putVlq(trk, pendingDelta);
                tick = cursor;
                trk.push_back(st);
                trk.push_back(uint8_t((param >> 8) & 0x7F));
                uint8_t t = st & 0xF0;
                if (t != 0xC0 && t != 0xD0) trk.push_back(uint8_t((param >> 16) & 0x7F));
            } else if (type == 0x01) {  // MEVT_TEMPO
                putVlq(trk, pendingDelta);
                tick = cursor;
                trk.insert(trk.end(), {0xFF, 0x51, 0x03, uint8_t(param >> 16), uint8_t(param >> 8), uint8_t(param)});
            }
            // MEVT_NOP and other types only carry time.
        }
        p = std::max(p, end);
    }
    putVlq(trk, uint32_t(cursor - tick));
    trk.insert(trk.end(), {0xFF, 0x2F, 0x00});
    smf.clear();
    smf.insert(smf.end(), {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1});
    smf.push_back(uint8_t(timeFormat >> 8));
    smf.push_back(uint8_t(timeFormat));
    smf.insert(smf.end(), {'M', 'T', 'r', 'k'});
    putBe32(smf, uint32_t(trk.size()));
    smf.insert(smf.end(), trk.begin(), trk.end());
    return true;
}

// Streaming parser for one MTrk chunk. Parses one event ahead so tracks can be merged
// without materialising and sorting every event.
struct TrackCursor {
    Reader r{nullptr, 0, 0};
    size_t start = 0;
    uint64_t tick = 0;
    uint8_t running = 0;
    uint8_t port = 0;
    int index = 0;
    bool done = false;
    bool include = true;
    bool haveName = false;
    std::vector<uint8_t> sysex;
    bool sysexOpen = false;
    uint64_t sysexTick = 0;
    uint64_t endTick = 0;

    // Look-ahead event
    enum Kind : uint8_t { Channel, Meta, Sysex, Raw };
    bool has = false;
    Kind kind = Channel;
    uint64_t evTick = 0;
    uint8_t status = 0, d1 = 0, d2 = 0, evPort = 0;
    const uint8_t* pdata = nullptr;
    uint32_t plen = 0;

    // Parses the next event into the look-ahead slot. Returns false at the end of the track.
    bool advance() {
        has = false;
        while (!done && r.ok()) {
            tick += r.vlq();
            if (!r.ok()) break;
            uint8_t b = r.peek();
            if (b == 0xFF) {
                r.u8();
                uint8_t type = r.u8();
                uint32_t mlen = r.vlq();
                if (r.pos + mlen > r.end) mlen = uint32_t(r.end - r.pos);
                const uint8_t* md = r.p + r.pos;
                r.pos += mlen;
                if (type == 0x2F) {
                    done = true;
                    break;
                }
                if (type == 0x21 && mlen >= 1) port = uint8_t(std::min<int>(md[0], kMaxPorts - 1));
                return set(Meta, tick, 0xFF, type, 0, md, mlen);
            }
            if (b == 0xF0 || b == 0xF7) {
                r.u8();
                uint32_t slen = r.vlq();
                if (r.pos + slen > r.end) slen = uint32_t(r.end - r.pos);
                const uint8_t* sd = r.p + r.pos;
                r.pos += slen;
                if (b == 0xF0) {
                    sysex.assign(1, 0xF0);
                    sysex.insert(sysex.end(), sd, sd + slen);
                    sysexTick = tick;
                    sysexOpen = !(slen > 0 && sd[slen - 1] == 0xF7);
                    if (!sysexOpen) return set(Sysex, tick, 0xF0, 0, 0, sysex.data(), uint32_t(sysex.size()));
                } else if (sysexOpen) {
                    sysex.insert(sysex.end(), sd, sd + slen);
                    if (slen > 0 && sd[slen - 1] == 0xF7) {
                        sysexOpen = false;
                        return set(Sysex, sysexTick, 0xF0, 0, 0, sysex.data(), uint32_t(sysex.size()));
                    }
                } else if (slen > 0) {
                    return set(Raw, tick, 0xF7, 0, 0, sd, slen);  // escaped bytes, sent verbatim
                }
                continue;
            }
            uint8_t st;
            if (b & 0x80) {
                r.u8();
                if (b >= 0xF0) {  // system common / real-time bytes are not valid in SMF
                    if (b == 0xF1 || b == 0xF3) r.u8();
                    else if (b == 0xF2) {
                        r.u8();
                        r.u8();
                    }
                    continue;
                }
                st = b;
                running = b;
            } else {
                if (!running) {
                    r.u8();
                    continue;
                }
                st = running;
            }
            uint8_t a = r.u8() & 0x7F, c = 0;
            if (channelDataLength(st) == 2) c = r.u8() & 0x7F;
            if ((st & 0xF0) == 0x90 && c == 0) {
                st = uint8_t(0x80 | (st & 0x0F));
                c = 0x40;
            }
            return set(Channel, tick, st, a, c, nullptr, 0);
        }
        done = true;
        endTick = std::max(endTick, tick);
        return false;
    }

    bool set(Kind k, uint64_t t, uint8_t s, uint8_t a, uint8_t c, const uint8_t* pd, uint32_t pl) {
        kind = k;
        evTick = t;
        status = s;
        d1 = a;
        d2 = c;
        evPort = port;
        pdata = pd;
        plen = pl;
        has = true;
        return true;
    }
};

} // namespace

bool MidiFile::loadFromMemory(const uint8_t* bytes, size_t size, std::string& error, std::atomic<float>* progress) {
    fileSize = size;
    events.clear();
    data.clear();
    tempos.clear();
    timeSigs.clear();
    keySigs.clear();
    texts.clear();
    trackNames.clear();
    trackStats.clear();
    portDeviceNames.clear();
    lyrics.clear();
    std::memset(channelMask, 0, sizeof channelMask);
    std::memset(noteChannelMask, 0, sizeof noteChannelMask);
    noteCount = sysexCount = 0;
    loopScan_ = LoopScan();
    containerFormat = "SMF";

    const uint8_t* smf = bytes;
    size_t smfSize = size;
    std::vector<uint8_t> converted;

    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "MIDS", 4) == 0) {
        if (!mdsToSmf(bytes, size, converted, error)) return false;
        smf = converted.data();
        smfSize = converted.size();
        containerFormat = "MIDI stream (MDS)";
    } else if (isRcpData(bytes, size)) {
        if (!rcpToSmf(bytes, size, converted, error)) return false;
        smf = converted.data();
        smfSize = converted.size();
        containerFormat = "Recomposer 2.0 (RCP)";
    } else if (isXmiData(bytes, size)) {
        int count = 0;
        if (!xmiToSmf(bytes, size, converted, error, 0, &count)) return false;
        smf = converted.data();
        smfSize = converted.size();
        containerFormat = count > 1 ? "XMIDI (sequence 1 of " + std::to_string(count) + ")" : std::string("XMIDI");
    } else if (size >= 20 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "RMID", 4) == 0) {
        // RIFF MIDI (.rmi): the SMF lives in the "data" chunk.
        size_t pos = 12;
        bool found = false;
        while (pos + 8 <= size) {
            uint32_t len = le32(bytes + pos + 4);
            if (std::memcmp(bytes + pos, "data", 4) == 0) {
                smf = bytes + pos + 8;
                smfSize = std::min<size_t>(len, size - pos - 8);
                found = true;
                break;
            }
            pos += 8 + len + (len & 1);
        }
        if (!found) {
            error = "RIFF file without MIDI data chunk";
            return false;
        }
        containerFormat = "RIFF MIDI (RMID)";
    }
    return parseSmf(smf, smfSize, error, progress);
}

bool MidiFile::convertToSmf(const uint8_t* bytes, size_t size, std::vector<uint8_t>& smf, std::string& error) {
    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "MIDS", 4) == 0) return mdsToSmf(bytes, size, smf, error);
    if (isRcpData(bytes, size)) return rcpToSmf(bytes, size, smf, error);
    if (isXmiData(bytes, size)) return xmiToSmf(bytes, size, smf, error);
    error = "not a MIDI stream (.mds), Recomposer (.rcp) or XMIDI (.xmi) file";
    return false;
}

bool MidiFile::parseSmf(const uint8_t* smf, size_t smfSize, std::string& error, std::atomic<float>* progress) {
    // Tolerate leading junk (e.g. MacBinary headers).
    size_t start = 0;
    bool haveHeader = false;
    for (size_t i = 0; i + 14 <= smfSize && i < 4096; i++) {
        if (std::memcmp(smf + i, "MThd", 4) == 0) {
            start = i;
            haveHeader = true;
            break;
        }
    }
    if (!haveHeader) {
        error = "Not a Standard MIDI File (no MThd header)";
        return false;
    }
    const uint8_t* h = smf + start;
    uint32_t hlen = be32(h + 4);
    format = be16(h + 8);
    rawDivision = be16(h + 12);
    if (rawDivision & 0x8000) {
        smpte = true;
        int8_t fps = int8_t(rawDivision >> 8);
        smpteFps = -fps;
        smpteTpf = rawDivision & 0xFF;
        if (smpteTpf <= 0) smpteTpf = 1;
        ppqn = 96;
    } else {
        smpte = false;
        ppqn = rawDivision ? rawDivision : 96;
    }

    // Locate the tracks.
    std::vector<TrackCursor> cur;
    size_t totalBytes = 0;
    for (size_t pos = start + 8 + hlen; pos + 8 <= smfSize;) {
        const uint8_t* ch = smf + pos;
        uint32_t len = be32(ch + 4);
        size_t bodyStart = pos + 8;
        size_t bodyEnd = std::min<size_t>(bodyStart + len, smfSize);
        pos = bodyStart + len;
        if (std::memcmp(ch, "MTrk", 4) != 0) continue;
        TrackCursor c;
        c.r = Reader{smf, bodyStart, bodyEnd};
        c.start = bodyStart;
        c.index = int(cur.size());
        cur.push_back(std::move(c));
        totalBytes += bodyEnd - bodyStart;
    }
    numTracks = int(cur.size());
    if (cur.empty()) {
        error = "MIDI file contains no tracks";
        return false;
    }
    trackNames.assign(cur.size(), std::string());
    trackStats.assign(cur.size(), TrackStat());

    // Apogee EMIDI track designation (CC#110 = play on device, CC#111 = exclude device) sits at the
    // start of each track. ImMidi behaves like a General MIDI (0) / Sound Canvas (1) device.
    emidi = false;
    emidiExcludedTracks = 0;
    for (TrackCursor& c : cur) {
        TrackCursor probe = c;
        int inc = -1;
        for (int n = 0; n < 256 && probe.advance(); n++) {
            if (probe.kind != TrackCursor::Channel || (probe.status & 0xF0) != 0xB0) continue;
            if (probe.d1 == 110) {
                emidi = true;
                bool ours = probe.d2 == 0 || probe.d2 == 1 || probe.d2 == 127;
                if (ours) inc = 1;
                else if (inc < 0) inc = 0;
            } else if (probe.d1 == 111 && (probe.d2 == 0 || probe.d2 == 1)) {
                inc = 0;
            }
        }
        c.include = inc != 0;
    }
    // Without CC#110 the file is not EMIDI: a CC#111 is then an RPG Maker loop start, which must
    // not silence its track.
    if (!emidi)
        for (TrackCursor& c : cur) c.include = true;
    if (emidi)
        for (TrackCursor& c : cur)
            if (!c.include) {
                trackStats[size_t(c.index)].excluded = true;
                emidiExcludedTracks++;
            }

    if (!summaryOnly_) events.reserve(std::max<size_t>(1024, smfSize / 4));

    // Tempo state while streaming (events arrive in time order).
    uint64_t tempoTick = 0;
    int64_t tempoUs = 0;
    uint32_t usPerQn = 500000;
    tempos.push_back({0, 0, 500000});
    double smpteTicksPerSec = (smpteFps == 29 ? 29.97 : double(smpteFps ? smpteFps : 25)) * smpteTpf;
    auto timeAt = [&](uint64_t tick) -> int64_t {
        if (smpte) return int64_t(double(tick) * 1000000.0 / smpteTicksPerSec);
        return tempoUs + int64_t((tick - tempoTick) * usPerQn / uint64_t(ppqn));
    };
    auto storePayload = [&](const uint8_t* p, uint32_t n) {
        uint32_t off = uint32_t(data.size());
        uint8_t len[4] = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
        data.insert(data.end(), len, len + 4);
        data.insert(data.end(), p, p + n);
        return off;
    };
    auto lowerNoSpace = [](const uint8_t* p, uint32_t n) {
        std::string o;
        for (uint32_t i = 0; i < n && i < 32; i++) {
            char ch = char(p[i]);
            if (ch == ' ' || ch == '_' || ch == '-') continue;
            o += char(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        }
        return o;
    };

    uint64_t lastEventTick = 0, maxEndTick = 0;
    bool anyEvent = false;
    size_t emitted = 0;
    int topSysexPort = 0;
    LoopScan& ls = loopScan_;

    auto emit = [&](TrackCursor& c) {
        uint64_t tick = c.evTick;
        int64_t t = timeAt(tick);
        TrackStat& stat = trackStats[size_t(c.index)];
        switch (c.kind) {
        case TrackCursor::Channel: {
            if (!c.include) return;
            MidiEvent e;
            e.timeUs = t;
            e.status = c.status;
            e.d1 = c.d1;
            e.d2 = c.d2;
            e.port = c.evPort;
            if (!summaryOnly_) events.push_back(e);
            uint8_t chn = c.status & 0x0F;
            channelMask[c.evPort] |= uint16_t(1u << chn);
            stat.events++;
            stat.channels |= uint16_t(1u << chn);
            if (stat.port < 0) stat.port = c.evPort;
            uint8_t ty = c.status & 0xF0;
            if (ty == 0x90) {
                noteChannelMask[c.evPort] |= uint16_t(1u << chn);
                noteCount++;
                stat.notes++;
            } else if (ty == 0xB0 && c.d1 >= 111 && c.d1 <= 119) {
                int64_t tk = int64_t(tick);
                if (c.d1 == 116 && ls.b116 < 0) {
                    ls.b116 = tk;
                    ls.count116 = c.d2;
                } else if (c.d1 == 117 && ls.b116 >= 0 && tk > ls.b116) ls.e117 = std::max(ls.e117, tk);
                else if (c.d1 == 118 && ls.b118 < 0) ls.b118 = tk;
                else if (c.d1 == 119 && ls.b118 >= 0 && tk > ls.b118) ls.e119 = std::max(ls.e119, tk);
                else if (c.d1 == 111 && ls.cc111 < 0) ls.cc111 = tk;
            }
            lastEventTick = std::max(lastEventTick, tick);
            anyEvent = true;
            break;
        }
        case TrackCursor::Sysex:
        case TrackCursor::Raw: {
            if (!summaryOnly_) {
                MidiEvent e;
                e.timeUs = t;
                e.status = c.status;
                e.port = c.evPort;
                e.aux = storePayload(c.pdata, c.plen);
                events.push_back(e);
            }
            topSysexPort = std::max(topSysexPort, int(c.evPort));
            stat.events++;
            if (c.kind == TrackCursor::Sysex) sysexCount++;
            break;
        }
        case TrackCursor::Meta: {
            stat.events++;
            uint8_t type = c.d1;
            const uint8_t* md = c.pdata;
            uint32_t mlen = c.plen;
            if (type == 0x51 && mlen >= 3 && !smpte) {
                uint32_t us = (uint32_t(md[0]) << 16) | (uint32_t(md[1]) << 8) | md[2];
                if (us == 0) us = 1;
                tempoUs = t;
                tempoTick = tick;
                usPerQn = us;
                if (tempos.back().tick == tick) tempos.back().usPerQn = us;
                else tempos.push_back({tick, t, us});
            } else if (type == 0x58 && mlen >= 2) {
                TimeSignature ts;
                ts.tick = tick;
                ts.num = md[0] ? md[0] : 4;
                ts.denPow = std::min<uint8_t>(md[1], 6);
                if (mlen >= 3) ts.clocks = md[2];
                if (mlen >= 4) ts.n32 = md[3];
                timeSigs.push_back(ts);
            } else if (type == 0x59 && mlen >= 2) {
                keySigs.push_back({tick, int8_t(md[0]), md[1]});
            } else if (type == 0x09 && mlen > 0) {
                std::string name(reinterpret_cast<const char*>(md), mlen);
                auto it = std::find(portDeviceNames.begin(), portDeviceNames.end(), name);
                size_t idx = size_t(it - portDeviceNames.begin());
                if (it == portDeviceNames.end()) portDeviceNames.push_back(name);
                c.port = uint8_t(std::min<size_t>(idx, kMaxPorts - 1));
            }
            if (type == 0x03 && !c.haveName) {
                c.haveName = true;
                trackNames[size_t(c.index)] = std::string(reinterpret_cast<const char*>(md), mlen);
            }
            if (type >= 1 && type <= 9) {
                TextItem ti;
                ti.tick = tick;
                ti.timeUs = t;
                ti.track = uint16_t(c.index);
                ti.type = type;
                ti.raw.assign(reinterpret_cast<const char*>(md), mlen);
                texts.push_back(std::move(ti));
                if (type == 0x01 || type == 0x06 || type == 0x07) {
                    std::string l = lowerNoSpace(md, mlen);
                    if ((l == "loopstart" || l == "loop") && ls.markerStart < 0) ls.markerStart = int64_t(tick);
                    else if (l == "loopend" && ls.markerEnd < 0) ls.markerEnd = int64_t(tick);
                }
            }
            break;
        }
        }
        if (progress && (++emitted & 0xFFFF) == 0 && totalBytes) {
            size_t done = 0;
            for (const TrackCursor& k : cur) done += k.r.pos - k.start;
            progress->store(float(double(done) / double(totalBytes)));
        }
    };

    if (format == 2) {
        // Independent sequences played one after another.
        uint64_t offset = 0;
        for (TrackCursor& c : cur) {
            c.tick = offset;
            c.endTick = offset;
            while (c.advance()) emit(c);
            offset = std::max(c.endTick, c.tick);
            maxEndTick = std::max(maxEndTick, offset);
        }
    } else {
        // k-way merge by (tick, track index): the same order as a stable sort by time.
        auto later = [&](int a, int b) {
            const TrackCursor& x = cur[size_t(a)];
            const TrackCursor& y = cur[size_t(b)];
            return x.evTick != y.evTick ? x.evTick > y.evTick : a > b;
        };
        std::vector<int> heap;
        for (TrackCursor& c : cur)
            if (c.advance()) heap.push_back(c.index);
        std::make_heap(heap.begin(), heap.end(), later);
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), later);
            int i = heap.back();
            TrackCursor& c = cur[size_t(i)];
            emit(c);
            if (c.advance()) std::push_heap(heap.begin(), heap.end(), later);
            else heap.pop_back();
        }
        for (const TrackCursor& c : cur) maxEndTick = std::max({maxEndTick, c.endTick, c.tick});
    }
    if (events.capacity() > events.size() + events.size() / 4 + 4096) events.shrink_to_fit();

    // Song length: honor trailing silence up to a bar or so, but not huge End-of-Track gaps.
    uint64_t barTicks = uint64_t(ppqn) * 4;
    if (!anyEvent) lastEventTick = maxEndTick;
    lengthTicks = maxEndTick;
    if (lengthTicks > lastEventTick + barTicks * 2) lengthTicks = lastEventTick + uint64_t(ppqn);
    if (lengthTicks < lastEventTick) lengthTicks = lastEventTick;
    lengthUs = tickToUs(lengthTicks);

    numPorts = 1;
    for (int p = kMaxPorts - 1; p > 0; p--)
        if (channelMask[p]) {
            numPorts = p + 1;
            break;
        }
    numPorts = std::max(numPorts, topSysexPort + 1);  // SysEx-only ports count too

    detectLoops();

    std::vector<std::string> raws;
    for (const TextItem& t : texts) raws.push_back(t.raw);
    for (const std::string& n : trackNames) raws.push_back(n);
    detectedEncoding = detectEncoding(raws);
    encoding = detectedEncoding;
    decodeTexts();
    if (progress) progress->store(1.0f);
    return true;
}

bool MidiFile::forEachSmfEvent(const uint8_t* smf, size_t size, const std::function<void(const SmfEventRef&)>& fn, int& format,
                               uint16_t& division, std::vector<uint64_t>& trackEnds, std::string& error) {
    size_t start = 0;
    bool haveHeader = false;
    for (size_t i = 0; i + 14 <= size && i < 4096; i++) {
        if (std::memcmp(smf + i, "MThd", 4) == 0) {
            start = i;
            haveHeader = true;
            break;
        }
    }
    if (!haveHeader) {
        error = "Not a Standard MIDI File (no MThd header)";
        return false;
    }
    const uint8_t* h = smf + start;
    format = be16(h + 8);
    division = be16(h + 12);
    std::vector<TrackCursor> cur;
    for (size_t pos = start + 8 + be32(h + 4); pos + 8 <= size;) {
        const uint8_t* ch = smf + pos;
        uint32_t len = be32(ch + 4);
        size_t bodyStart = pos + 8, bodyEnd = std::min<size_t>(bodyStart + len, size);
        pos = bodyStart + len;
        if (std::memcmp(ch, "MTrk", 4) != 0) continue;
        TrackCursor c;
        c.r = Reader{smf, bodyStart, bodyEnd};
        c.start = bodyStart;
        c.index = int(cur.size());
        cur.push_back(std::move(c));
    }
    if (cur.empty()) {
        error = "MIDI file contains no tracks";
        return false;
    }
    std::vector<std::string> deviceNames;  // FF 09: one port per name, as load() assigns them
    std::vector<uint64_t> base(cur.size(), 0);
    auto emit = [&](TrackCursor& c) {
        SmfEventRef e;
        e.kind = SmfEventRef::Kind(c.kind);
        e.track = c.index;
        e.tick = c.evTick - base[size_t(c.index)];
        e.port = c.evPort;
        e.status = c.status;
        e.d1 = c.d1;
        e.d2 = c.d2;
        e.data = c.pdata;
        e.len = c.plen;
        if (c.kind == TrackCursor::Meta && c.d1 == 0x09 && c.plen > 0) {
            std::string name(reinterpret_cast<const char*>(c.pdata), c.plen);
            auto it = std::find(deviceNames.begin(), deviceNames.end(), name);
            size_t idx = size_t(it - deviceNames.begin());
            if (it == deviceNames.end()) deviceNames.push_back(name);
            c.port = uint8_t(std::min<size_t>(idx, kMaxPorts - 1));
        }
        fn(e);
    };
    if (format == 2) {
        uint64_t offset = 0;
        for (TrackCursor& c : cur) {
            c.tick = offset;
            c.endTick = offset;
            base[size_t(c.index)] = offset;
            while (c.advance()) emit(c);
            offset = std::max(c.endTick, c.tick);
        }
    } else {
        // The same k-way merge as load(): by tick, then track index.
        auto later = [&](int a, int b) {
            const TrackCursor& x = cur[size_t(a)];
            const TrackCursor& y = cur[size_t(b)];
            return x.evTick != y.evTick ? x.evTick > y.evTick : a > b;
        };
        std::vector<int> heap;
        for (TrackCursor& c : cur)
            if (c.advance()) heap.push_back(c.index);
        std::make_heap(heap.begin(), heap.end(), later);
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), later);
            TrackCursor& c = cur[size_t(heap.back())];
            emit(c);
            if (c.advance()) std::push_heap(heap.begin(), heap.end(), later);
            else heap.pop_back();
        }
    }
    trackEnds.clear();
    for (const TrackCursor& c : cur) trackEnds.push_back(std::max(c.endTick, c.tick) - base[size_t(c.index)]);
    return true;
}

void MidiFile::detectLoops() {
    loopStartTick = loopEndTick = -1;
    loopStartUs = loopEndUs = -1;
    loopCount = 0;
    loopType.clear();
    const LoopScan& ls = loopScan_;
    if (ls.markerStart >= 0) {
        // 1) Marker / text / cue meta events "loopStart" and "loopEnd".
        loopStartTick = ls.markerStart;
        loopEndTick = ls.markerEnd >= 0 ? ls.markerEnd : int64_t(lengthTicks);
        loopType = "Marker";
    } else if (ls.b116 >= 0) {
        // 2) Apogee EMIDI: CC#116 loop begin (value = count, 0 = forever), CC#117 loop end.
        loopStartTick = ls.b116;
        loopEndTick = ls.e117 >= 0 ? ls.e117 : int64_t(lengthTicks);
        loopCount = ls.count116;
        loopType = "EMIDI";
    } else if (ls.b118 >= 0) {
        // CC#118/119 are per-track loops; used as a global loop when no CC#116 is present.
        loopStartTick = ls.b118;
        loopEndTick = ls.e119 >= 0 ? ls.e119 : int64_t(lengthTicks);
        loopType = "EMIDI (track)";
    } else if (ls.cc111 >= 0 && !emidi) {
        // 3) RPG Maker: CC#111 marks the loop start, the loop end is the end of the song.
        loopStartTick = ls.cc111;
        loopEndTick = int64_t(lengthTicks);
        loopType = "RPG Maker CC#111";
    }
    if (hasLoop()) {
        if (uint64_t(loopEndTick) > lengthTicks) loopEndTick = int64_t(lengthTicks);
        loopStartUs = tickToUs(uint64_t(loopStartTick));
        loopEndUs = tickToUs(uint64_t(loopEndTick));
    } else {
        loopStartTick = loopEndTick = -1;
        loopType.clear();
    }
}

void MidiFile::setEncoding(TextEncoding enc) {
    encoding = (enc == TextEncoding::Auto) ? detectedEncoding : enc;
    decodeTexts();
}

void MidiFile::decodeTexts() {
    for (TextItem& t : texts) t.text = decodeText(t.raw, encoding);
    title.clear();
    copyright.clear();
    // Title: first track name in the first track (format 1 convention), otherwise first sequence name.
    for (const TextItem& t : texts) {
        if (t.type == 3 && t.track == 0 && t.tick == 0 && !t.text.empty()) {
            title = t.text;
            break;
        }
    }
    if (title.empty()) {
        for (const TextItem& t : texts)
            if (t.type == 3 && !t.text.empty()) {
                title = t.text;
                break;
            }
    }
    for (const TextItem& t : texts) {
        if (t.type == 2 && !t.text.empty()) {
            copyright = t.text;
            break;
        }
    }
    // Karaoke (.kar) header: "@T" lines carry the song title.
    for (const TextItem& t : texts) {
        if (t.type == 1 && t.text.size() > 2 && t.text[0] == '@' && t.text[1] == 'T') {
            title = t.text.substr(2);
            break;
        }
    }
    // Trim
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') i++;
        s.erase(0, i);
    };
    trim(title);
    trim(copyright);
    buildLyrics();
}

namespace {

// The characters of decoded (UTF-8) text.
std::vector<uint32_t> codePoints(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i++]);
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        uint32_t cp = more == 3 ? c & 0x07u : more == 2 ? c & 0x0Fu : more == 1 ? c & 0x1Fu : c;
        for (; more > 0 && i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80; more--) cp = (cp << 6) | (uint8_t(s[i++]) & 0x3Fu);
        out.push_back(cp);
    }
    return out;
}

// The spaces between lyric words: ASCII ones and the full-width space of Japanese lyrics (U+3000).
bool lyricSpace(uint32_t c) { return c == ' ' || c == '\t' || c == 0x3000; }

// Characters two columns wide: CJK, kana, hangul and the full-width forms.
bool wideChar(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || c >= 0x20000;
}

// A lyric event's text without the spaces and line breaks around it: whether anything is left, whether it
// has spaces between its words, and its width in columns.
struct LyricShape {
    bool blank = true, innerSpace = false;
    int columns = 0;
};

LyricShape lyricShape(const std::string& s) {
    std::vector<uint32_t> cp = codePoints(s);
    auto outer = [](uint32_t c) { return lyricSpace(c) || c == '\r' || c == '\n'; };
    size_t a = 0, b = cp.size();
    while (a < b && outer(cp[a])) a++;
    while (b > a && outer(cp[b - 1])) b--;
    LyricShape sh;
    sh.blank = a == b;
    for (size_t i = a; i < b; i++) {
        if (lyricSpace(cp[i])) sh.innerSpace = true;
        sh.columns += wideChar(cp[i]) ? 2 : 1;
    }
    return sh;
}

bool startsWithSpace(const std::string& s) { return !s.empty() && (s[0] == ' ' || s[0] == '\t' || s.compare(0, 3, "\xE3\x80\x80") == 0); }
bool endsWithSpace(const std::string& s) {
    return !s.empty() && (s.back() == ' ' || s.back() == '\t' || (s.size() >= 3 && s.compare(s.size() - 3, 3, "\xE3\x80\x80") == 0));
}

// `s` without the spaces and line breaks around it.
std::string trimLyric(std::string s) {
    for (;;) {
        if (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        else if (s.size() >= 3 && s.compare(s.size() - 3, 3, "\xE3\x80\x80") == 0) s.erase(s.size() - 3);
        else break;
    }
    size_t a = 0;
    for (;;) {
        if (a < s.size() && (s[a] == '\r' || s[a] == '\n' || s[a] == ' ' || s[a] == '\t')) a++;
        else if (s.compare(a, 3, "\xE3\x80\x80") == 0) a += 3;
        else break;
    }
    return s.substr(a);
}

// The longest run of line breaks (CR, LF) in `s`.
int breakRun(const std::string& s) {
    int best = 0, run = 0;
    for (char c : s) {
        run = (c == '\r' || c == '\n') ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}

// The middle value (the lower of the two middle ones).
int64_t medianOf(std::vector<int64_t> v) {
    if (v.empty()) return 0;
    size_t mid = (v.size() - 1) / 2;
    std::nth_element(v.begin(), v.begin() + long(mid), v.end());
    return v[mid];
}

// Whether a line can end before syllable i: between words where the words have spaces between them,
// anywhere in Japanese and other text that has none.
bool wordBreak(const std::vector<LyricSyllable>& syl, size_t i, bool spaced) {
    return !spaced || endsWithSpace(syl[i - 1].text) || startsWithSpace(syl[i].text);
}

// Lines of syllables [a, b), each at most `maxColumns` wide where the words allow it: a longer line ends
// at its longest pause, preferring places that leave a quarter of it or more on each side.
void splitLongLine(const std::vector<LyricSyllable>& syl, size_t a, size_t b, int maxColumns, bool spaced, std::vector<LyricLine>& out) {
    std::vector<int> cols(b - a);
    int total = 0;
    for (size_t i = a; i < b; i++) total += cols[i - a] = lyricColumns(syl[i].text);
    size_t best = 0;
    if (total > maxColumns) {
        int64_t bestGap = -1;
        bool bestBalanced = false;
        int left = 0;
        for (size_t i = a + 1; i < b; i++) {
            left += cols[i - 1 - a];
            if (!wordBreak(syl, i, spaced)) continue;
            bool balanced = left * 4 >= total && (total - left) * 4 >= total;
            int64_t gap = syl[i].timeUs - syl[i - 1].timeUs;
            if ((balanced && !bestBalanced) || (balanced == bestBalanced && gap > bestGap)) {
                best = i;
                bestGap = gap;
                bestBalanced = balanced;
            }
        }
    }
    if (best) {
        splitLongLine(syl, a, best, maxColumns, spaced, out);
        splitLongLine(syl, best, b, maxColumns, spaced, out);
        return;
    }
    LyricLine l;
    l.syllables.assign(syl.begin() + long(a), syl.begin() + long(b));
    out.push_back(std::move(l));
}

} // namespace

int lyricColumns(const std::string& s) {
    int n = 0;
    for (uint32_t c : codePoints(s)) n += wideChar(c) ? 2 : 1;
    return n;
}

// The lines and verses of the lyrics. Songs mark them in several ways: KAR text events start a line with
// '/' and a verse with a backslash, XF lyrics use '/' and '<', and lyric events (RP-026) end lines with CR or LF
// (several: a verse). A song may also put a whole phrase in each lyric event, or mark nothing at all:
// then the events, the pauses of the singing and the song's section markers make the lines and verses.
void MidiFile::buildLyrics() {
    lyrics.clear();
    lyricsFromKar = false;
    size_t lyricCount = 0;
    for (const TextItem& t : texts)
        if (t.type == 5) lyricCount++;

    // Karaoke text events: find the track with the most '/' or '\' prefixed text events.
    std::vector<int> karScore(size_t(std::max(numTracks, 1)), 0);
    bool karHeader = false;
    for (const TextItem& t : texts) {
        if (t.type != 1 || t.raw.empty()) continue;
        if (t.raw.rfind("@KMIDI", 0) == 0 || t.raw.rfind("@K", 0) == 0) karHeader = true;
        if ((t.raw[0] == '/' || t.raw[0] == '\\') && t.track < karScore.size()) karScore[t.track]++;
    }
    int karTrack = -1, best = 0;
    for (size_t i = 0; i < karScore.size(); i++)
        if (karScore[i] > best) {
            best = karScore[i];
            karTrack = int(i);
        }
    bool useKar = karTrack >= 0 && (karHeader || best >= 4) && size_t(best) * 2 > lyricCount;

    std::vector<LyricSyllable> items;  // the lyric texts in order
    for (const TextItem& t : texts) {
        if (useKar) {
            if (t.type != 1 || int(t.track) != karTrack) continue;
            if (!t.text.empty() && t.text[0] == '@') continue;
        } else if (t.type != 5) {
            continue;
        }
        if (!t.text.empty()) items.push_back({t.timeUs, t.text});
    }
    bool lineMarks = false, verseMarks = false;
    for (const LyricSyllable& it : items) {
        const std::string& s = it.text;
        if (s[0] == '/' || s[0] == '\\' || s[0] == '<' || s[0] == '>' || s.find_first_of("\r\n") != std::string::npos) lineMarks = true;
        if (s[0] == '\\' || s[0] == '<' || breakRun(s) >= 3) verseMarks = true;
    }
    // Phrases: most events hold several words, or a long run of text, rather than a syllable or a word.
    int shown = 0, phraseLike = 0;
    for (const LyricSyllable& it : items) {
        LyricShape sh = lyricShape(it.text);
        if (sh.blank) continue;
        shown++;
        if (sh.innerSpace || sh.columns >= 12) phraseLike++;
    }
    const bool phrases = !lineMarks && phraseLike * 2 > shown;
    const bool spaced = std::any_of(items.begin(), items.end(), [](const LyricSyllable& it) { return it.text.find(' ') != std::string::npos; });

    if (lineMarks) {
        LyricLine line;
        bool pendingBreak = false, pendingPara = true;
        auto flush = [&](bool paragraph) {
            if (!line.syllables.empty()) {
                lyrics.push_back(std::move(line));
                line = LyricLine();
            }
            line.paragraphStart = paragraph;
        };
        line.paragraphStart = true;

        for (const LyricSyllable& t : items) {
            std::string s = t.text;
            if (pendingBreak) {
                flush(pendingPara);
                pendingBreak = false;
                pendingPara = false;
            }
            // Leading markers
            while (!s.empty() && (s[0] == '\\' || s[0] == '/' || s[0] == '<' || s[0] == '>')) {
                bool para = (s[0] == '\\' || s[0] == '<');
                flush(para);
                s.erase(0, 1);
            }
            // Trailing line breaks
            int breaks = 0;
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
                if (s.back() == '\n' || s.back() == '\r') breaks++;
                s.pop_back();
            }
            // Embedded breaks: split
            size_t nl;
            while ((nl = s.find_first_of("\r\n")) != std::string::npos) {
                std::string head = s.substr(0, nl);
                if (!head.empty()) line.syllables.push_back({t.timeUs, head});
                size_t skip = nl;
                int n = 0;
                while (skip < s.size() && (s[skip] == '\r' || s[skip] == '\n')) {
                    skip++;
                    n++;
                }
                flush(n >= 3);
                s.erase(0, skip);
            }
            if (!s.empty()) line.syllables.push_back({t.timeUs, s});
            if (breaks > 0) {
                pendingBreak = true;
                pendingPara = breaks >= 3;
            }
        }
        flush(false);
        // Lines the marks leave very long (songs that mark only some of them) end at their longest pause.
        std::vector<LyricLine> marked = std::move(lyrics);
        lyrics.clear();
        for (const LyricLine& l : marked) {
            size_t first = lyrics.size();
            splitLongLine(l.syllables, 0, l.syllables.size(), 80, spaced, lyrics);
            lyrics[first].paragraphStart = l.paragraphStart;
        }
    } else if (phrases) {
        // A line for each phrase; a blank event (a space) ends the line before it.
        for (const LyricSyllable& it : items) {
            if (lyricShape(it.text).blank) {
                if (!lyrics.empty() && !lyrics.back().endUs) lyrics.back().endUs = it.timeUs;
                continue;
            }
            LyricLine l;
            l.syllables.push_back({it.timeUs, trimLyric(it.text)});
            lyrics.push_back(std::move(l));
        }
    } else {
        // Syllables that mark no lines: a line ends where the singing pauses much longer than its usual
        // step, and a line still long at its longest pause.
        std::vector<int64_t> steps;
        for (size_t i = 1; i < items.size(); i++) steps.push_back(items[i].timeUs - items[i - 1].timeUs);
        const int64_t pause = std::max<int64_t>(medianOf(steps) * 5 / 2, 1000000);
        size_t from = 0;
        for (size_t i = 1; i <= items.size(); i++) {
            if (i < items.size() && !(items[i].timeUs - items[i - 1].timeUs >= pause && wordBreak(items, i, spaced))) continue;
            splitLongLine(items, from, i, 44, spaced, lyrics);
            from = i;
        }
    }
    if (!lyrics.empty()) lyrics.front().paragraphStart = true;
    if (!verseMarks) markVerses();
    lyricsFromKar = useKar;
}

// Verses where the lyrics do not mark them: the song's section markers (FF 06, such as "Verse", "Chorus",
// "A", "B") when the lyrics run across a few of them, else long pauses between lines.
void MidiFile::markVerses() {
    if (lyrics.size() < 2) return;
    // A section's first line may start before its marker, as a pickup does: up to a beat.
    std::vector<int64_t> sections;
    const int64_t firstUs = lyrics.front().startUs(), lastUs = lyrics.back().startUs();
    for (const TextItem& t : texts) {
        if (t.type != 6) continue;
        int64_t beat = smpte ? 500000 : t.timeUs - tickToUs(t.tick > uint64_t(ppqn) ? t.tick - uint64_t(ppqn) : 0);
        if (t.timeUs > firstUs && t.timeUs - beat <= lastUs) sections.push_back(t.timeUs - beat);
    }
    if (!sections.empty() && sections.size() * 2 <= lyrics.size()) {
        auto section = [&sections](int64_t us) { return std::count_if(sections.begin(), sections.end(), [us](int64_t s) { return s <= us; }); };
        for (size_t i = 1; i < lyrics.size(); i++)
            if (section(lyrics[i].startUs()) != section(lyrics[i - 1].startUs())) lyrics[i].paragraphStart = true;
        return;
    }
    // Pauses: from a line's last syllable to the next line, much longer than usual.
    std::vector<int64_t> rests;
    for (size_t i = 1; i < lyrics.size(); i++) {
        const LyricLine& prev = lyrics[i - 1];
        rests.push_back(lyrics[i].startUs() - (prev.syllables.empty() ? prev.startUs() : prev.syllables.back().timeUs));
    }
    const int64_t verse = std::max<int64_t>(medianOf(rests) * 5 / 2, 5000000);
    for (size_t i = 1; i < lyrics.size(); i++)
        if (rests[i - 1] >= verse) lyrics[i].paragraphStart = true;
}

std::string MidiFile::divisionDescription() const {
    char buf[96];
    if (smpte) {
        const char* fpsName = smpteFps == 29 ? "29.97 (drop)" : nullptr;
        if (fpsName) snprintf(buf, sizeof buf, "SMPTE %s fps x %d ticks/frame", fpsName, smpteTpf);
        else snprintf(buf, sizeof buf, "SMPTE %d fps x %d ticks/frame", smpteFps, smpteTpf);
    } else {
        snprintf(buf, sizeof buf, "PPQN (0x%04X)", rawDivision);
    }
    return buf;
}

std::string MidiFile::resolutionDescription() const {
    char buf[64];
    if (smpte) {
        double fps = smpteFps == 29 ? 29.97 : smpteFps;
        snprintf(buf, sizeof buf, "%.0f ticks/sec", fps * smpteTpf);
    } else {
        snprintf(buf, sizeof buf, "%d PPQN", ppqn);
    }
    return buf;
}

int64_t MidiFile::tickToUs(uint64_t tick) const {
    if (smpte) {
        double fps = smpteFps == 29 ? 29.97 : double(smpteFps ? smpteFps : 25);
        return int64_t(double(tick) * 1000000.0 / (fps * smpteTpf));
    }
    if (tempos.empty()) return int64_t(tick * 500000 / uint64_t(ppqn));
    auto it = std::upper_bound(tempos.begin(), tempos.end(), tick, [](uint64_t t, const TempoChange& c) { return t < c.tick; });
    const TempoChange& c = *(it == tempos.begin() ? it : it - 1);
    return c.timeUs + int64_t((tick - c.tick) * c.usPerQn / uint64_t(ppqn));
}

uint64_t MidiFile::usToTick(int64_t us) const {
    if (us <= 0) return 0;
    if (smpte) {
        double fps = smpteFps == 29 ? 29.97 : double(smpteFps ? smpteFps : 25);
        return uint64_t(double(us) * fps * smpteTpf / 1000000.0);
    }
    if (tempos.empty()) return uint64_t(us) * uint64_t(ppqn) / 500000;
    auto it = std::upper_bound(tempos.begin(), tempos.end(), us, [](int64_t t, const TempoChange& c) { return t < c.timeUs; });
    const TempoChange& c = *(it == tempos.begin() ? it : it - 1);
    return c.tick + uint64_t(us - c.timeUs) * uint64_t(ppqn) / c.usPerQn;
}

double MidiFile::bpmAtTick(uint64_t tick) const {
    if (smpte || tempos.empty()) return 120.0;
    auto it = std::upper_bound(tempos.begin(), tempos.end(), tick, [](uint64_t t, const TempoChange& c) { return t < c.tick; });
    const TempoChange& c = *(it == tempos.begin() ? it : it - 1);
    return 60000000.0 / c.usPerQn;
}

const TimeSignature& MidiFile::timeSigAt(uint64_t tick) const {
    const TimeSignature* ts = &defaultTimeSig_;
    for (const TimeSignature& t : timeSigs) {
        if (t.tick > tick) break;
        ts = &t;
    }
    return *ts;
}

const KeySignature* MidiFile::keySigAt(uint64_t tick) const {
    const KeySignature* ks = nullptr;
    for (const KeySignature& k : keySigs) {
        if (k.tick > tick) break;
        ks = &k;
    }
    return ks;
}

void MidiFile::tickToBarBeat(uint64_t tick, int& bar, int& beat, int& subTick) const {
    uint64_t segStart = 0;
    int barBase = 0;
    TimeSignature cur = defaultTimeSig_;
    cur.tick = 0;
    auto ticksPerBeat = [&](const TimeSignature& ts) { return std::max<uint64_t>(1, uint64_t(ppqn) * 4 >> ts.denPow); };
    for (const TimeSignature& ts : timeSigs) {
        if (ts.tick > tick) break;
        uint64_t tpb = ticksPerBeat(cur) * cur.num;
        barBase += int((ts.tick - segStart + tpb - 1) / tpb);
        segStart = ts.tick;
        cur = ts;
    }
    uint64_t tpBeat = ticksPerBeat(cur);
    uint64_t tpBar = tpBeat * cur.num;
    uint64_t rel = tick - segStart;
    bar = barBase + int(rel / tpBar) + 1;
    beat = int((rel % tpBar) / tpBeat) + 1;
    subTick = int(rel % tpBeat);
}

size_t MidiFile::eventIndexAtUs(int64_t us) const {
    auto it = std::lower_bound(events.begin(), events.end(), us, [](const MidiEvent& e, int64_t t) { return e.timeUs < t; });
    return size_t(it - events.begin());
}

} // namespace immidi

#pragma once
#include "TextCodec.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace immidi {

constexpr int kMaxPorts = 16;

// 16 bytes per event: black MIDI files carry tens of millions of them.
struct MidiEvent {
    int64_t timeUs = 0;    // absolute time at 100% speed
    uint32_t aux = 0;      // SysEx / raw events: payload offset in MidiFile::data
    uint8_t status = 0;    // 0x80-0xEF channel message, 0xF0 SysEx (payload = complete F0..F7), 0xF7 raw bytes
    uint8_t d1 = 0, d2 = 0; // channel data bytes
    uint8_t port = 0;
    bool isChannel() const { return status >= 0x80 && status < 0xF0; }
    uint8_t type() const { return status & 0xF0; }
    uint8_t channel() const { return status & 0x0F; }
};

struct TrackStat {
    uint32_t events = 0;
    uint32_t notes = 0;
    uint16_t channels = 0;
    int port = -1;
    bool excluded = false;  // EMIDI track for another sound card
};

struct TempoChange {
    uint64_t tick;
    int64_t timeUs;
    uint32_t usPerQn;
};

struct TimeSignature {
    uint64_t tick;
    uint8_t num = 4, denPow = 2, clocks = 24, n32 = 8;
};

struct KeySignature {
    uint64_t tick;
    int8_t sf = 0;
    uint8_t minor = 0;
    std::string name() const;
};

struct TextItem {
    uint64_t tick = 0;
    int64_t timeUs = 0;
    uint16_t track = 0;
    uint8_t type = 0;  // meta type 1..9
    std::string raw;   // raw bytes
    std::string text;  // decoded UTF-8
};

struct LyricSyllable {
    int64_t timeUs = 0;
    std::string text;
};

// The width of lyric text in columns: CJK, kana and other full-width characters count two.
int lyricColumns(const std::string& utf8);

struct LyricLine {
    std::vector<LyricSyllable> syllables;
    bool paragraphStart = false;  // the first line of a verse
    int64_t endUs = 0;            // when a blank lyric event ends the line (0: not known)
    int64_t startUs() const { return syllables.empty() ? 0 : syllables.front().timeUs; }
};

// One event of a Standard MIDI File as MidiFile reads it: SysEx split into packets is joined, a note-on
// with velocity 0 is a note-off, and the port follows the track's port prefix (FF 21) or device name
// (FF 09).
struct SmfEventRef {
    enum Kind : uint8_t { Channel, Meta, Sysex, Raw };
    Kind kind = Channel;
    int track = 0;              // index of the MTrk chunk
    uint64_t tick = 0;          // from the start of the track
    uint8_t port = 0;
    uint8_t status = 0, d1 = 0, d2 = 0;  // channel messages; meta events: d1 = type
    const uint8_t* data = nullptr;       // SysEx: F0 .. F7; meta events and escaped bytes (F7): their data
    uint32_t len = 0;
};

class MidiFile {
public:
    // Every event of an SMF in playback order (by tick, then track; format 2: one sequence after the
    // other), as load() reads them. `trackEnds` receives each track's End Of Track tick.
    static bool forEachSmfEvent(const uint8_t* smf, size_t size, const std::function<void(const SmfEventRef&)>& fn, int& format,
                                uint16_t& division, std::vector<uint64_t>& trackEnds, std::string& error);

    // `progress` (optional) receives 0..1 while parsing; loading may run on a worker thread.
    bool load(const std::string& path, std::string& error, std::atomic<float>* progress = nullptr);
    // Reads the song's details (texts, length, tracks, ports) the way load() does, but keeps no
    // events: quick and small even for black MIDI files. `events` stays empty.
    bool loadSummary(const std::string& path, std::string& error);
    bool loadFromMemory(const uint8_t* bytes, size_t size, std::string& error, std::atomic<float>* progress = nullptr);
    // Converts a non-SMF song (MIDI stream .mds, Recomposer .rcp, XMIDI .xmi) into a Standard MIDI File.
    // Fails for data that is already an SMF or in an unknown format.
    static bool convertToSmf(const uint8_t* bytes, size_t size, std::vector<uint8_t>& smf, std::string& error);

    // Re-decodes all text with a forced encoding (Auto = detected).
    void setEncoding(TextEncoding enc);

    std::string path;
    std::string fileName;
    size_t fileSize = 0;

    int format = 0;
    int numTracks = 0;
    uint16_t rawDivision = 0;
    bool smpte = false;
    int ppqn = 96;        // ticks per quarter note (PPQ files)
    int smpteFps = 0;     // 24, 25, 29 (=29.97 drop frame), 30
    int smpteTpf = 0;     // ticks per frame
    std::string divisionDescription() const;
    std::string resolutionDescription() const;

    std::vector<MidiEvent> events;
    std::vector<uint8_t> data;
    std::vector<TempoChange> tempos;
    std::vector<TimeSignature> timeSigs;
    std::vector<KeySignature> keySigs;
    std::vector<TextItem> texts;      // all text-like meta events (types 1-9)
    std::vector<std::string> trackNames;
    std::vector<TrackStat> trackStats;
    std::string containerFormat;  // "SMF", "RIFF MIDI (RMID)", "MIDI stream (MDS)"
    std::vector<std::string> portDeviceNames;  // FF 09 names, index = port
    std::vector<LyricLine> lyrics;
    bool lyricsFromKar = false;

    TextEncoding detectedEncoding = TextEncoding::Utf8;
    TextEncoding encoding = TextEncoding::Utf8;

    std::string title;
    std::string copyright;

    // Loop points (-1 = none). loopCount: 0 = infinite / unspecified.
    int64_t loopStartTick = -1, loopEndTick = -1;
    int64_t loopStartUs = -1, loopEndUs = -1;
    int loopCount = 0;
    bool emidi = false;
    int emidiExcludedTracks = 0;
    std::string loopType;  // "Marker", "EMIDI", "RPG Maker CC#111"
    bool hasLoop() const { return loopStartTick >= 0 && loopEndTick > loopStartTick; }

    uint64_t lengthTicks = 0;
    int64_t lengthUs = 0;
    int numPorts = 1;
    uint16_t channelMask[kMaxPorts] = {};  // channels carrying events, per port
    uint16_t noteChannelMask[kMaxPorts] = {};
    size_t noteCount = 0;
    size_t sysexCount = 0;

    int64_t tickToUs(uint64_t tick) const;
    uint64_t usToTick(int64_t us) const;
    double bpmAtTick(uint64_t tick) const;
    const TimeSignature& timeSigAt(uint64_t tick) const;
    const KeySignature* keySigAt(uint64_t tick) const;
    void tickToBarBeat(uint64_t tick, int& bar, int& beat, int& subTick) const;
    // First event index with timeUs >= us.
    size_t eventIndexAtUs(int64_t us) const;

    const uint8_t* payload(const MidiEvent& e) const { return data.data() + e.aux + 4; }
    uint32_t payloadLen(const MidiEvent& e) const {
        const uint8_t* p = data.data() + e.aux;
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    struct LoopScan {
        int64_t markerStart = -1, markerEnd = -1;
        int64_t b116 = -1, e117 = -1, b118 = -1, e119 = -1, cc111 = -1;
        int count116 = 0;
    };

private:
    bool parseSmf(const uint8_t* smf, size_t smfSize, std::string& error, std::atomic<float>* progress);
    bool summaryOnly_ = false;  // loadSummary(): parse without storing events
    void decodeTexts();
    void buildLyrics();
    void markVerses();
    void detectLoops();
    LoopScan loopScan_;
    TimeSignature defaultTimeSig_;
};

} // namespace immidi

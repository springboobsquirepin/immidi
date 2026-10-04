#pragma once
#include "Standards.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace immidi {

// ImMidi's instrument conversion tables (conversion/*.json). For a song written for one kind of
// sound module they give the closest voices and drum kits of another, with volume corrections,
// drum note changes and controller scaling:
//   gs-to-xg.json  GS sounds (SC-55, SC-88 and SC-88Pro maps, CM-64 banks) on XG modules
//   xg-to-gs.json  XG sounds on the SC-55, the SC-88, the SC-88Pro (SC-88Pro map) and the SC-8850 (SC-8850 map)
//   gs-maps.json   SC-88Pro and SC-88 sounds on modules without their map (SC-88, SC-55)
// Program numbers in the files are 1-128 ("pc"), as in the manuals; here they are MIDI values (0-127).

struct ConvNoteMap {
    int16_t note[128];     // target note; -1 = unchanged, -2 = not played (no counterpart, or no sound in the source kit)
    int8_t velocity[128];  // velocity correction
    ConvNoteMap();
};

// The bank select values and program to send, for a voice or a drum kit.
struct ConvTarget {
    uint8_t msb = 0, lsb = 0, program = 0;  // GS targets: msb = variation (CC#0), lsb = map (CC#32)
    int8_t volume = 0;                       // added to the part volume (CC#7)
    int8_t attack = 0;                       // voices: added to the part's attack time (64 = the sound's own)
    const ConvNoteMap* notes = nullptr;      // drum kits: note changes
};

struct ConvRule {
    bool set = false;
    int scale = 100, offset = 0;  // value * scale / 100 + offset, limited to 0-127
    int apply(int v) const;
};

struct ConvRules {
    ConvRule cc[128];
    std::map<uint16_t, ConvRule> nrpn;  // (msb << 7) | lsb
    ConvRule velocity;
};

enum class ConvDirection : uint8_t { None, GsToXg, XgToGs, GsMaps };

// What the tables do for one song on one module.
struct ConvSetup {
    ConvDirection direction = ConvDirection::None;
    int sourceMap = 0;      // GS songs: the map a part uses while CC#32 = 0 (1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8850)
    int targetMaxMap = 0;   // GS modules: the highest map they have (1 for the SC-55)
    bool targetSc55 = false;
    bool sc55MapUp = false; // SC-55 songs on later modules: SC-55 sounds use the SC-55 map
    std::string rulesKey;   // controller rules: "sc55", "sc88", "sc88pro", "sc88pro-sc55", ...
    std::string describe() const;
};

class ConversionTables {
public:
    static constexpr const char* kFiles[3] = {"gs-to-xg.json", "xg-to-gs.json", "gs-maps.json"};

    // The folder ImMidi, ImMidi Bridge and the Conversion Editor read the tables from: the user's own copy
    // (userFolder()) when it has them, else conversion/ next to the program (inside the app on macOS), else
    // conversion/ in the working directory. "" when none has them.
    static std::string defaultFolder();
    // <settings folder>/conversion: the tables of the user, which the Conversion Editor saves there when the
    // program's own are inside an app bundle (macOS), and which every program then reads instead.
    static std::string userFolder();

    // Loads the tables from a folder. False, with a message, when a file is missing or invalid.
    bool load(const std::string& dir, std::string& error);
    // Loads the tables from the files' text (in kFiles order), as load() checks them.
    bool loadTexts(const std::string (&texts)[3], std::string& error);
    const std::string& folder() const { return folder_; }

    // `forcedToneMap`: the map a GS module was set to play every part with (0 = none).
    static ConvSetup setupFor(MidiStandard source, DeviceKind sourceDevice, DeviceKind target, int forcedToneMap = 0);

    // `msb`/`lsb` are the song's bank select values (GS: variation, map; 0 = the song's own map).
    // Null when the tables have nothing; the emulation then converts generically.
    const ConvTarget* voice(const ConvSetup& s, int msb, int lsb, int program) const;
    const ConvTarget* drumKit(const ConvSetup& s, int msb, int lsb, int program) const;
    const ConvRules* rules(const ConvSetup& s) const;
    // Whether a sound exists on a GS map (the sources of gs-to-xg.json are every GS sound).
    bool gsVoiceExists(int map, int variation, int program) const;
    bool gsKitExists(int map, int program) const;

private:
    struct Pair {  // XG -> GS and GS map targets: for the SC-8850 (SC-8850 map), the SC-88Pro, the SC-88, the SC-55
        ConvTarget sc8850, sc88pro, sc88, sc55;
        bool hasSc8850 = false, hasSc88pro = false, hasSc88 = false, hasSc55 = false;
        const ConvTarget* pick(const ConvSetup& s) const;
    };
    static uint32_t key(int a, int b, int c) { return (uint32_t(a & 0xFF) << 16) | (uint32_t(b & 0xFF) << 8) | uint32_t(c & 0xFF); }

    const ConvTarget* gsToXgVoice(const ConvSetup& s, int bank, int map, int program) const;
    const ConvTarget* gsMapsVoice(const ConvSetup& s, int bank, int map, int program) const;

    std::string folder_;
    std::unordered_map<uint32_t, ConvTarget> gsXgVoices_, gsXgKits_;  // key(map, bank, pc) / key(map, 0, kit)
    std::unordered_map<uint32_t, Pair> xgGsVoices_, xgGsKits_;        // key(msb, lsb, pc) / key(msb, 0, kit)
    std::unordered_map<uint32_t, Pair> mapVoices_, mapKits_;          // key(map, bank, pc) / key(map, 0, kit)
    std::map<std::string, std::unique_ptr<ConvNoteMap>> noteMaps_;    // "<file>/<name>"
    std::map<std::string, ConvRules> rules_[3];                       // per file, by rules key
};

} // namespace immidi

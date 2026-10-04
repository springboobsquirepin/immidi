#pragma once
#include "MidiFile.h"
#include "ConversionTables.h"
#include "Standards.h"

#include <memory>

#include <vector>

namespace immidi {

// Converts MIDI messages written for one standard (GM/GS/XG/GM2) into messages for a
// target device, mapping equivalents where they exist ("XG on GS", "GS on XG", ...).
class Emulator {
public:
    // source = the standard the song was written for. When source and target belong to the
    // same family, only device-level differences are adapted (e.g. GS tone maps).
    // forcedToneMap (GS targets with tone maps): 0 = off, 1 = SC-55, 2 = SC-88, 3 = SC-88Pro, 4 = SC-8850.
    // adaptSameFamily: also adapt songs of the target's own family (GS tone maps of other SC models).
    // Without it and without a forced map, messages of the target's own standard pass unchanged.
    void configure(MidiStandard source, const DeviceProfile& target, int forcedToneMap = 0, bool adaptSameFamily = true);
    int forcedToneMap() const { return forcedMap_; }
    // Messages that make every part of a GS module use the forced map when CC#32 is 0.
    std::vector<Bytes> forcedMapMessages() const;
    // Optional instrument conversion tables, with what applies to the current song and module
    // (ConversionTables::setupFor). Null tables or a setup without a direction turn them off.
    void setConversion(std::shared_ptr<const ConversionTables> tables, const ConvSetup& setup);
    bool hasConversion() const { return tables_ != nullptr; }
    const ConvSetup& conversionSetup() const { return setup_; }
    // True when note messages must pass through mapNote() (drum note / velocity conversion).
    // Also true for XG songs on GS/GM targets, whose drum notes 13-34 are remapped as TMIDI Player does.
    bool mapsNotes() const { return active_ && (xgDrumPatch_ || tables_); }
    // Applies drum note remapping and velocity rules to a note on/off in place. Returns false when
    // the note has no counterpart on the target and must not be sent.
    bool mapNote(int port, uint8_t* msg) const;
    void reset();
    bool active() const { return active_; }
    MidiStandard source() const { return source_; }

    // Converts one message arriving on a file port. Results are appended to `out`.
    void convert(int port, const uint8_t* msg, size_t len, std::vector<Bytes>& out);

    static uint8_t mapDrumKit(MidiStandard from, const DeviceProfile& to, uint8_t program, bool sfxKit, bool* toSfxKit);

private:
    static constexpr uint16_t kNoSet = 0xFFFF;
    struct Chan {
        uint8_t msb = 0, lsb = 0;       // source bank select values
        bool srcDrum = false;           // drum part in source semantics
        uint8_t tgtMap = 0;             // GS targets: the drum map of the part (1 or 2; 0 = not a drum part)
        uint16_t tgtSet = kNoSet;       // GS targets: the drum set the part selected, (map << 8) | program
        uint8_t nrpnMsb = 127, nrpnLsb = 127;
        bool nrpnSel = false;
        uint8_t program = 0;
        int8_t level = 0;                    // conversion table volume correction for CC#7
        int8_t attackAdj = 0;                // conversion table attack time correction of the sound in use
        uint8_t attack = 64;                 // the part's attack time as the song set it (target values)
        const ConvNoteMap* kit = nullptr;    // conversion table drum notes of the kit in use
        uint8_t volume = 100;             // last volume from the song (before correction)
        bool haveVolume = false;
    };
    struct Port {
        Chan ch[16];
        uint16_t mapSet[3] = {kNoSet, kNoSet, kNoSet};  // GS targets: the drum set last selected on maps 1 and 2
        uint8_t xgRevMsb = 1, xgRevLsb = 0, xgChoMsb = 0x41, xgChoLsb = 0, xgVarMsb = 5, xgVarLsb = 0;
        uint8_t xgVarConnection = 0;
        uint8_t xgVarPart = 127;
        int gsEfxPart = -1;  // part currently assigned to EFX on a GS target
        uint8_t gsEfxMsb = 0, gsEfxLsb = 0;
        uint16_t gsEfxAssignMask = 0;
        uint8_t gsDelayMacro = 0;
        bool gsDelayUsed = false;
        void resetFor(MidiStandard s);
    };

    void channelMessage(int port, uint8_t st, uint8_t d1, uint8_t d2, std::vector<Bytes>& out);
    void programChange(int port, int ch, uint8_t prog, std::vector<Bytes>& out);
    void emitNrpn(int ch, uint8_t msb, uint8_t lsb, uint8_t value, std::vector<Bytes>& out);
    uint8_t attackOut(const Chan& c) const;
    void emitAttack(int ch, uint8_t value, std::vector<Bytes>& out);
    void sysex(int port, const uint8_t* m, size_t n, std::vector<Bytes>& out);
    void emitReset(std::vector<Bytes>& out);
    void setSourceDrum(int port, int ch, bool drum, uint8_t map, std::vector<Bytes>& out);
    void gsDrumMap(int port, int ch, bool drum, uint16_t set, std::vector<Bytes>& out);
    void gsDrumSetSelected(int port, int ch, uint16_t set, std::vector<Bytes>& out);
    void gsPartToTarget(int port, int ch, uint8_t a3, uint8_t v, std::vector<Bytes>& out);
    void gsSystemToTarget(int port, uint8_t a2, uint8_t a3, uint8_t v, std::vector<Bytes>& out);
    void xgToTarget(int port, uint8_t hi, uint8_t mid, uint8_t lo, uint8_t v, bool lastByte, std::vector<Bytes>& out);
    void xgEffectTypeToTarget(int port, int block, std::vector<Bytes>& out);  // 0 reverb, 1 chorus, 2 variation
    void assignGsEfxPart(int port, int ch, std::vector<Bytes>& out);
    void gm2GlobalToTarget(int port, bool reverb, uint8_t param, uint8_t value, std::vector<Bytes>& out);
    static Bytes cc(int ch, uint8_t c, uint8_t v) { return {uint8_t(0xB0 | ch), c, v}; }

    bool active_ = false;
    int forcedMap_ = 0;
    std::shared_ptr<const ConversionTables> tables_;
    ConvSetup setup_;
    const ConvRules* rules_ = nullptr;  // controller rules of the setup
    bool xgDrumPatch_ = false;  // XG source on a GS/GM target
    uint8_t volumeOut(const Chan& c) const;
    uint8_t toneMap(uint8_t fileMap) const;
    // GS targets: the CC#32 for a sound; SC-55 songs on later modules take SC-55 sounds from the SC-55 map.
    uint8_t gsTargetMap(uint8_t fileMap, bool drum, int variation, int program) const;
    MidiStandard source_ = MidiStandard::GM;
    DeviceProfile target_{};
    Port ports_[kMaxPorts];
};

} // namespace immidi

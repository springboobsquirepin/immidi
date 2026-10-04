#pragma once
#include "MidiFile.h"
#include "Standards.h"

#include <bitset>
#include <cstdint>
#include <cstring>

namespace immidi {

// Per-part sound parameters shared by GS/XG NRPNs, GS tone modify, XG multi part and
// GM2 sound controllers. Relative ones are centred on 64.
enum class SoundParam : uint8_t {
    VibRate = 0, VibDepth, VibDelay, Cutoff, Resonance, Attack, Decay, Release,
    HpfCutoff, EqBassGain, EqTrebleGain, EqBassFreq, EqTrebleFreq, Count
};
constexpr int kSoundParamCount = int(SoundParam::Count);
const char* soundParamName(SoundParam p);
const char* soundParamShortName(SoundParam p);
// NRPN (MSB 1, LSB n) for a parameter; returns -1 when there is none.
int soundParamNrpnLsb(SoundParam p);
// GM2 sound controller number for a parameter; -1 when there is none.
int soundParamGm2Cc(SoundParam p);
uint8_t soundParamDefault(SoundParam p);

struct ChannelState {
    uint8_t cc[128];
    uint8_t program = 0;
    uint8_t bankMsb = 0, bankLsb = 0;  // bank latched by the last program change
    uint16_t pitchBend = 8192;
    uint8_t pressure = 0;
    uint8_t bendRange = 2;
    int8_t coarseTune = 0;
    uint16_t fineTune = 8192;
    uint8_t modDepthRange = 0;
    uint8_t sound[kSoundParamCount];
    bool drum = false;
    uint8_t drumMap = 0;        // GS: 1/2, XG: part mode
    int8_t keyShift = 0;        // GS/XG part key shift
    uint8_t gsToneMap = 0;      // SC-88Pro "tone map number" (0 = selected by CC#32)
    uint8_t gsToneMap0 = 0;     // SC-88Pro "tone map-0 number": map used while CC#32 = 0 (0 = unit default)
    bool efxAssign = false;     // GS part EFX assign
    // Part pan set to random by GS / XG part SysEx (value 0). CC#10 cannot select it (its 0 is the
    // leftmost position) and ends it; cc[10] then holds 64.
    bool randomPan = false;
    bool gsEqSwitch = true;
    uint8_t xgDryLevel = 127;

    // RPN/NRPN selection
    uint8_t rpnMsb = 127, rpnLsb = 127, nrpnMsb = 127, nrpnLsb = 127;
    bool nrpnSelected = false;

    // Notes (for keyboards and meters)
    uint8_t noteVel[128];            // velocity of the keys held down (0 = up)
    uint8_t noteDepth[128];          // note-ons of a held key still waiting for their note-off (tracks sharing a channel can overlap a note)
    uint8_t noteSus[128];            // velocity of released notes the sustain (hold 1) or sostenuto pedal keeps sounding
    std::bitset<128> sostenutoKeys;  // keys that were down when the sostenuto pedal was pressed
    int activeNotes = 0;             // keys held down
    int sustainedNotes = 0;          // notes in noteSus
    uint32_t noteOnSerial = 0;
    uint8_t lastOnVelocity = 0;

    ChannelState() { clearAll(); }
    void clearAll();
    void resetControllers();  // CC#121 semantics
    void clearNotes() {       // all sound off: nothing sounds any more
        std::memset(noteVel, 0, sizeof noteVel);
        std::memset(noteDepth, 0, sizeof noteDepth);
        std::memset(noteSus, 0, sizeof noteSus);
        sostenutoKeys.reset();
        activeNotes = sustainedNotes = 0;
    }
    void noteOn(int note, uint8_t velocity);
    // Releases one note-on of the key (all of them with `all`); the note keeps sounding while a pedal holds it.
    void noteOff(int note, bool all = false);
    void allNotesOff();       // CC#123: every key released, the pedals still hold theirs
    void releasePedalNotes(); // after a pedal was released: ends the notes no pedal holds any more
};

struct PortState {
    ChannelState ch[16];
    MidiStandard mode = MidiStandard::None;
    uint16_t masterVolume = 16383;
    int masterKeyShift = 0;
    uint8_t masterPan = 64;
    int masterTune = 0;             // master fine tune in 0.1 cent steps (0 = A 440 Hz)
    uint16_t masterTuneRaw = 0x400; // GS/XG 4-nibble register (written one nibble at a time)

    // GS system effects (raw data as in the address map)
    uint8_t gsReverb[8];      // 40 01 30..37
    uint8_t gsChorus[9];      // 40 01 38..40
    uint8_t gsDelay[11];      // 40 01 50..5A
    uint8_t gsEq[4];          // 40 02 00..03
    uint8_t gsEfxMsb = 0, gsEfxLsb = 0;
    uint8_t gsEfxParam[20];   // 40 03 03..16
    uint8_t gsEfxSend[3];     // 40 03 17..19 (reverb, chorus, delay)
    uint8_t gsEfxCtrl[4];     // 40 03 1B..1E
    uint8_t gsEfxEqSwitch = 1;

    // XG effect memory
    uint8_t xgEffect[0x80];   // 02 01 00..7F
    uint8_t xgEq[0x15];       // 02 40 00..14
    uint8_t xgIns[2][0x30];   // 03 0n 00..2F

    // GM2 global parameters
    uint8_t gm2ReverbType = 4, gm2ReverbTime = 64;
    uint8_t gm2ChorusType = 2, gm2ChorusRate = 3, gm2ChorusDepth = 19, gm2ChorusFeedback = 8, gm2ChorusSendRev = 0;

    uint32_t sysexSerial = 0;  // bumps whenever system/effect state changes

    // Sound Canvas display (Roland model 45h): 16x16 dot pages 1-10 (10 0p 00, one row per entry,
    // bit 15 = leftmost dot), the page on screen (10 20 00; 0 = normal level display) and text
    // messages (10 00 00). The serials bump on every display message so the UI can time them out.
    uint16_t lcdPage[10][16];
    uint8_t lcdShownPage = 0;
    uint32_t lcdDotsSerial = 0;
    char lcdText[33];
    uint32_t lcdTextSerial = 0;

    PortState() { reset(MidiStandard::None); }
    void reset(MidiStandard m);
    void applyGsEfxDefaults();
};

class SynthState {
public:
    PortState ports[kMaxPorts];
    // Standard assumed for bank/drum interpretation while a port has seen no reset message.
    MidiStandard assumedStandard = MidiStandard::GM;

    void resetAll(MidiStandard m);
    void resetPort(int port, MidiStandard m);
    void clearNotes();
    // Applies a complete MIDI message (channel message or SysEx) arriving on a port.
    void apply(int port, const uint8_t* msg, size_t len);

    MidiStandard effectiveMode(int port) const {
        MidiStandard m = ports[port].mode;
        return m == MidiStandard::None ? assumedStandard : m;
    }

private:
    void applyChannel(int port, uint8_t status, uint8_t d1, uint8_t d2);
    void programChange(int port, int ch, uint8_t program);
    void dataEntry(int port, int ch);
    void applySysex(int port, const uint8_t* m, size_t n);
    void gsWrite(int port, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t v);
    void xgWrite(int port, uint8_t hi, uint8_t mid, uint8_t lo, uint8_t v);
};

} // namespace immidi

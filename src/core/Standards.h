#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace immidi {

using Bytes = std::vector<uint8_t>;

enum class MidiStandard : uint8_t { None = 0, GM, GM2, GS, XG };
const char* standardName(MidiStandard s);

// Target sound module the player drives. Decides which messages are native and
// which ones get converted by the emulation layer.
enum class DeviceKind : uint8_t { GM = 0, GM2, SC55, SC88, SC88Pro, SC8850, XG, Count };

struct DeviceProfile {
    DeviceKind kind;
    const char* name;
    MidiStandard family;
    int gsMap;               // GS tone map number used when CC#32 = 0 (1=SC-55, 2=SC-88, 3=SC-88Pro, 4=SC-8850)
    int gsMaxMap;            // highest CC#32 map the device understands (0 = no map select)
    bool hasDelay;           // GS delay (SC-88 and later)
    bool hasEfx;             // SC-88Pro style insertion effect
    bool hasGsEq;            // SC-88 2-band system EQ
    bool hasPartEq;          // XG part EQ (MU100 and later)
    bool nativeSoundCtrl;    // CC#71-78 (GM2 sound controllers) are native
    int ports;               // number of MIDI IN ports the real device has
    const char* insMelodic;  // instrument definition names; ';' separates fallbacks
    const char* insDrums;
};

const DeviceProfile& deviceProfile(DeviceKind k);
int deviceCount();
// Short names ("SC-8850") and ids for files and the command line ("sc8850"; "sc-8850" is read too).
const char* deviceShortName(DeviceKind k);
const char* deviceId(DeviceKind k);
bool deviceFromId(const std::string& id, DeviceKind& out);

// ---------------------------------------------------------------- effect tables
struct NamedType {
    uint8_t msb, lsb;
    const char* name;
};

extern const char* const kGsReverbMacros[8];
extern const char* const kGsChorusMacros[8];
extern const char* const kGsDelayMacros[10];
extern const char* const kGm2ReverbTypes[9];  // index = GM2 type value (5..7 unused)
extern const char* const kGm2ChorusTypes[6];

const std::vector<NamedType>& xgReverbTypes();
const std::vector<NamedType>& xgChorusTypes();
const std::vector<NamedType>& xgVariationTypes();
const std::vector<NamedType>& xgInsertionTypes();
std::string xgEffectName(uint8_t msb, uint8_t lsb);
extern const char* const kXgEqTypes[5];

// SC-88Pro insertion effect (EFX) definitions.
enum class GsEfxKind : uint8_t { LINEAR, TABLE, ENUM, PAN, BALANCE_DE, BALANCE_AB };
struct GsEfxParamDef {
    int index;         // 1..20 -> address 40 03 (02 + index)
    const char* name;
    int control;       // 0, 1 (+ Effect control 1), 2 (# Effect control 2)
    GsEfxKind kind;
    int lo, hi;        // valid data range
    float dlo, dhi;    // LINEAR display range
    int table;         // TABLE column in the effect data table
    const char* unit;
    const char* labels; // ENUM labels separated by '|'
    const char* values; // ENUM data values separated by ','
    int defaultValue;   // -1 when unknown
};
struct GsEfxTypeDef {
    uint8_t msb, lsb;
    const char* name;
    int firstParam, paramCount;
};
int gsEfxTypeCount();
const GsEfxTypeDef& gsEfxType(int i);
int gsEfxFindType(uint8_t msb, uint8_t lsb);  // -1 when unknown
const GsEfxParamDef& gsEfxParam(int i);
std::string gsEfxFormatValue(const GsEfxParamDef& p, int value);
std::vector<std::pair<int, std::string>> gsEfxEnumOptions(const GsEfxParamDef& p);

// ---------------------------------------------------------------- message builders
uint8_t rolandChecksum(const uint8_t* p, size_t n);
Bytes gsSysex(uint32_t address, std::initializer_list<uint8_t> data, uint8_t dev = 0x10);
Bytes gsSysex(uint32_t address, const uint8_t* data, size_t n, uint8_t dev = 0x10);
Bytes xgSysex(uint8_t hi, uint8_t mid, uint8_t lo, std::initializer_list<uint8_t> data);
Bytes xgSysex(uint8_t hi, uint8_t mid, uint8_t lo, const uint8_t* data, size_t n);

Bytes gmSystemOn();
Bytes gm2SystemOn();
Bytes gmSystemOff();
Bytes gsReset();
Bytes sc88ModeSet(bool doubleModule);
Bytes xgSystemOn();
Bytes masterVolumeSysex(uint16_t value14);
Bytes masterFineTuningSysex(int tenthsOfCent);  // universal realtime, +-100 cents
Bytes gm2GlobalReverb(uint8_t param, uint8_t value);  // param 0 = type, 1 = time
Bytes gm2GlobalChorus(uint8_t param, uint8_t value);  // 0 type, 1 rate, 2 depth, 3 feedback, 4 send to reverb

// GS part block number for a channel (0-15) and back.
inline uint8_t gsBlockOfChannel(int ch) { return ch == 9 ? 0 : (ch < 9 ? uint8_t(ch + 1) : uint8_t(ch)); }
inline int gsChannelOfBlock(int block) { return block == 0 ? 9 : (block <= 9 ? block - 1 : block); }

// Controller naming
const char* ccName(int cc);

} // namespace immidi

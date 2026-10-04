#include "Standards.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace immidi {

#include "GsEfxData.inc"

const char* standardName(MidiStandard s) {
    switch (s) {
    case MidiStandard::None: return "None";
    case MidiStandard::GM: return "GM";
    case MidiStandard::GM2: return "GM2";
    case MidiStandard::GS: return "GS";
    case MidiStandard::XG: return "XG";
    }
    return "?";
}

static const DeviceProfile kProfiles[] = {
    {DeviceKind::GM, "General MIDI (GM1)", MidiStandard::GM, 0, 0, false, false, false, false, false, 1,
     "General MIDI", "General MIDI Drums"},
    {DeviceKind::GM2, "General MIDI Level 2 (GM2)", MidiStandard::GM2, 0, 0, false, false, false, false, true, 1,
     "General MIDI Level 2", "General MIDI Level 2 Drums"},
    {DeviceKind::SC55, "Roland SC-55 (GS)", MidiStandard::GS, 1, 0, false, false, false, false, false, 1,
     "Roland SC-88 Pro", "Roland SC-88 Pro Drumsets"},
    {DeviceKind::SC88, "Roland SC-88 (GS)", MidiStandard::GS, 2, 2, true, false, true, false, false, 2,
     "Roland SC-88 Pro", "Roland SC-88 Pro Drumsets"},
    {DeviceKind::SC88Pro, "Roland SC-88Pro (GS)", MidiStandard::GS, 3, 3, true, true, true, false, false, 2,
     "Roland SC-88 Pro", "Roland SC-88 Pro Drumsets"},
    {DeviceKind::SC8850, "Roland SC-8850 (GS)", MidiStandard::GS, 4, 4, true, true, true, false, true, 4,
     "Roland SC-8850", "Roland SC-8850 (Drum Set)"},
    {DeviceKind::XG, "Yamaha XG (MU series)", MidiStandard::XG, 0, 0, false, false, false, true, false, 4,
     "Yamaha MU2000;Yamaha XG", "Yamaha MU2000 Drums;Yamaha XG Drums"},
};

namespace {
struct DeviceNames {
    const char* shortName;
    const char* id;
};
const DeviceNames kDeviceNames[] = {{"GM", "gm"},           {"GM2", "gm2"},         {"SC-55", "sc55"}, {"SC-88", "sc88"},
                                    {"SC-88Pro", "sc88pro"}, {"SC-8850", "sc8850"}, {"XG", "xg"}};
static_assert(sizeof kDeviceNames / sizeof kDeviceNames[0] == size_t(DeviceKind::Count), "a name for every device");
} // namespace

const char* deviceShortName(DeviceKind k) { return kDeviceNames[int(deviceProfile(k).kind)].shortName; }

const char* deviceId(DeviceKind k) { return kDeviceNames[int(deviceProfile(k).kind)].id; }

bool deviceFromId(const std::string& id, DeviceKind& out) {
    std::string s;
    for (char c : id)
        if (c != '-' && c != ' ') s += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (int d = 0; d < int(DeviceKind::Count); d++)
        if (s == kDeviceNames[d].id) {
            out = DeviceKind(d);
            return true;
        }
    return false;
}

const DeviceProfile& deviceProfile(DeviceKind k) {
    int i = int(k);
    if (i < 0 || i >= int(DeviceKind::Count)) i = int(DeviceKind::SC88Pro);
    return kProfiles[i];
}

int deviceCount() { return int(DeviceKind::Count); }

const char* const kGsReverbMacros[8] = {"Room 1", "Room 2", "Room 3", "Hall 1", "Hall 2", "Plate", "Delay", "Panning Delay"};
const char* const kGsChorusMacros[8] = {"Chorus 1", "Chorus 2", "Chorus 3", "Chorus 4", "Feedback Chorus", "Flanger", "Short Delay", "Short Delay (FB)"};
const char* const kGsDelayMacros[10] = {"Delay 1", "Delay 2", "Delay 3", "Delay 4", "Pan Delay 1",
                                        "Pan Delay 2", "Pan Delay 3", "Pan Delay 4", "Delay to Reverb", "Pan Repeat"};
const char* const kGm2ReverbTypes[9] = {"Small Room", "Medium Room", "Large Room", "Medium Hall", "Large Hall", nullptr, nullptr, nullptr, "Plate"};
const char* const kGm2ChorusTypes[6] = {"Chorus 1", "Chorus 2", "Chorus 3", "Chorus 4", "FB Chorus", "Flanger"};
const char* const kXgEqTypes[5] = {"Flat", "Jazz", "Pops", "Rock", "Classic"};

// Yamaha MU128 "Effect Type List" (reverb and chorus blocks) and "Effect LSB/MSB List".
const std::vector<NamedType>& xgReverbTypes() {
    static const std::vector<NamedType> v = {
        {0x00, 0x00, "No Effect"}, {0x01, 0x00, "Hall 1"}, {0x01, 0x01, "Hall 2"}, {0x02, 0x00, "Room 1"},
        {0x02, 0x01, "Room 2"},    {0x02, 0x02, "Room 3"}, {0x03, 0x00, "Stage 1"}, {0x03, 0x01, "Stage 2"},
        {0x04, 0x00, "Plate"},     {0x10, 0x00, "White Room"}, {0x11, 0x00, "Tunnel"}, {0x12, 0x00, "Canyon"},
        {0x13, 0x00, "Basement"},
    };
    return v;
}

const std::vector<NamedType>& xgChorusTypes() {
    static const std::vector<NamedType> v = {
        {0x00, 0x00, "No Effect"}, {0x41, 0x00, "Chorus 1"},  {0x41, 0x01, "Chorus 2"},  {0x41, 0x02, "Chorus 3"},
        {0x41, 0x08, "Chorus 4"},  {0x42, 0x00, "Celeste 1"}, {0x42, 0x01, "Celeste 2"}, {0x42, 0x02, "Celeste 3"},
        {0x42, 0x08, "Celeste 4"}, {0x43, 0x00, "Flanger 1"}, {0x43, 0x01, "Flanger 2"}, {0x43, 0x08, "Flanger 3"},
        {0x44, 0x00, "Symphonic"}, {0x48, 0x00, "Phaser 1"},  {0x48, 0x08, "Phaser 2"},  {0x57, 0x00, "Ensemble Detune"},
        {0x58, 0x00, "Ambience"},
    };
    return v;
}

const std::vector<NamedType>& xgVariationTypes() {
    static const std::vector<NamedType> v = {
        {0x00, 0x00, "No Effect"},       {0x01, 0x00, "Hall 1"},          {0x01, 0x01, "Hall 2"},
        {0x02, 0x00, "Room 1"},          {0x02, 0x01, "Room 2"},          {0x02, 0x02, "Room 3"},
        {0x03, 0x00, "Stage 1"},         {0x03, 0x01, "Stage 2"},         {0x04, 0x00, "Plate"},
        {0x05, 0x00, "Delay L,C,R"},     {0x06, 0x00, "Delay L,R"},       {0x07, 0x00, "Echo"},
        {0x08, 0x00, "Cross Delay"},     {0x09, 0x00, "ER 1"},            {0x09, 0x01, "ER 2"},
        {0x0A, 0x00, "Gate Reverb"},     {0x0B, 0x00, "Reverse Gate"},    {0x10, 0x00, "White Room"},
        {0x11, 0x00, "Tunnel"},          {0x12, 0x00, "Canyon"},          {0x13, 0x00, "Basement"},
        {0x14, 0x00, "Karaoke 1"},       {0x14, 0x01, "Karaoke 2"},       {0x14, 0x02, "Karaoke 3"},
        {0x40, 0x00, "Thru"},            {0x41, 0x00, "Chorus 1"},        {0x41, 0x01, "Chorus 2"},
        {0x41, 0x02, "Chorus 3"},        {0x41, 0x08, "Chorus 4"},        {0x42, 0x00, "Celeste 1"},
        {0x42, 0x01, "Celeste 2"},       {0x42, 0x02, "Celeste 3"},       {0x42, 0x08, "Celeste 4"},
        {0x43, 0x00, "Flanger 1"},       {0x43, 0x01, "Flanger 2"},       {0x43, 0x08, "Flanger 3"},
        {0x44, 0x00, "Symphonic"},       {0x45, 0x00, "Rotary Speaker"},  {0x46, 0x00, "Tremolo"},
        {0x47, 0x00, "Auto Pan"},        {0x48, 0x00, "Phaser 1"},        {0x48, 0x08, "Phaser 2"},
        {0x49, 0x00, "Distortion"},      {0x49, 0x01, "Comp+Distortion"}, {0x4A, 0x00, "Over Drive"},
        {0x4B, 0x00, "Amp Simulator"},   {0x4C, 0x00, "3-Band EQ"},       {0x4D, 0x00, "2-Band EQ"},
        {0x4E, 0x00, "Auto Wah (LFO)"},  {0x4E, 0x01, "Auto Wah+Dist"},   {0x4E, 0x02, "Auto Wah+Overdrive"},
        {0x50, 0x00, "Pitch Change 1"},  {0x50, 0x01, "Pitch Change 2"},  {0x51, 0x00, "Harmonic Enhancer"},
        {0x52, 0x00, "Touch Wah 1"},     {0x52, 0x01, "Touch Wah+Dist"},  {0x52, 0x02, "Touch Wah+Overdrive"},
        {0x52, 0x08, "Touch Wah 2"},     {0x53, 0x00, "Compressor"},      {0x54, 0x00, "Noise Gate"},
        {0x55, 0x00, "Voice Cancel"},    {0x56, 0x00, "2-Way Rotary Speaker"}, {0x57, 0x00, "Ensemble Detune"},
        {0x58, 0x00, "Ambience"},        {0x5D, 0x00, "Talking Modulator"}, {0x5E, 0x00, "Lo-Fi"},
        {0x5F, 0x00, "Dist+Delay"},      {0x5F, 0x01, "Overdrive+Delay"}, {0x60, 0x00, "Comp+Dist+Delay"},
        {0x60, 0x01, "Comp+Overdrive+Delay"}, {0x61, 0x00, "Wah+Dist+Delay"}, {0x61, 0x01, "Wah+Overdrive+Delay"},
    };
    return v;
}

const std::vector<NamedType>& xgInsertionTypes() {
    static const std::vector<NamedType> v = {
        {0x00, 0x00, "Thru"},           {0x01, 0x00, "Hall 1"},         {0x01, 0x01, "Hall 2"},
        {0x02, 0x00, "Room 1"},         {0x02, 0x01, "Room 2"},         {0x02, 0x02, "Room 3"},
        {0x03, 0x00, "Stage 1"},        {0x03, 0x01, "Stage 2"},        {0x04, 0x00, "Plate"},
        {0x05, 0x00, "Delay L,C,R"},    {0x06, 0x00, "Delay L,R"},      {0x07, 0x00, "Echo"},
        {0x08, 0x00, "Cross Delay"},    {0x14, 0x00, "Karaoke 1"},      {0x14, 0x01, "Karaoke 2"},
        {0x14, 0x02, "Karaoke 3"},      {0x41, 0x00, "Chorus 1"},       {0x41, 0x01, "Chorus 2"},
        {0x41, 0x02, "Chorus 3"},       {0x41, 0x08, "Chorus 4"},       {0x42, 0x00, "Celeste 1"},
        {0x42, 0x01, "Celeste 2"},      {0x42, 0x02, "Celeste 3"},      {0x42, 0x08, "Celeste 4"},
        {0x43, 0x00, "Flanger 1"},      {0x43, 0x01, "Flanger 2"},      {0x43, 0x08, "Flanger 3"},
        {0x44, 0x00, "Symphonic"},      {0x45, 0x00, "Rotary Speaker"}, {0x46, 0x00, "Tremolo"},
        {0x47, 0x00, "Auto Pan"},       {0x48, 0x00, "Phaser 1"},       {0x49, 0x00, "Distortion"},
        {0x4A, 0x00, "Over Drive"},     {0x4B, 0x00, "Amp Simulator"},  {0x4C, 0x00, "3-Band EQ"},
        {0x4D, 0x00, "2-Band EQ"},      {0x4E, 0x00, "Auto Wah (LFO)"}, {0x51, 0x00, "Harmonic Enhancer"},
        {0x52, 0x00, "Touch Wah 1"},    {0x52, 0x08, "Touch Wah 2"},    {0x53, 0x00, "Compressor"},
        {0x54, 0x00, "Noise Gate"},     {0x57, 0x00, "Ensemble Detune"},
    };
    return v;
}

std::string xgEffectName(uint8_t msb, uint8_t lsb) {
    for (const auto* list : {&xgReverbTypes(), &xgChorusTypes(), &xgVariationTypes()})
        for (const NamedType& t : *list)
            if (t.msb == msb && t.lsb == lsb) return t.name;
    // LSB variations that are not listed behave like the basic type (LSB 0).
    for (const NamedType& t : xgVariationTypes())
        if (t.msb == msb && t.lsb == 0) return std::string(t.name) + "*";
    if (msb == 0) return "No Effect";
    if (msb >= 0x40) return "Thru";
    char buf[32];
    snprintf(buf, sizeof buf, "%02X/%02X", msb, lsb);
    return buf;
}

// ---------------------------------------------------------------- SC-88Pro EFX

int gsEfxTypeCount() { return int(sizeof kGsEfxTypes / sizeof kGsEfxTypes[0]); }
const GsEfxTypeDef& gsEfxType(int i) { return kGsEfxTypes[i]; }
const GsEfxParamDef& gsEfxParam(int i) { return kGsEfxParams[i]; }

int gsEfxFindType(uint8_t msb, uint8_t lsb) {
    for (int i = 0; i < gsEfxTypeCount(); i++)
        if (kGsEfxTypes[i].msb == msb && kGsEfxTypes[i].lsb == lsb) return i;
    return -1;
}

static std::vector<std::string> splitStr(const char* s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s; *p; p++) {
        if (*p == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty() || !out.empty()) out.push_back(cur);
    return out;
}

std::vector<std::pair<int, std::string>> gsEfxEnumOptions(const GsEfxParamDef& p) {
    std::vector<std::pair<int, std::string>> out;
    auto labels = splitStr(p.labels, '|');
    auto values = splitStr(p.values, ',');
    for (size_t i = 0; i < labels.size() && i < values.size(); i++) out.push_back({atoi(values[i].c_str()), labels[i]});
    return out;
}

std::string gsEfxFormatValue(const GsEfxParamDef& p, int v) {
    char buf[48];
    switch (p.kind) {
    case GsEfxKind::TABLE:
        if (p.table >= 0 && p.table < 14 && v >= 0 && v < 128) return kGsEfxDataTable[v][p.table];
        break;
    case GsEfxKind::ENUM: {
        auto opts = gsEfxEnumOptions(p);
        for (auto& o : opts)
            if (o.first == v) return o.second;
        // Rotary speed style (00/7F): values between map to the nearest option.
        if (opts.size() == 2 && opts[1].first == 127) return v >= 64 ? opts[1].second : opts[0].second;
        break;
    }
    case GsEfxKind::PAN:
        if (v == 64) return "0";
        snprintf(buf, sizeof buf, "%s%d", v < 64 ? "L" : "R", std::abs(v - 64));
        return buf;
    case GsEfxKind::BALANCE_DE:
    case GsEfxKind::BALANCE_AB: {
        char a = p.kind == GsEfxKind::BALANCE_DE ? 'D' : 'A';
        char b = p.kind == GsEfxKind::BALANCE_DE ? 'E' : 'B';
        if (v == 64) {
            snprintf(buf, sizeof buf, "%c=%c", a, b);
        } else if (v < 64) {
            snprintf(buf, sizeof buf, "%c>%d%c", a, v * 2, b);
        } else {
            snprintf(buf, sizeof buf, "%c%d<%c", a, (127 - v) * 2, b);
        }
        return buf;
    }
    case GsEfxKind::LINEAR: {
        if (p.hi == p.lo) break;
        int c = v < p.lo ? p.lo : (v > p.hi ? p.hi : v);
        float d = p.dlo + (p.dhi - p.dlo) * float(c - p.lo) / float(p.hi - p.lo);
        bool integral = std::fabs(p.dlo - std::round(p.dlo)) < 1e-4f && std::fabs(p.dhi - std::round(p.dhi)) < 1e-4f;
        if (integral) snprintf(buf, sizeof buf, "%s%d%s", (p.dlo < 0 && d > 0.5f) ? "+" : "", int(std::lround(d)), p.unit);
        else snprintf(buf, sizeof buf, "%.2f%s", d, p.unit);
        return buf;
    }
    }
    snprintf(buf, sizeof buf, "%d", v);
    return buf;
}

// ---------------------------------------------------------------- builders

uint8_t rolandChecksum(const uint8_t* p, size_t n) {
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) sum += p[i];
    return uint8_t((128 - (sum & 0x7F)) & 0x7F);
}

Bytes gsSysex(uint32_t address, const uint8_t* data, size_t n, uint8_t dev) {
    Bytes b = {0xF0, 0x41, dev, 0x42, 0x12, uint8_t((address >> 16) & 0x7F), uint8_t((address >> 8) & 0x7F), uint8_t(address & 0x7F)};
    b.insert(b.end(), data, data + n);
    b.push_back(rolandChecksum(b.data() + 5, b.size() - 5));
    b.push_back(0xF7);
    return b;
}

Bytes gsSysex(uint32_t address, std::initializer_list<uint8_t> data, uint8_t dev) {
    return gsSysex(address, data.begin(), data.size(), dev);
}

Bytes xgSysex(uint8_t hi, uint8_t mid, uint8_t lo, const uint8_t* data, size_t n) {
    Bytes b = {0xF0, 0x43, 0x10, 0x4C, hi, mid, lo};
    b.insert(b.end(), data, data + n);
    b.push_back(0xF7);
    return b;
}

Bytes xgSysex(uint8_t hi, uint8_t mid, uint8_t lo, std::initializer_list<uint8_t> data) {
    return xgSysex(hi, mid, lo, data.begin(), data.size());
}

Bytes gmSystemOn() { return {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}; }
Bytes gm2SystemOn() { return {0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7}; }
Bytes gmSystemOff() { return {0xF0, 0x7E, 0x7F, 0x09, 0x02, 0xF7}; }
Bytes gsReset() { return gsSysex(0x40007F, {0x00}); }
Bytes sc88ModeSet(bool doubleModule) { return gsSysex(0x00007F, {uint8_t(doubleModule ? 1 : 0)}); }
Bytes xgSystemOn() { return xgSysex(0x00, 0x00, 0x7E, {0x00}); }

Bytes masterFineTuningSysex(int tenths) {
    int v = 0x2000 + tenths * 0x2000 / 1000;
    v = v < 0 ? 0 : (v > 0x3FFF ? 0x3FFF : v);
    return {0xF0, 0x7F, 0x7F, 0x04, 0x03, uint8_t(v & 0x7F), uint8_t(v >> 7), 0xF7};
}

Bytes masterVolumeSysex(uint16_t v) {
    return {0xF0, 0x7F, 0x7F, 0x04, 0x01, uint8_t(v & 0x7F), uint8_t((v >> 7) & 0x7F), 0xF7};
}

Bytes gm2GlobalReverb(uint8_t param, uint8_t value) {
    return {0xF0, 0x7F, 0x7F, 0x04, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, param, value, 0xF7};
}

Bytes gm2GlobalChorus(uint8_t param, uint8_t value) {
    return {0xF0, 0x7F, 0x7F, 0x04, 0x05, 0x01, 0x01, 0x01, 0x01, 0x02, param, value, 0xF7};
}

const char* ccName(int cc) {
    switch (cc) {
    case 0: return "Bank Select MSB";
    case 1: return "Modulation";
    case 2: return "Breath";
    case 4: return "Foot";
    case 5: return "Portamento Time";
    case 6: return "Data Entry MSB";
    case 7: return "Volume";
    case 8: return "Balance";
    case 10: return "Pan";
    case 11: return "Expression";
    case 32: return "Bank Select LSB";
    case 38: return "Data Entry LSB";
    case 64: return "Sustain";
    case 65: return "Portamento";
    case 66: return "Sostenuto";
    case 67: return "Soft";
    case 71: return "Resonance";
    case 72: return "Release";
    case 73: return "Attack";
    case 74: return "Cutoff";
    case 75: return "Decay";
    case 76: return "Vibrato Rate";
    case 77: return "Vibrato Depth";
    case 78: return "Vibrato Delay";
    case 84: return "Portamento Control";
    case 91: return "Reverb";
    case 93: return "Chorus";
    case 94: return "Delay/Variation";
    case 98: return "NRPN LSB";
    case 99: return "NRPN MSB";
    case 100: return "RPN LSB";
    case 101: return "RPN MSB";
    case 120: return "All Sound Off";
    case 121: return "Reset All Controllers";
    case 123: return "All Notes Off";
    case 126: return "Mono";
    case 127: return "Poly";
    }
    return nullptr;
}

} // namespace immidi

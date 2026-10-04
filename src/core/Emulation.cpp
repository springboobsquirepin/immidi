#include "Emulation.h"
#include "SynthState.h"

#include <algorithm>
#include <cstring>

namespace immidi {

namespace {

struct TypeMap {
    uint8_t fromMsb, fromLsb;  // fromLsb 0xFF = any
    uint8_t toMsb, toLsb;
};

// XG variation/insertion type -> SC-88Pro EFX type
const TypeMap kXgToEfx[] = {
    {0x01, 0xFF, 0x01, 0x55}, {0x02, 0xFF, 0x01, 0x55}, {0x03, 0xFF, 0x01, 0x55}, {0x04, 0xFF, 0x01, 0x55},
    {0x05, 0xFF, 0x01, 0x52}, {0x06, 0xFF, 0x01, 0x50}, {0x07, 0xFF, 0x01, 0x50}, {0x08, 0xFF, 0x01, 0x50},
    {0x09, 0xFF, 0x01, 0x55}, {0x0A, 0xFF, 0x01, 0x56}, {0x0B, 0xFF, 0x01, 0x56}, {0x10, 0xFF, 0x01, 0x55},
    {0x11, 0xFF, 0x01, 0x55}, {0x12, 0xFF, 0x01, 0x55}, {0x13, 0xFF, 0x01, 0x55}, {0x14, 0xFF, 0x01, 0x50},
    {0x40, 0xFF, 0x00, 0x00}, {0x41, 0xFF, 0x01, 0x42}, {0x42, 0xFF, 0x01, 0x40}, {0x43, 0x08, 0x01, 0x24},
    {0x43, 0xFF, 0x01, 0x23}, {0x44, 0xFF, 0x01, 0x40}, {0x45, 0xFF, 0x01, 0x22}, {0x46, 0xFF, 0x01, 0x25},
    {0x47, 0xFF, 0x01, 0x26}, {0x48, 0xFF, 0x01, 0x20}, {0x49, 0xFF, 0x01, 0x11}, {0x4A, 0xFF, 0x01, 0x10},
    {0x4B, 0xFF, 0x01, 0x10}, {0x4C, 0xFF, 0x01, 0x00}, {0x4D, 0xFF, 0x01, 0x00}, {0x4E, 0xFF, 0x01, 0x21},
    {0x50, 0x01, 0x01, 0x61}, {0x50, 0xFF, 0x01, 0x60}, {0x51, 0xFF, 0x01, 0x02}, {0x52, 0xFF, 0x01, 0x21},
    {0x53, 0xFF, 0x01, 0x30}, {0x54, 0xFF, 0x01, 0x31}, {0x55, 0xFF, 0x00, 0x00}, {0x56, 0xFF, 0x01, 0x22},
    {0x57, 0xFF, 0x01, 0x43}, {0x58, 0xFF, 0x01, 0x55}, {0x5D, 0xFF, 0x01, 0x03}, {0x5E, 0xFF, 0x01, 0x72},
    {0x5F, 0x01, 0x02, 0x02}, {0x5F, 0xFF, 0x02, 0x05}, {0x60, 0x01, 0x02, 0x02}, {0x60, 0xFF, 0x02, 0x05},
    {0x61, 0xFF, 0x04, 0x02},
};

// SC-88Pro EFX type -> XG variation type
const TypeMap kEfxToXg[] = {
    {0x00, 0x00, 0x40, 0x00}, {0x01, 0x00, 0x4C, 0x00}, {0x01, 0x01, 0x4C, 0x00}, {0x01, 0x02, 0x51, 0x00},
    {0x01, 0x03, 0x5D, 0x00}, {0x01, 0x10, 0x4A, 0x00}, {0x01, 0x11, 0x49, 0x00}, {0x01, 0x20, 0x48, 0x00},
    {0x01, 0x21, 0x4E, 0x00}, {0x01, 0x22, 0x45, 0x00}, {0x01, 0x23, 0x43, 0x00}, {0x01, 0x24, 0x43, 0x08},
    {0x01, 0x25, 0x46, 0x00}, {0x01, 0x26, 0x47, 0x00}, {0x01, 0x30, 0x53, 0x00}, {0x01, 0x31, 0x53, 0x00},
    {0x01, 0x40, 0x44, 0x00}, {0x01, 0x41, 0x41, 0x00}, {0x01, 0x42, 0x41, 0x00}, {0x01, 0x43, 0x57, 0x00},
    {0x01, 0x44, 0x41, 0x01}, {0x01, 0x50, 0x06, 0x00}, {0x01, 0x51, 0x06, 0x00}, {0x01, 0x52, 0x05, 0x00},
    {0x01, 0x53, 0x05, 0x00}, {0x01, 0x54, 0x07, 0x00}, {0x01, 0x55, 0x01, 0x00}, {0x01, 0x56, 0x0A, 0x00},
    {0x01, 0x57, 0x05, 0x00}, {0x01, 0x60, 0x50, 0x00}, {0x01, 0x61, 0x50, 0x01}, {0x01, 0x70, 0x47, 0x00},
    {0x01, 0x71, 0x47, 0x00}, {0x01, 0x72, 0x5E, 0x00}, {0x01, 0x73, 0x5E, 0x00}, {0x02, 0x00, 0x4A, 0x00},
    {0x02, 0x01, 0x4A, 0x00}, {0x02, 0x02, 0x5F, 0x01}, {0x02, 0x03, 0x49, 0x00}, {0x02, 0x04, 0x49, 0x00},
    {0x02, 0x05, 0x5F, 0x00}, {0x02, 0x06, 0x51, 0x00}, {0x02, 0x07, 0x51, 0x00}, {0x02, 0x08, 0x51, 0x00},
    {0x02, 0x09, 0x41, 0x00}, {0x02, 0x0A, 0x43, 0x00}, {0x02, 0x0B, 0x41, 0x00}, {0x03, 0x00, 0x45, 0x00},
    {0x04, 0x00, 0x49, 0x01}, {0x04, 0x01, 0x49, 0x00}, {0x04, 0x02, 0x61, 0x00}, {0x04, 0x03, 0x53, 0x00},
    {0x04, 0x04, 0x52, 0x00}, {0x04, 0x05, 0x49, 0x01}, {0x04, 0x06, 0x48, 0x00}, {0x05, 0x00, 0x48, 0x00},
    {0x11, 0x00, 0x41, 0x00}, {0x11, 0x01, 0x43, 0x00}, {0x11, 0x02, 0x41, 0x00}, {0x11, 0x03, 0x4A, 0x00},
    {0x11, 0x04, 0x45, 0x00}, {0x11, 0x05, 0x48, 0x00}, {0x11, 0x06, 0x4E, 0x02}, {0x11, 0x07, 0x45, 0x00},
    {0x11, 0x08, 0x4E, 0x00},
};

bool lookupType(const TypeMap* t, size_t n, uint8_t msb, uint8_t lsb, uint8_t& om, uint8_t& ol) {
    for (size_t i = 0; i < n; i++)
        if (t[i].fromMsb == msb && t[i].fromLsb == lsb) {
            om = t[i].toMsb;
            ol = t[i].toLsb;
            return true;
        }
    for (size_t i = 0; i < n; i++)
        if (t[i].fromMsb == msb && t[i].fromLsb == 0xFF) {
            om = t[i].toMsb;
            ol = t[i].toLsb;
            return true;
        }
    return false;
}

int xgReverbToGsMacro(uint8_t msb, uint8_t lsb) {
    switch (msb) {
    case 0x01: return lsb == 0 ? 3 : 4;
    case 0x02: return std::min<int>(lsb, 2);
    case 0x03: return lsb == 0 ? 3 : 4;
    case 0x04: return 5;
    case 0x10: return 1;
    case 0x11:
    case 0x12: return 4;
    case 0x13: return 2;
    default: return -1;
    }
}

int xgReverbToGm2(uint8_t msb, uint8_t lsb) {
    switch (msb) {
    case 0x01: return lsb == 0 ? 3 : 4;
    case 0x02: return std::min<int>(lsb, 2);
    case 0x03: return lsb == 0 ? 3 : 4;
    case 0x04: return 8;
    case 0x10: return 1;
    case 0x13: return 2;
    case 0x11:
    case 0x12: return 4;
    default: return -1;
    }
}

void gsMacroToXgReverb(int m, uint8_t& msb, uint8_t& lsb) {
    static const uint8_t t[8][2] = {{2, 0}, {2, 1}, {2, 2}, {1, 0}, {1, 1}, {4, 0}, {2, 0}, {2, 0}};
    m = std::clamp(m, 0, 7);
    msb = t[m][0];
    lsb = t[m][1];
}

int gm2ReverbToGsMacro(int t) {
    switch (t) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 4;
    case 8: return 5;
    default: return 4;
    }
}

int gsMacroToGm2Reverb(int m) {
    static const int t[8] = {0, 1, 2, 3, 4, 8, 1, 1};
    return t[std::clamp(m, 0, 7)];
}

int xgChorusToGsMacro(uint8_t msb, uint8_t lsb) {
    switch (msb) {
    case 0x41: return lsb == 0 ? 0 : lsb == 1 ? 1 : lsb == 2 ? 2 : 3;
    case 0x42: return 2;
    case 0x43: return 5;
    case 0x44: return 3;
    case 0x48: return 5;
    case 0x57: return 1;
    case 0x58: return 2;
    default: return -1;
    }
}

int xgChorusToGm2(uint8_t msb, uint8_t lsb) {
    switch (msb) {
    case 0x41: return lsb == 0 ? 0 : lsb == 1 ? 1 : lsb == 2 ? 2 : 3;
    case 0x42: return 4;
    case 0x43: return 5;
    case 0x44: return 3;
    case 0x48: return 5;
    default: return -1;
    }
}

void gsMacroToXgChorus(int m, uint8_t& msb, uint8_t& lsb) {
    static const uint8_t t[8][2] = {{0x41, 0}, {0x41, 1}, {0x41, 2}, {0x41, 8}, {0x42, 0}, {0x43, 0}, {0x42, 1}, {0x43, 1}};
    m = std::clamp(m, 0, 7);
    msb = t[m][0];
    lsb = t[m][1];
}

void gm2ChorusToXg(int m, uint8_t& msb, uint8_t& lsb) {
    static const uint8_t t[6][2] = {{0x41, 0}, {0x41, 1}, {0x41, 2}, {0x41, 8}, {0x42, 0}, {0x43, 0}};
    m = std::clamp(m, 0, 5);
    msb = t[m][0];
    lsb = t[m][1];
}

int xgVariationDelayToGsMacro(uint8_t msb) {
    switch (msb) {
    case 0x05: return 0;
    case 0x06: return 4;
    case 0x07: return 2;
    case 0x08: return 5;
    case 0x14: return 1;
    default: return -1;
    }
}

// XG SFX voice (bank MSB 64) -> closest GM sound effect program.
uint8_t xgSfxVoiceToGm(uint8_t prog) {
    if (prog < 16) return 120;
    if (prog < 32) return 121;
    if (prog < 48) return 122;
    if (prog < 64) return 123;
    if (prog < 80) return 124;
    if (prog < 96) return 125;
    if (prog < 112) return 126;
    return 127;
}

SoundParam gsToneModifyParam(uint8_t a3) {
    static const SoundParam t[8] = {SoundParam::VibRate, SoundParam::VibDepth, SoundParam::Cutoff, SoundParam::Resonance,
                                    SoundParam::Attack, SoundParam::Decay, SoundParam::Release, SoundParam::VibDelay};
    return t[a3 - 0x30];
}

SoundParam xgPartSoundParam(uint8_t lo) {
    static const SoundParam t[8] = {SoundParam::VibRate, SoundParam::VibDepth, SoundParam::VibDelay, SoundParam::Cutoff,
                                    SoundParam::Resonance, SoundParam::Attack, SoundParam::Decay, SoundParam::Release};
    return t[lo - 0x15];
}

uint8_t xgPartAddressOf(SoundParam p) {
    switch (p) {
    case SoundParam::VibRate: return 0x15;
    case SoundParam::VibDepth: return 0x16;
    case SoundParam::VibDelay: return 0x17;
    case SoundParam::Cutoff: return 0x18;
    case SoundParam::Resonance: return 0x19;
    case SoundParam::Attack: return 0x1A;
    case SoundParam::Decay: return 0x1B;
    case SoundParam::Release: return 0x1C;
    default: return 0;
    }
}

} // namespace

void Emulator::Port::resetFor(MidiStandard s) {
    for (int c = 0; c < 16; c++) {
        ch[c] = Chan();
        ch[c].srcDrum = (c == 9);
        ch[c].tgtMap = (c == 9) ? 1 : 0;
        if (s == MidiStandard::XG && c == 9) ch[c].msb = 127;
        if (s == MidiStandard::GM2) ch[c].msb = (c == 9) ? 120 : 121;
    }
    xgRevMsb = 1;
    xgRevLsb = 0;
    xgChoMsb = 0x41;
    xgChoLsb = 0;
    xgVarMsb = 5;
    xgVarLsb = 0;
    xgVarConnection = 0;
    xgVarPart = 127;
    mapSet[0] = mapSet[1] = mapSet[2] = kNoSet;
    gsEfxPart = -1;
    gsEfxMsb = gsEfxLsb = 0;
    gsEfxAssignMask = 0;
    gsDelayMacro = 0;
    gsDelayUsed = false;
}

void Emulator::configure(MidiStandard source, const DeviceProfile& target, int forcedToneMap, bool adaptSameFamily) {
    source_ = source == MidiStandard::None ? MidiStandard::GM : source;
    target_ = target;
    forcedMap_ = (target.family == MidiStandard::GS && forcedToneMap > 0 && forcedToneMap <= target.gsMaxMap) ? forcedToneMap : 0;
    active_ = source_ != target.family || (target.family == MidiStandard::GS && (adaptSameFamily || forcedMap_));
    xgDrumPatch_ = source_ == MidiStandard::XG && target.family != MidiStandard::XG;
    reset();
}

void Emulator::setConversion(std::shared_ptr<const ConversionTables> tables, const ConvSetup& setup) {
    tables_ = setup.direction == ConvDirection::None ? nullptr : std::move(tables);
    setup_ = tables_ ? setup : ConvSetup();
    rules_ = tables_ ? tables_->rules(setup_) : nullptr;
    for (auto& p : ports_)
        for (auto& c : p.ch) {
            c.kit = nullptr;
            c.level = 0;
        }
}

uint8_t Emulator::volumeOut(const Chan& c) const {
    int v = rules_ ? rules_->cc[7].apply(c.volume) : c.volume;
    v += c.level;
    return uint8_t(v < 0 ? 0 : (v > 127 ? 127 : v));
}

namespace {
// XG drum notes 13-34 on GS/GM modules, from TMIDI Player's module.def [PatchDrumXGtoGS]
// (0 = no counterpart). Notes 13-34 of an XG kit differ from the GS layout. The Open Rim Shot (34),
// which GS lacks, plays the tight snare (E2) rather than nothing.
const uint8_t kXgDrumToGs[22] = {
    86, 87, 27, 28, 29, 30, 26, 32, 33, 34, 33,  // 13 Surdo Mute .. 23 Seq Click L
    33, 0, 0, 0, 0, 25, 85, 40, 31, 35, 40,      // 24 Seq Click H .. 34 Open Rim Shot
};
} // namespace

bool Emulator::mapNote(int port, uint8_t* msg) const {
    if (port < 0 || port >= kMaxPorts) return true;
    const Chan& c = ports_[port].ch[msg[0] & 0x0F];
    uint8_t note = msg[1];
    int vadj = 0;
    if (c.kit) {
        if (c.kit->note[note] == -2) return false;
        if (c.kit->note[note] >= 0) msg[1] = uint8_t(c.kit->note[note]);
        vadj = c.kit->velocity[note];
    } else if (xgDrumPatch_ && c.srcDrum && c.msb != 126 && note >= 13 && note <= 34) {
        if (!kXgDrumToGs[note - 13]) return false;
        msg[1] = kXgDrumToGs[note - 13];
    }
    if ((rules_ || vadj) && (msg[0] & 0xF0) == 0x90 && msg[2] > 0) {
        int v = (rules_ ? rules_->velocity.apply(msg[2]) : msg[2]) + vadj;
        msg[2] = uint8_t(v < 1 ? 1 : (v > 127 ? 127 : v));
    }
    return true;
}

uint8_t Emulator::toneMap(uint8_t fileMap) const {
    if (forcedMap_) return uint8_t(forcedMap_);
    return (source_ == MidiStandard::GS && fileMap <= target_.gsMaxMap) ? fileMap : 0;
}

uint8_t Emulator::gsTargetMap(uint8_t fileMap, bool drum, int variation, int program) const {
    if (!forcedMap_ && !fileMap && tables_ && setup_.sc55MapUp &&
        (drum ? tables_->gsKitExists(1, program) : tables_->gsVoiceExists(1, variation, program)))
        return 1;
    return toneMap(fileMap);
}

std::vector<Bytes> Emulator::forcedMapMessages() const {
    std::vector<Bytes> v;
    if (!forcedMap_) return v;
    // SC-88Pro / SC-8850 "TONE MAP-0 NUMBER": the map used by a part while CC#32 = 0.
    for (int block = 0; block < 16; block++) v.push_back(gsSysex(0x404001 | (uint32_t(block) << 8), {uint8_t(forcedMap_)}));
    return v;
}

void Emulator::reset() {
    for (auto& p : ports_) p.resetFor(source_);
}

void Emulator::convert(int port, const uint8_t* msg, size_t len, std::vector<Bytes>& out) {
    if (len == 0) return;
    if (!active_ || port < 0 || port >= kMaxPorts) {
        out.emplace_back(msg, msg + len);
        return;
    }
    uint8_t s = msg[0];
    if (s >= 0x80 && s < 0xF0) {
        channelMessage(port, s, len > 1 ? msg[1] : 0, len > 2 ? msg[2] : 0, out);
    } else if (s == 0xF0) {
        sysex(port, msg, len, out);
    } else {
        out.emplace_back(msg, msg + len);
    }
}

void Emulator::emitNrpn(int ch, uint8_t msb, uint8_t lsb, uint8_t value, std::vector<Bytes>& out) {
    out.push_back(cc(ch, 99, msb));
    out.push_back(cc(ch, 98, lsb));
    out.push_back(cc(ch, 6, value));
    out.push_back(cc(ch, 101, 127));
    out.push_back(cc(ch, 100, 127));
}

// The part's attack time with the conversion table's correction for the sound in use (an envelope
// value, 64 = the sound's own; -50..+50 around it).
uint8_t Emulator::attackOut(const Chan& c) const {
    if (!c.attackAdj) return c.attack;
    return uint8_t(std::clamp(int(c.attack) + c.attackAdj, 14, 114));
}

void Emulator::emitAttack(int ch, uint8_t value, std::vector<Bytes>& out) {
    switch (target_.family) {
    case MidiStandard::GS:
        if (target_.nativeSoundCtrl) out.push_back(cc(ch, 73, value));
        else emitNrpn(ch, 1, 0x63, value, out);
        break;
    case MidiStandard::XG: emitNrpn(ch, 1, 0x63, value, out); break;
    case MidiStandard::GM2: out.push_back(cc(ch, 73, value)); break;
    default: break;
    }
}

void Emulator::channelMessage(int port, uint8_t st, uint8_t d1, uint8_t d2, std::vector<Bytes>& out) {
    int ch = st & 0x0F;
    Chan& c = ports_[port].ch[ch];
    MidiStandard tf = target_.family;
    if ((st & 0xF0) == 0xC0) {
        programChange(port, ch, d1, out);
        return;
    }
    if ((st & 0xF0) != 0xB0) {
        out.push_back({st, d1, d2});
        if ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) out.back().pop_back();
        return;
    }
    switch (d1) {
    case 0:
        c.msb = d2;
        return;
    case 32:
        c.lsb = d2;
        if (tf == MidiStandard::GS && source_ == MidiStandard::GS) {
            // Tone map select: forced map, or clamped to what the target understands.
            out.push_back(cc(ch, 32, toneMap(d2)));
        }
        return;
    case 99:
    case 98:
        if (d1 == 99) c.nrpnMsb = d2;
        else c.nrpnLsb = d2;
        c.nrpnSel = true;
        if (tf == MidiStandard::GM || tf == MidiStandard::GM2) return;
        out.push_back(cc(ch, d1, d2));
        return;
    case 101:
    case 100:
        c.nrpnSel = false;
        out.push_back(cc(ch, d1, d2));
        return;
    case 6:
    case 38:
        if (c.nrpnSel && d1 == 6 && rules_) {
            auto it = rules_->nrpn.find(uint16_t((c.nrpnMsb << 7) | c.nrpnLsb));
            if (it != rules_->nrpn.end()) d2 = uint8_t(it->second.apply(d2));
        }
        if (c.nrpnSel) {
            if (d1 == 38) {
                if (tf == MidiStandard::GS || tf == MidiStandard::XG) out.push_back(cc(ch, d1, d2));
                return;
            }
            if (tf == MidiStandard::GM2) {
                if (c.nrpnMsb == 1 && c.nrpnLsb == 0x63) {
                    c.attack = d2;
                    d2 = attackOut(c);
                }
                if (c.nrpnMsb == 1)
                    for (int i = 0; i < kSoundParamCount; i++)
                        if (soundParamNrpnLsb(SoundParam(i)) == c.nrpnLsb && soundParamGm2Cc(SoundParam(i)) >= 0)
                            out.push_back(cc(ch, uint8_t(soundParamGm2Cc(SoundParam(i))), d2));
                return;
            }
            if (tf == MidiStandard::GM) return;
            if (c.nrpnMsb == 1 && c.nrpnLsb == 0x63) {  // attack time: with the table's correction
                c.attack = d2;
                d2 = attackOut(c);
            }
            if (tf == MidiStandard::GS && c.nrpnMsb == 1 &&
                (c.nrpnLsb == 0x24 || c.nrpnLsb == 0x30 || c.nrpnLsb == 0x31 || c.nrpnLsb == 0x34 || c.nrpnLsb == 0x35))
                return;  // XG-only part EQ / HPF NRPNs
            out.push_back(cc(ch, d1, d2));
            return;
        }
        out.push_back(cc(ch, d1, d2));
        return;
    case 7:
        c.volume = d2;
        c.haveVolume = true;
        out.push_back(cc(ch, 7, volumeOut(c)));
        return;
    case 94:
        if (source_ == MidiStandard::XG && tf == MidiStandard::GS && ports_[port].xgVarConnection == 0) return;
        if (tf == MidiStandard::GS && !target_.hasDelay && source_ != MidiStandard::GS) return;
        out.push_back(cc(ch, d1, d2));
        return;
    default:
        break;
    }
    if (rules_ && rules_->cc[d1].set) d2 = uint8_t(rules_->cc[d1].apply(d2));
    if (d1 == 73) {  // attack time: with the table's correction
        c.attack = d2;
        d2 = attackOut(c);
    }
    if (d1 >= 71 && d1 <= 78 && (source_ == MidiStandard::GM2 || source_ == MidiStandard::XG)) {
        SoundParam p = SoundParam::Count;
        for (int i = 0; i < kSoundParamCount; i++)
            if (soundParamGm2Cc(SoundParam(i)) == d1) p = SoundParam(i);
        if (p != SoundParam::Count) {
            if (tf == MidiStandard::GS && !target_.nativeSoundCtrl) {
                emitNrpn(ch, 1, uint8_t(soundParamNrpnLsb(p)), d2, out);
                return;
            }
            if (tf == MidiStandard::XG && d1 >= 75) {
                emitNrpn(ch, 1, uint8_t(soundParamNrpnLsb(p)), d2, out);
                return;
            }
            if (tf == MidiStandard::GM) return;
        }
    }
    out.push_back(cc(ch, d1, d2));
}

uint8_t Emulator::mapDrumKit(MidiStandard from, const DeviceProfile& to, uint8_t program, bool sfxKit, bool* toSfxKit) {
    if (toSfxKit) *toSfxKit = false;
    // Canonical numbering = GS kit numbers (0-based).
    int kit = program;
    if (from == MidiStandard::XG) {
        if (sfxKit) {
            kit = 56;
        } else {
            switch (program) {
            case 2: case 3: kit = 0; break;
            case 9: kit = 8; break;
            case 17: kit = 16; break;
            case 26: kit = 25; break;
            case 27: kit = 26; break;   // Dance
            case 28: kit = 9; break;    // Hip Hop
            case 29: kit = 10; break;   // Jungle
            case 33: kit = 32; break;
            case 112: kit = 0; break;   // R&B
            case 113: kit = 16; break;  // Rock 3
            case 0: case 1: case 8: case 16: case 24: case 25: case 32: case 40: case 48: break;
            default: kit = 0; break;    // undefined XG kits play the Standard Kit
            }
        }
    }
    switch (to.family) {
    case MidiStandard::XG: {
        switch (kit) {
        case 56: case 57: case 58: case 59:
            if (toSfxKit) *toSfxKit = true;
            return 0;
        case 26: return 27;
        case 9: return 28;
        case 10: return 29;
        case 127: return 0;
        default: break;
        }
        static const uint8_t xgKits[] = {0, 1, 2, 3, 8, 9, 16, 17, 24, 25, 26, 27, 28, 29, 32, 33, 40, 48};
        for (uint8_t k : xgKits)
            if (k == kit) return uint8_t(kit);
        return uint8_t(std::min(kit & ~7, 48));
    }
    case MidiStandard::GS:
        if (to.kind != DeviceKind::SC55) return uint8_t(kit);
        [[fallthrough]];
    case MidiStandard::GM2: {
        static const uint8_t base[] = {0, 8, 16, 24, 25, 32, 40, 48, 56};
        for (uint8_t k : base)
            if (k == kit) return uint8_t(kit);
        if (kit == 127) return to.family == MidiStandard::GS ? 127 : 0;
        int f = kit & ~7;
        return uint8_t(f <= 56 ? f : 0);
    }
    default:
        return 0;
    }
}

void Emulator::programChange(int port, int ch, uint8_t prog, std::vector<Bytes>& out) {
    Chan& c = ports_[port].ch[ch];
    c.program = prog;
    bool drum = c.srcDrum, sfxKit = false, xgSfxVoice = false;
    uint8_t var = 0, map = 0;
    switch (source_) {
    case MidiStandard::XG:
        if (c.msb == 127 || c.msb == 126) {
            drum = true;
            sfxKit = c.msb == 126;
        } else {
            drum = false;
            xgSfxVoice = c.msb == 64;
            var = c.lsb;
        }
        break;
    case MidiStandard::GM2:
        if (c.msb == 120) drum = true;
        else if (c.msb == 121) drum = false;
        var = c.lsb;
        break;
    case MidiStandard::GS:
        var = c.msb;
        map = c.lsb;
        break;
    default:
        drum = c.srcDrum || ch == 9;
        break;
    }
    c.srcDrum = drum;

    // Conversion tables, in source terms.
    const ConvTarget* tv = nullptr;  // voice
    const ConvTarget* tk = nullptr;  // drum kit
    c.kit = nullptr;
    c.level = 0;
    if (tables_) {
        if (source_ == MidiStandard::XG) {
            if (drum) tk = tables_->drumKit(setup_, sfxKit ? 126 : 127, 0, prog);
            else tv = tables_->voice(setup_, c.msb, c.lsb, prog);
        } else if (source_ == MidiStandard::GS) {
            if (drum) tk = tables_->drumKit(setup_, 0, map, prog);
            else tv = tables_->voice(setup_, var, map, prog);
        }
        if (tk) {
            c.kit = tk->notes;
            c.level = tk->volume;
        } else if (tv) {
            c.level = tv->volume;
        }
    }
    // A sound with an attack correction, or the song's own attack again after one.
    const int8_t attackAdj = tv ? tv->attack : 0;
    const bool attackChanged = attackAdj != c.attackAdj;
    c.attackAdj = attackAdj;
    if (tv || tk) {
        const ConvTarget& t = tv ? *tv : *tk;
        uint16_t set = kNoSet;
        if (target_.family == MidiStandard::GS) {
            uint8_t lsb = target_.gsMaxMap > 0 ? uint8_t(forcedMap_ ? forcedMap_ : std::min<int>(t.lsb, target_.gsMaxMap)) : 0;
            if (drum) set = uint16_t((lsb << 8) | t.program);
            gsDrumMap(port, ch, drum, set, out);
            out.push_back(cc(ch, 0, drum ? 0 : t.msb));
            if (target_.gsMaxMap > 0) out.push_back(cc(ch, 32, lsb));
        } else {
            out.push_back(cc(ch, 0, t.msb));
            out.push_back(cc(ch, 32, t.lsb));
        }
        out.push_back({uint8_t(0xC0 | ch), t.program});
        if (set != kNoSet) gsDrumSetSelected(port, ch, set, out);
        if (c.haveVolume) out.push_back(cc(ch, 7, volumeOut(c)));
        if (attackChanged) emitAttack(ch, attackOut(c), out);
        return;
    }
    if (tables_ && c.haveVolume) out.push_back(cc(ch, 7, volumeOut(c)));

    switch (target_.family) {
    case MidiStandard::GS: {
        if (drum) {
            uint8_t kit = source_ == MidiStandard::GS ? prog : mapDrumKit(source_, target_, prog, sfxKit, nullptr);
            uint8_t lsb = target_.gsMaxMap > 0 ? gsTargetMap(map, true, 0, prog) : 0;
            uint16_t set = uint16_t((lsb << 8) | kit);
            gsDrumMap(port, ch, true, set, out);
            out.push_back(cc(ch, 0, 0));
            if (target_.gsMaxMap > 0) out.push_back(cc(ch, 32, lsb));
            out.push_back({uint8_t(0xC0 | ch), kit});
            gsDrumSetSelected(port, ch, set, out);
        } else {
            gsDrumMap(port, ch, false, kNoSet, out);
            uint8_t p = xgSfxVoice ? xgSfxVoiceToGm(prog) : prog;
            out.push_back(cc(ch, 0, source_ == MidiStandard::GS ? var : 0));
            if (target_.gsMaxMap > 0) out.push_back(cc(ch, 32, source_ == MidiStandard::GS ? gsTargetMap(map, false, var, prog) : toneMap(map)));
            out.push_back({uint8_t(0xC0 | ch), p});
        }
        break;
    }
    case MidiStandard::XG: {
        if (drum) {
            bool toSfx = false;
            uint8_t kit = mapDrumKit(source_, target_, prog, sfxKit, &toSfx);
            out.push_back(cc(ch, 0, toSfx ? 126 : 127));
            out.push_back(cc(ch, 32, 0));
            out.push_back({uint8_t(0xC0 | ch), kit});
        } else {
            out.push_back(cc(ch, 0, 0));
            out.push_back(cc(ch, 32, source_ == MidiStandard::XG ? var : 0));
            out.push_back({uint8_t(0xC0 | ch), prog});
        }
        break;
    }
    case MidiStandard::GM2: {
        if (drum) {
            out.push_back(cc(ch, 0, 120));
            out.push_back(cc(ch, 32, 0));
            out.push_back({uint8_t(0xC0 | ch), mapDrumKit(source_, target_, prog, sfxKit, nullptr)});
        } else {
            out.push_back(cc(ch, 0, 121));
            out.push_back(cc(ch, 32, source_ == MidiStandard::GM2 ? var : 0));
            out.push_back({uint8_t(0xC0 | ch), xgSfxVoice ? xgSfxVoiceToGm(prog) : prog});
        }
        break;
    }
    default:
        out.push_back({uint8_t(0xC0 | ch), drum ? uint8_t(0) : (xgSfxVoice ? xgSfxVoiceToGm(prog) : prog)});
        break;
    }
    if (attackChanged) emitAttack(ch, attackOut(c), out);   // the song's own attack again
}

void Emulator::emitReset(std::vector<Bytes>& out) {
    switch (target_.family) {
    case MidiStandard::GS:
        out.push_back(gsReset());
        for (Bytes& b : forcedMapMessages()) out.push_back(std::move(b));
        break;
    case MidiStandard::XG: out.push_back(xgSystemOn()); break;
    case MidiStandard::GM2: out.push_back(gm2SystemOn()); break;
    default: out.push_back(gmSystemOn()); break;
    }
}

void Emulator::setSourceDrum(int port, int ch, bool drum, uint8_t map, std::vector<Bytes>& out) {
    Chan& c = ports_[port].ch[ch];
    c.srcDrum = drum;
    switch (target_.family) {
    case MidiStandard::GS:
        (void)map;  // the drum map follows channel 10 and the sets in use (gsDrumMap)
        gsDrumMap(port, ch, drum, c.tgtSet, out);
        break;
    case MidiStandard::XG:
        out.push_back(xgSysex(0x08, uint8_t(port * 16 + ch), 0x07, {uint8_t(drum ? (map >= 2 ? 3 : 2) : 0)}));
        c.msb = drum ? 127 : 0;
        break;
    case MidiStandard::GM2:
        out.push_back(cc(ch, 0, drum ? 120 : 121));
        out.push_back(cc(ch, 32, 0));
        out.push_back({uint8_t(0xC0 | ch), drum ? mapDrumKit(source_, target_, c.program, false, nullptr) : c.program});
        c.msb = drum ? 120 : 121;
        break;
    default:
        break;
    }
}

// GS modules have two drum maps, one drum set each: channel 10 uses map 1, other drum parts map 2, or
// map 1 when they play channel 10's set (the same set should not take the second map). `set` is the
// part's set, (map << 8) | program, or kNoSet while it is not known. A GS song's own maps stay.
void Emulator::gsDrumMap(int port, int ch, bool drum, uint16_t set, std::vector<Bytes>& out) {
    Port& p = ports_[port];
    Chan& c = p.ch[ch];
    uint8_t want = 0;
    if (drum) {
        if (source_ == MidiStandard::GS && c.tgtMap) return;
        want = (ch == 9 || (set != kNoSet && p.ch[9].tgtMap == 1 && p.mapSet[1] == set)) ? 1 : 2;
    }
    if (want == c.tgtMap) return;
    out.push_back(gsSysex(0x401015 | (uint32_t(gsBlockOfChannel(ch)) << 8), {want}));
    c.tgtMap = want;
}

// A drum part selected `set`, which its map holds now. When channel 10 selects another set, the parts
// that shared map 1 with it move to map 2 and select their own set there again.
void Emulator::gsDrumSetSelected(int port, int ch, uint16_t set, std::vector<Bytes>& out) {
    Port& p = ports_[port];
    Chan& c = p.ch[ch];
    c.tgtSet = set;
    if (c.tgtMap == 0) return;
    p.mapSet[c.tgtMap] = set;
    if (ch != 9 || source_ == MidiStandard::GS) return;
    for (int o = 0; o < 16; o++) {
        Chan& oc = p.ch[o];
        if (o == 9 || oc.tgtMap != 1 || oc.tgtSet == set || oc.tgtSet == kNoSet) continue;
        out.push_back(gsSysex(0x401015 | (uint32_t(gsBlockOfChannel(o)) << 8), {2}));
        oc.tgtMap = 2;
        out.push_back(cc(o, 0, 0));
        if (target_.gsMaxMap > 0) out.push_back(cc(o, 32, uint8_t(oc.tgtSet >> 8)));
        out.push_back({uint8_t(0xC0 | o), uint8_t(oc.tgtSet & 0x7F)});
        p.mapSet[2] = oc.tgtSet;
    }
}

void Emulator::assignGsEfxPart(int port, int ch, std::vector<Bytes>& out) {
    Port& p = ports_[port];
    if (!target_.hasEfx) return;
    if (p.gsEfxPart >= 0 && p.gsEfxPart != ch)
        out.push_back(gsSysex(0x404022 | (uint32_t(gsBlockOfChannel(p.gsEfxPart)) << 8), {0}));
    p.gsEfxPart = ch;
    if (ch >= 0) out.push_back(gsSysex(0x404022 | (uint32_t(gsBlockOfChannel(ch)) << 8), {1}));
}

void Emulator::gsPartToTarget(int port, int ch, uint8_t a3, uint8_t v, std::vector<Bytes>& out) {
    MidiStandard tf = target_.family;
    Chan& c = ports_[port].ch[ch];
    switch (a3) {
    case 0x00: c.msb = v; return;
    case 0x01: programChange(port, ch, v, out); return;
    case 0x02:
        if (tf == MidiStandard::XG) out.push_back(xgSysex(0x08, uint8_t(port * 16 + ch), 0x04, {uint8_t(v >= 16 ? 0x7F : v)}));
        return;
    case 0x15: setSourceDrum(port, ch, v != 0, v, out); return;
    case 0x16:
        if (tf == MidiStandard::XG) out.push_back(xgSysex(0x08, uint8_t(port * 16 + ch), 0x08, {v}));
        else if (tf == MidiStandard::GM2) {
            out.push_back(cc(ch, 101, 0));
            out.push_back(cc(ch, 100, 2));
            out.push_back(cc(ch, 6, v));
            out.push_back(cc(ch, 101, 127));
            out.push_back(cc(ch, 100, 127));
        }
        return;
    case 0x19: out.push_back(cc(ch, 7, v)); return;
    case 0x1C:
        if (v == 0 && tf == MidiStandard::XG) out.push_back(xgSysex(0x08, uint8_t(port * 16 + ch), 0x0E, {0}));
        else out.push_back(cc(ch, 10, v ? v : 64));
        return;
    case 0x21: out.push_back(cc(ch, 93, v)); return;
    case 0x22: out.push_back(cc(ch, 91, v)); return;
    case 0x2C:
        if (tf == MidiStandard::XG) out.push_back(cc(ch, 94, v));
        return;
    default: break;
    }
    if (a3 >= 0x30 && a3 <= 0x37) {
        SoundParam p = gsToneModifyParam(a3);
        if (tf == MidiStandard::XG) out.push_back(xgSysex(0x08, uint8_t(port * 16 + ch), xgPartAddressOf(p), {v}));
        else if (tf == MidiStandard::GM2) out.push_back(cc(ch, uint8_t(soundParamGm2Cc(p)), v));
    }
}

void Emulator::gsSystemToTarget(int port, uint8_t a2, uint8_t a3, uint8_t v, std::vector<Bytes>& out) {
    MidiStandard tf = target_.family;
    Port& p = ports_[port];
    if (a2 == 0x01) {
        switch (a3) {
        case 0x30:
            if (tf == MidiStandard::XG) {
                uint8_t m, l;
                gsMacroToXgReverb(v, m, l);
                out.push_back(xgSysex(0x02, 0x01, 0x00, {m, l}));
            } else if (tf == MidiStandard::GM2) {
                out.push_back(gm2GlobalReverb(0, uint8_t(gsMacroToGm2Reverb(v))));
            }
            break;
        case 0x33:
            if (tf == MidiStandard::XG) out.push_back(xgSysex(0x02, 0x01, 0x0C, {v}));
            break;
        case 0x34:
            if (tf == MidiStandard::GM2) out.push_back(gm2GlobalReverb(1, v));
            break;
        case 0x38:
            if (tf == MidiStandard::XG) {
                uint8_t m, l;
                gsMacroToXgChorus(v, m, l);
                out.push_back(xgSysex(0x02, 0x01, 0x20, {m, l}));
            } else if (tf == MidiStandard::GM2) {
                static const uint8_t t[8] = {0, 1, 2, 3, 4, 5, 5, 4};
                out.push_back(gm2GlobalChorus(0, t[std::min<int>(v, 7)]));
            }
            break;
        case 0x3A:
            if (tf == MidiStandard::XG) out.push_back(xgSysex(0x02, 0x01, 0x2C, {v}));
            break;
        case 0x3B:
            if (tf == MidiStandard::GM2) out.push_back(gm2GlobalChorus(3, v));
            break;
        case 0x3D:
            if (tf == MidiStandard::GM2) out.push_back(gm2GlobalChorus(1, v));
            break;
        case 0x3E:
            if (tf == MidiStandard::GM2) out.push_back(gm2GlobalChorus(2, v));
            break;
        case 0x3F:
            if (tf == MidiStandard::XG) out.push_back(xgSysex(0x02, 0x01, 0x2E, {v}));
            else if (tf == MidiStandard::GM2) out.push_back(gm2GlobalChorus(4, v));
            break;
        case 0x50:
            p.gsDelayMacro = v;
            p.gsDelayUsed = true;
            if (tf == MidiStandard::XG && p.gsEfxMsb == 0) {
                uint8_t m = (v >= 4 && v <= 7) ? 0x08 : (v == 9 ? 0x06 : 0x05);
                out.push_back(xgSysex(0x02, 0x01, 0x40, {m, 0}));
                out.push_back(xgSysex(0x02, 0x01, 0x5A, {1}));
                p.xgVarConnection = 1;
            }
            break;
        case 0x58:
            if (tf == MidiStandard::XG && p.xgVarConnection == 1) out.push_back(xgSysex(0x02, 0x01, 0x56, {v}));
            break;
        default: break;
        }
        return;
    }
    if (a2 == 0x03 && tf == MidiStandard::XG) {
        if (a3 == 0x00) {
            p.gsEfxMsb = v;
        } else if (a3 == 0x01) {
            p.gsEfxLsb = v;
            uint8_t m = 0x40, l = 0;
            lookupType(kEfxToXg, sizeof kEfxToXg / sizeof kEfxToXg[0], p.gsEfxMsb, p.gsEfxLsb, m, l);
            out.push_back(xgSysex(0x02, 0x01, 0x40, {m, l}));
            out.push_back(xgSysex(0x02, 0x01, 0x5A, {0}));
            p.xgVarConnection = 0;
            if (p.gsEfxAssignMask) {
                for (int c = 0; c < 16; c++)
                    if (p.gsEfxAssignMask & (1u << c)) {
                        out.push_back(xgSysex(0x02, 0x01, 0x5B, {uint8_t(port * 16 + c)}));
                        break;
                    }
            }
        } else if (a3 == 0x17) {
            out.push_back(xgSysex(0x02, 0x01, 0x58, {v}));
        } else if (a3 == 0x18) {
            out.push_back(xgSysex(0x02, 0x01, 0x59, {v}));
        }
    }
}

void Emulator::xgEffectTypeToTarget(int port, int block, std::vector<Bytes>& out) {
    Port& p = ports_[port];
    MidiStandard tf = target_.family;
    if (block == 0) {
        if (tf == MidiStandard::GS) {
            int m = xgReverbToGsMacro(p.xgRevMsb, p.xgRevLsb);
            if (m < 0) out.push_back(gsSysex(0x400133, {0}));
            else out.push_back(gsSysex(0x400130, {uint8_t(m)}));
        } else if (tf == MidiStandard::GM2) {
            int t = xgReverbToGm2(p.xgRevMsb, p.xgRevLsb);
            if (t >= 0) out.push_back(gm2GlobalReverb(0, uint8_t(t)));
        }
    } else if (block == 1) {
        if (tf == MidiStandard::GS) {
            int m = xgChorusToGsMacro(p.xgChoMsb, p.xgChoLsb);
            if (m < 0) out.push_back(gsSysex(0x40013A, {0}));
            else out.push_back(gsSysex(0x400138, {uint8_t(m)}));
        } else if (tf == MidiStandard::GM2) {
            int t = xgChorusToGm2(p.xgChoMsb, p.xgChoLsb);
            if (t >= 0) out.push_back(gm2GlobalChorus(0, uint8_t(t)));
        }
    } else if (tf == MidiStandard::GS) {
        if (p.xgVarConnection == 1) {
            int m = xgVariationDelayToGsMacro(p.xgVarMsb);
            if (m >= 0 && target_.hasDelay) out.push_back(gsSysex(0x400150, {uint8_t(m)}));
            else if (target_.hasDelay) out.push_back(gsSysex(0x400158, {0}));
        } else if (target_.hasEfx) {
            uint8_t m = 0, l = 0;
            if (!lookupType(kXgToEfx, sizeof kXgToEfx / sizeof kXgToEfx[0], p.xgVarMsb, p.xgVarLsb, m, l)) m = l = 0;
            if (m != p.gsEfxMsb || l != p.gsEfxLsb) {
                out.push_back(gsSysex(0x400300, {m, l}));
                p.gsEfxMsb = m;
                p.gsEfxLsb = l;
                if (p.xgVarMsb == 0x08) out.push_back(gsSysex(0x400306, {1}));        // cross feedback
                if (p.xgVarMsb == 0x0B) out.push_back(gsSysex(0x400303, {1}));        // reverse gate
            }
            if (p.xgVarPart < 64 && (p.xgVarPart >> 4) == port) assignGsEfxPart(port, p.xgVarPart & 15, out);
        }
    }
}

void Emulator::gm2GlobalToTarget(int port, bool reverb, uint8_t param, uint8_t value, std::vector<Bytes>& out) {
    (void)port;
    MidiStandard tf = target_.family;
    if (reverb) {
        if (param == 0) {
            if (tf == MidiStandard::GS) out.push_back(gsSysex(0x400130, {uint8_t(gm2ReverbToGsMacro(value))}));
            else if (tf == MidiStandard::XG) {
                uint8_t m, l;
                gsMacroToXgReverb(gm2ReverbToGsMacro(value), m, l);
                out.push_back(xgSysex(0x02, 0x01, 0x00, {m, l}));
            }
        } else if (param == 1 && tf == MidiStandard::GS) {
            out.push_back(gsSysex(0x400134, {value}));
        }
    } else {
        if (param == 0) {
            if (tf == MidiStandard::GS) out.push_back(gsSysex(0x400138, {uint8_t(std::min<int>(value, 5))}));
            else if (tf == MidiStandard::XG) {
                uint8_t m, l;
                gm2ChorusToXg(value, m, l);
                out.push_back(xgSysex(0x02, 0x01, 0x20, {m, l}));
            }
        } else if (tf == MidiStandard::GS) {
            static const uint32_t addr[5] = {0, 0x40013D, 0x40013E, 0x40013B, 0x40013F};
            if (param >= 1 && param <= 4) out.push_back(gsSysex(addr[param], {value}));
        } else if (tf == MidiStandard::XG && param == 4) {
            out.push_back(xgSysex(0x02, 0x01, 0x2E, {value}));
        }
    }
}

void Emulator::xgToTarget(int port, uint8_t hi, uint8_t mid, uint8_t lo, uint8_t v, bool lastByte, std::vector<Bytes>& out) {
    MidiStandard tf = target_.family;
    Port& p = ports_[port];
    if (hi == 0x00 && mid == 0x00) {
        if (lo == 0x7E || lo == 0x7F) {
            for (int i = 0; i < kMaxPorts; i++) ports_[i].resetFor(MidiStandard::XG);
            emitReset(out);
        } else if (lo == 0x04) {
            out.push_back(masterVolumeSysex(uint16_t(v << 7)));
        } else if (lo == 0x06 && tf == MidiStandard::GS) {
            out.push_back(gsSysex(0x400005, {v}));
        }
        return;
    }
    if (hi == 0x02 && mid == 0x01) {
        switch (lo) {
        case 0x00: p.xgRevMsb = v; p.xgRevLsb = 0; if (lastByte) xgEffectTypeToTarget(port, 0, out); break;
        case 0x01: p.xgRevLsb = v; xgEffectTypeToTarget(port, 0, out); break;
        case 0x0C: if (tf == MidiStandard::GS) out.push_back(gsSysex(0x400133, {v})); break;
        case 0x20: p.xgChoMsb = v; p.xgChoLsb = 0; if (lastByte) xgEffectTypeToTarget(port, 1, out); break;
        case 0x21: p.xgChoLsb = v; xgEffectTypeToTarget(port, 1, out); break;
        case 0x2C: if (tf == MidiStandard::GS) out.push_back(gsSysex(0x40013A, {v})); break;
        case 0x2E: if (tf == MidiStandard::GS) out.push_back(gsSysex(0x40013F, {v})); else if (tf == MidiStandard::GM2) out.push_back(gm2GlobalChorus(4, v)); break;
        case 0x40: p.xgVarMsb = v; p.xgVarLsb = 0; if (lastByte) xgEffectTypeToTarget(port, 2, out); break;
        case 0x41: p.xgVarLsb = v; xgEffectTypeToTarget(port, 2, out); break;
        case 0x56:
            if (tf == MidiStandard::GS && p.xgVarConnection == 1 && target_.hasDelay && xgVariationDelayToGsMacro(p.xgVarMsb) >= 0)
                out.push_back(gsSysex(0x400158, {v}));
            break;
        case 0x58:
            if (tf == MidiStandard::GS && p.xgVarConnection == 0 && target_.hasEfx) out.push_back(gsSysex(0x400317, {v}));
            break;
        case 0x59:
            if (tf == MidiStandard::GS && p.xgVarConnection == 0 && target_.hasEfx) out.push_back(gsSysex(0x400318, {v}));
            break;
        case 0x5A: p.xgVarConnection = v; xgEffectTypeToTarget(port, 2, out); break;
        case 0x5B:
            p.xgVarPart = v;
            if (tf == MidiStandard::GS && p.xgVarConnection == 0) {
                if (v < 64 && (v >> 4) == port) assignGsEfxPart(port, v & 15, out);
                else assignGsEfxPart(port, -1, out);
            }
            break;
        default: break;
        }
        return;
    }
    if (hi == 0x03 && mid <= 1) {
        // Insertion effects (MU100 and later): the GS target has a single EFX.
        if (tf != MidiStandard::GS || !target_.hasEfx) return;
        if (lo == 0x00 || lo == 0x01) {
            static uint8_t insMsb[kMaxPorts][2], insLsb[kMaxPorts][2];
            if (lo == 0x00) {
                insMsb[port][mid] = v;
                insLsb[port][mid] = 0;
                if (!lastByte) return;
            } else {
                insLsb[port][mid] = v;
            }
            uint8_t m = 0, l = 0;
            if (!lookupType(kXgToEfx, sizeof kXgToEfx / sizeof kXgToEfx[0], insMsb[port][mid], insLsb[port][mid], m, l)) m = l = 0;
            if (m || l) {
                out.push_back(gsSysex(0x400300, {m, l}));
                p.gsEfxMsb = m;
                p.gsEfxLsb = l;
            }
        } else if (lo == 0x0C) {
            if (v < 64 && (v >> 4) == port) assignGsEfxPart(port, v & 15, out);
        }
        return;
    }
    if (hi == 0x08) {
        int tport = mid >> 4, ch = mid & 0x0F;
        if (tport >= kMaxPorts) return;
        Chan& c = ports_[tport].ch[ch];
        switch (lo) {
        case 0x01: c.msb = v; return;
        case 0x02: c.lsb = v; return;
        case 0x03: programChange(tport, ch, v, out); return;
        case 0x04:
            if (tf == MidiStandard::GS) out.push_back(gsSysex(0x401002 | (uint32_t(gsBlockOfChannel(ch)) << 8), {uint8_t(v >= 16 ? 0x10 : v)}));
            return;
        case 0x07: setSourceDrum(tport, ch, v != 0, uint8_t(v >= 3 ? 2 : 1), out); return;
        case 0x08:
            if (tf == MidiStandard::GS) out.push_back(gsSysex(0x401016 | (uint32_t(gsBlockOfChannel(ch)) << 8), {v}));
            else if (tf == MidiStandard::GM2) {
                out.push_back(cc(ch, 101, 0));
                out.push_back(cc(ch, 100, 2));
                out.push_back(cc(ch, 6, v));
                out.push_back(cc(ch, 101, 127));
                out.push_back(cc(ch, 100, 127));
            }
            return;
        case 0x0B: out.push_back(cc(ch, 7, v)); return;
        case 0x0E:
            if (v == 0 && tf == MidiStandard::GS) out.push_back(gsSysex(0x40101C | (uint32_t(gsBlockOfChannel(ch)) << 8), {0}));
            else out.push_back(cc(ch, 10, v ? v : 64));
            return;
        case 0x12: out.push_back(cc(ch, 93, v)); return;
        case 0x13: out.push_back(cc(ch, 91, v)); return;
        case 0x14:
            if (tf == MidiStandard::GS && target_.hasDelay && p.xgVarConnection == 1) out.push_back(cc(ch, 94, v));
            return;
        case 0x23:
            out.push_back(cc(ch, 101, 0));
            out.push_back(cc(ch, 100, 0));
            out.push_back(cc(ch, 6, uint8_t(std::max(0, int(v) - 64))));
            out.push_back(cc(ch, 101, 127));
            out.push_back(cc(ch, 100, 127));
            return;
        default: break;
        }
        if (lo >= 0x15 && lo <= 0x1C) {
            SoundParam sp = xgPartSoundParam(lo);
            if (sp == SoundParam::Attack) {  // with the table's correction
                c.attack = v;
                v = attackOut(c);
            }
            if (tf == MidiStandard::GS) emitNrpn(ch, 1, uint8_t(soundParamNrpnLsb(sp)), v, out);
            else if (tf == MidiStandard::GM2) out.push_back(cc(ch, uint8_t(soundParamGm2Cc(sp)), v));
        }
        return;
    }
    if ((hi & 0xF0) == 0x30 && tf == MidiStandard::GS) {
        // Drum setup -> GS drum instrument NRPNs on the drum parts of this port.
        static const int nrpnFor[8] = {0x18, -1, 0x1A, -1, 0x1C, 0x1D, 0x1E, 0x1F};
        if (lo > 7 || nrpnFor[lo] < 0) return;
        for (int c = 0; c < 16; c++)
            if (ports_[port].ch[c].srcDrum) emitNrpn(c, uint8_t(nrpnFor[lo]), mid, v, out);
    }
}

void Emulator::sysex(int port, const uint8_t* m, size_t n, std::vector<Bytes>& out) {
    MidiStandard tf = target_.family;
    // Universal messages
    if (n >= 6 && m[1] == 0x7E && m[3] == 0x09) {
        if (m[4] == 0x01 || m[4] == 0x03) {
            ports_[port].resetFor(source_);
            emitReset(out);
            return;
        }
        if (tf == MidiStandard::GM2 || tf == MidiStandard::GM) out.emplace_back(m, m + n);
        return;
    }
    if (n >= 13 && m[1] == 0x7F && m[3] == 0x04 && m[4] == 0x05) {
        if (tf == MidiStandard::GM2) {
            out.emplace_back(m, m + n);
            return;
        }
        if (m[5] == 1 && m[6] == 1 && m[7] == 1 && m[8] == 1 && (m[9] == 1 || m[9] == 2)) {
            for (size_t i = 10; i + 2 < n; i += 2) gm2GlobalToTarget(port, m[9] == 1, m[i], m[i + 1], out);
        }
        return;
    }
    // Roland GS
    if (n >= 10 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12) {
        size_t dataEnd = (m[n - 1] == 0xF7) ? n - 2 : n - 1;
        uint32_t addr = (uint32_t(m[5]) << 14) | (uint32_t(m[6]) << 7) | m[7];
        if (tf == MidiStandard::GS) {
            // A converted song's own drum part setting (GS messages in an XG song, say) makes the part
            // a drum part on the map the rules give it (gsDrumMap), not on the map the song names.
            if (source_ != MidiStandard::GS && n == 11 && m[5] == 0x40 && (m[6] & 0xF0) == 0x10 && m[7] == 0x15) {
                int ch = gsChannelOfBlock(m[6] & 0x0F);
                Chan& c = ports_[port].ch[ch];
                c.srcDrum = m[8] != 0;
                gsDrumMap(port, ch, c.srcDrum, c.tgtSet, out);
                return;
            }
            // Native: pass through, but keep the drum-part bookkeeping in sync.
            for (size_t i = 8; i < dataEnd; i++) {
                uint32_t a = addr + uint32_t(i - 8);
                uint8_t a1 = uint8_t((a >> 14) & 0x7F), a2 = uint8_t((a >> 7) & 0x7F), a3 = uint8_t(a & 0x7F);
                int tp = port;
                if (a1 == 0x50) {
                    tp = port ^ 1;
                    a1 = 0x40;
                }
                if (tp >= kMaxPorts) continue;
                if (a1 == 0x40 && (a2 & 0xF0) == 0x10 && a3 == 0x15) {
                    Chan& c = ports_[tp].ch[gsChannelOfBlock(a2 & 0x0F)];
                    c.srcDrum = m[i] != 0;
                    c.tgtMap = uint8_t(std::min<int>(m[i], 2));
                }
                if (a1 == 0x40 && a2 == 0x00 && a3 == 0x7F) ports_[tp].resetFor(source_);
            }
            bool isReset = n == 11 && m[6] == 0x00 && m[7] == 0x7F && (m[5] == 0x40 || m[5] == 0x50 || m[5] == 0x00);
            if (forcedMap_ && n == 11 && (m[5] == 0x40 || m[5] == 0x50) && (m[6] & 0xF0) == 0x40 && (m[7] == 0x00 || m[7] == 0x01)) {
                // The song's own tone map selection is replaced by the forced map.
                uint32_t a = (uint32_t(m[5]) << 16) | (uint32_t(m[6]) << 8) | m[7];
                out.push_back(gsSysex(a, {uint8_t(m[7] == 0x00 && m[8] == 0 ? 0 : forcedMap_)}, m[2]));
                return;
            }
            out.emplace_back(m, m + n);
            if (isReset)
                for (Bytes& b : forcedMapMessages()) out.push_back(std::move(b));
            return;
        }
        for (size_t i = 8; i < dataEnd; i++) {
            uint32_t a = addr + uint32_t(i - 8);
            uint8_t a1 = uint8_t((a >> 14) & 0x7F), a2 = uint8_t((a >> 7) & 0x7F), a3 = uint8_t(a & 0x7F);
            uint8_t v = m[i];
            int tp = port;
            if (a1 == 0x50) {
                tp = port ^ 1;
                a1 = 0x40;
            }
            if (tp >= kMaxPorts) continue;
            if ((a1 == 0x40 && a2 == 0x00 && a3 == 0x7F) || (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F)) {
                ports_[tp].resetFor(MidiStandard::GS);
                emitReset(out);
                break;
            }
            if (a1 != 0x40) continue;
            if (a2 == 0x00) {
                if (a3 == 0x04) out.push_back(masterVolumeSysex(uint16_t(v << 7)));
                else if (a3 == 0x05 && tf == MidiStandard::XG) out.push_back(xgSysex(0x00, 0x00, 0x06, {v}));
            } else if ((a2 & 0xF0) == 0x10) {
                gsPartToTarget(tp, gsChannelOfBlock(a2 & 0x0F), a3, v, out);
            } else if ((a2 & 0xF0) == 0x40 && a3 == 0x22) {
                int ch = gsChannelOfBlock(a2 & 0x0F);
                Port& p = ports_[tp];
                if (v) p.gsEfxAssignMask |= uint16_t(1u << ch);
                else p.gsEfxAssignMask &= uint16_t(~(1u << ch));
                if (tf == MidiStandard::XG && v && p.gsEfxMsb) out.push_back(xgSysex(0x02, 0x01, 0x5B, {uint8_t(tp * 16 + ch)}));
            } else if (a2 <= 0x03) {
                gsSystemToTarget(tp, a2, a3, v, out);
            }
        }
        return;
    }
    // Yamaha XG
    if (n >= 8 && m[1] == 0x43 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C) {
        if (tf == MidiStandard::XG) {
            out.emplace_back(m, m + n);
            return;
        }
        uint32_t addr = (uint32_t(m[4]) << 14) | (uint32_t(m[5]) << 7) | m[6];
        size_t dataEnd = (m[n - 1] == 0xF7) ? n - 1 : n;
        for (size_t i = 7; i < dataEnd; i++) {
            uint32_t a = addr + uint32_t(i - 7);
            xgToTarget(port, uint8_t((a >> 14) & 0x7F), uint8_t((a >> 7) & 0x7F), uint8_t(a & 0x7F), m[i], i + 1 == dataEnd, out);
            if (a == 0x7E || a == 0x7F) break;
        }
        return;
    }
    out.emplace_back(m, m + n);
}

} // namespace immidi

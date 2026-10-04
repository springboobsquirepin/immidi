#include "SynthState.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace immidi {

const char* soundParamName(SoundParam p) {
    switch (p) {
    case SoundParam::VibRate: return "Vibrato Rate";
    case SoundParam::VibDepth: return "Vibrato Depth";
    case SoundParam::VibDelay: return "Vibrato Delay";
    case SoundParam::Cutoff: return "Filter Cutoff";
    case SoundParam::Resonance: return "Filter Resonance";
    case SoundParam::Attack: return "Attack Time";
    case SoundParam::Decay: return "Decay Time";
    case SoundParam::Release: return "Release Time";
    case SoundParam::HpfCutoff: return "HPF Cutoff";
    case SoundParam::EqBassGain: return "EQ Bass Gain";
    case SoundParam::EqTrebleGain: return "EQ Treble Gain";
    case SoundParam::EqBassFreq: return "EQ Bass Freq";
    case SoundParam::EqTrebleFreq: return "EQ Treble Freq";
    default: return "?";
    }
}

const char* soundParamShortName(SoundParam p) {
    switch (p) {
    case SoundParam::VibRate: return "VRate";
    case SoundParam::VibDepth: return "VDepth";
    case SoundParam::VibDelay: return "VDelay";
    case SoundParam::Cutoff: return "Cutoff";
    case SoundParam::Resonance: return "Reso";
    case SoundParam::Attack: return "Attack";
    case SoundParam::Decay: return "Decay";
    case SoundParam::Release: return "Release";
    case SoundParam::HpfCutoff: return "HPF";
    case SoundParam::EqBassGain: return "Bass";
    case SoundParam::EqTrebleGain: return "Treble";
    case SoundParam::EqBassFreq: return "BassF";
    case SoundParam::EqTrebleFreq: return "TrebF";
    default: return "?";
    }
}

int soundParamNrpnLsb(SoundParam p) {
    switch (p) {
    case SoundParam::VibRate: return 0x08;
    case SoundParam::VibDepth: return 0x09;
    case SoundParam::VibDelay: return 0x0A;
    case SoundParam::Cutoff: return 0x20;
    case SoundParam::Resonance: return 0x21;
    case SoundParam::HpfCutoff: return 0x24;
    case SoundParam::EqBassGain: return 0x30;
    case SoundParam::EqTrebleGain: return 0x31;
    case SoundParam::EqBassFreq: return 0x34;
    case SoundParam::EqTrebleFreq: return 0x35;
    case SoundParam::Attack: return 0x63;
    case SoundParam::Decay: return 0x64;
    case SoundParam::Release: return 0x66;
    default: return -1;
    }
}

int soundParamGm2Cc(SoundParam p) {
    switch (p) {
    case SoundParam::Resonance: return 71;
    case SoundParam::Release: return 72;
    case SoundParam::Attack: return 73;
    case SoundParam::Cutoff: return 74;
    case SoundParam::Decay: return 75;
    case SoundParam::VibRate: return 76;
    case SoundParam::VibDepth: return 77;
    case SoundParam::VibDelay: return 78;
    default: return -1;
    }
}

uint8_t soundParamDefault(SoundParam p) {
    if (p == SoundParam::EqBassFreq) return 0x0C;
    if (p == SoundParam::EqTrebleFreq) return 0x36;
    return 64;
}

void ChannelState::clearAll() {
    std::memset(cc, 0, sizeof cc);
    cc[7] = 100;
    cc[10] = 64;
    cc[11] = 127;
    cc[91] = 40;
    for (int i = 0; i < kSoundParamCount; i++) sound[i] = soundParamDefault(SoundParam(i));
    for (int c = 71; c <= 79; c++) cc[c] = 64;
    program = 0;
    bankMsb = bankLsb = 0;
    pitchBend = 8192;
    pressure = 0;
    bendRange = 2;
    coarseTune = 0;
    fineTune = 8192;
    modDepthRange = 0;
    drum = false;
    drumMap = 0;
    keyShift = 0;
    gsToneMap = 0;
    gsToneMap0 = 0;
    efxAssign = false;
    randomPan = false;
    gsEqSwitch = true;
    xgDryLevel = 127;
    rpnMsb = rpnLsb = nrpnMsb = nrpnLsb = 127;
    nrpnSelected = false;
    clearNotes();
}

void ChannelState::resetControllers() {
    cc[1] = 0;
    cc[11] = 127;
    cc[64] = cc[65] = cc[66] = cc[67] = 0;
    pitchBend = 8192;
    pressure = 0;
    rpnMsb = rpnLsb = nrpnMsb = nrpnLsb = 127;
    nrpnSelected = false;
    sostenutoKeys.reset();
    releasePedalNotes();
}

void ChannelState::noteOn(int note, uint8_t velocity) {
    if (!noteVel[note]) activeNotes++;
    if (noteDepth[note] < 255) noteDepth[note]++;
    if (noteSus[note]) {  // struck again while still held by a pedal
        noteSus[note] = 0;
        sustainedNotes--;
    }
    noteVel[note] = velocity;
    lastOnVelocity = velocity;
    noteOnSerial++;
}

void ChannelState::noteOff(int note, bool all) {
    uint8_t v = noteVel[note];
    if (!v) return;
    if (!all && noteDepth[note] > 1) {  // an overlapping note-on of the same key still sounds
        noteDepth[note]--;
        return;
    }
    noteDepth[note] = 0;
    noteVel[note] = 0;
    activeNotes = std::max(0, activeNotes - 1);
    if (cc[64] >= 64 || (cc[66] >= 64 && sostenutoKeys[size_t(note)])) {
        noteSus[note] = v;
        sustainedNotes++;
    }
}

void ChannelState::allNotesOff() {
    for (int n = 0; n < 128 && activeNotes > 0; n++) noteOff(n, true);
}

void ChannelState::releasePedalNotes() {
    for (int n = 0; n < 128 && sustainedNotes > 0; n++) {
        if (!noteSus[n] || cc[64] >= 64 || (cc[66] >= 64 && sostenutoKeys[size_t(n)])) continue;
        noteSus[n] = 0;
        sustainedNotes--;
    }
}

void PortState::reset(MidiStandard m) {
    std::memset(lcdPage, 0, sizeof lcdPage);
    lcdShownPage = 0;
    std::memset(lcdText, 0, sizeof lcdText);
    for (int c = 0; c < 16; c++) {
        ch[c].clearAll();
        ch[c].drum = (c == 9);
        ch[c].drumMap = (c == 9) ? 1 : 0;
    }
    if (m == MidiStandard::XG) {
        ch[9].cc[0] = ch[9].bankMsb = 127;
        ch[9].drumMap = 2;
    } else if (m == MidiStandard::GM2) {
        for (int c = 0; c < 16; c++) ch[c].cc[0] = ch[c].bankMsb = 121;
        ch[9].cc[0] = ch[9].bankMsb = 120;
    }
    mode = m;
    masterVolume = 16383;
    masterKeyShift = 0;
    masterPan = 64;
    masterTune = 0;
    masterTuneRaw = 0x400;
    const uint8_t rev[8] = {4, 4, 0, 64, 64, 0, 0, 0};
    const uint8_t cho[9] = {2, 0, 64, 8, 80, 3, 19, 0, 0};
    const uint8_t dly[11] = {0, 0, 0x61, 1, 1, 127, 0, 0, 64, 0x50, 0};
    const uint8_t eq[4] = {0, 0x40, 0, 0x40};
    std::memcpy(gsReverb, rev, sizeof rev);
    std::memcpy(gsChorus, cho, sizeof cho);
    std::memcpy(gsDelay, dly, sizeof dly);
    std::memcpy(gsEq, eq, sizeof eq);
    gsEfxMsb = gsEfxLsb = 0;
    std::memset(gsEfxParam, 0, sizeof gsEfxParam);
    gsEfxSend[0] = 40;
    gsEfxSend[1] = 0;
    gsEfxSend[2] = 0;
    gsEfxCtrl[0] = 0;
    gsEfxCtrl[1] = 64;
    gsEfxCtrl[2] = 0;
    gsEfxCtrl[3] = 64;
    gsEfxEqSwitch = 1;

    std::memset(xgEffect, 0, sizeof xgEffect);
    xgEffect[0x00] = 0x01;  // Hall 1
    xgEffect[0x0C] = 64;    // reverb return
    xgEffect[0x0D] = 64;    // reverb pan
    xgEffect[0x20] = 0x41;  // Chorus 1
    xgEffect[0x2C] = 64;
    xgEffect[0x2D] = 64;
    xgEffect[0x40] = 0x05;  // Delay L,C,R
    xgEffect[0x56] = 64;
    xgEffect[0x57] = 64;
    xgEffect[0x5A] = 0;     // insertion
    xgEffect[0x5B] = 127;   // part: off
    for (int i = 0x5C; i <= 0x60; i++) xgEffect[i] = 64;
    std::memset(xgEq, 0, sizeof xgEq);
    for (int i : {1, 5, 9, 13, 17}) xgEq[i] = 64;
    std::memset(xgIns, 0, sizeof xgIns);
    xgIns[0][0] = xgIns[1][0] = 0x49;  // Distortion
    xgIns[0][0x0C] = xgIns[1][0x0C] = 127;

    gm2ReverbType = 4;
    gm2ReverbTime = 64;
    gm2ChorusType = 2;
    gm2ChorusRate = 3;
    gm2ChorusDepth = 19;
    gm2ChorusFeedback = 8;
    gm2ChorusSendRev = 0;
    sysexSerial++;
}

void PortState::applyGsEfxDefaults() {
    int t = gsEfxFindType(gsEfxMsb, gsEfxLsb);
    std::memset(gsEfxParam, 0, sizeof gsEfxParam);
    if (t < 0) return;
    const GsEfxTypeDef& def = gsEfxType(t);
    for (int i = 0; i < def.paramCount; i++) {
        const GsEfxParamDef& p = immidi::gsEfxParam(def.firstParam + i);
        if (p.index < 1 || p.index > 20) continue;
        int v = p.defaultValue;
        if (v < 0) {
            if (p.kind == GsEfxKind::LINEAR && p.lo == 0x34 && p.hi == 0x4C) v = 64;  // 0 dB
            else if (p.kind == GsEfxKind::LINEAR && p.hi == 127 && p.lo == 0 && std::string(p.name).find("Level") != std::string::npos) v = 127;
            else if (p.kind == GsEfxKind::ENUM) v = atoi(p.values);
            else v = p.lo;
        }
        gsEfxParam[p.index - 1] = uint8_t(std::clamp(v, 0, 127));
    }
}

void SynthState::resetAll(MidiStandard m) {
    for (auto& p : ports) p.reset(m);
}

void SynthState::resetPort(int port, MidiStandard m) {
    if (port >= 0 && port < kMaxPorts) ports[port].reset(m);
}

void SynthState::clearNotes() {
    for (auto& p : ports)
        for (auto& c : p.ch) c.clearNotes();
}

void SynthState::apply(int port, const uint8_t* msg, size_t len) {
    if (len == 0 || port < 0 || port >= kMaxPorts) return;
    uint8_t s = msg[0];
    if (s >= 0x80 && s < 0xF0) {
        applyChannel(port, s, len > 1 ? msg[1] : 0, len > 2 ? msg[2] : 0);
    } else if (s == 0xF0) {
        applySysex(port, msg, len);
    }
}

void SynthState::applyChannel(int port, uint8_t status, uint8_t d1, uint8_t d2) {
    ChannelState& c = ports[port].ch[status & 0x0F];
    int ch = status & 0x0F;
    switch (status & 0xF0) {
    case 0x80:
        c.noteOff(d1);
        break;
    case 0x90:
        if (d2 == 0) c.noteOff(d1);
        else c.noteOn(d1, d2);
        break;
    case 0xB0: {
        const bool wasDown = c.cc[d1] >= 64, down = d2 >= 64;
        c.cc[d1] = d2;
        switch (d1) {
        case 10:
            c.randomPan = false;
            break;
        case 64:  // sustain (hold 1)
            if (wasDown && !down) c.releasePedalNotes();
            break;
        case 66:  // sostenuto: holds the keys that are down when it is pressed
            if (!wasDown && down) {
                c.sostenutoKeys.reset();
                for (int n = 0; n < 128; n++)
                    if (c.noteVel[n]) c.sostenutoKeys.set(size_t(n));
            } else if (wasDown && !down) {
                c.sostenutoKeys.reset();
                c.releasePedalNotes();
            }
            break;
        case 6:
        case 38:
            dataEntry(port, ch);
            break;
        case 98:
            c.nrpnLsb = d2;
            c.nrpnSelected = true;
            break;
        case 99:
            c.nrpnMsb = d2;
            c.nrpnSelected = true;
            break;
        case 100:
            c.rpnLsb = d2;
            c.nrpnSelected = false;
            break;
        case 101:
            c.rpnMsb = d2;
            c.nrpnSelected = false;
            break;
        case 120:  // all sounds off
        case 126:  // mono and poly also act as all sounds off
        case 127:
            c.clearNotes();
            break;
        case 123:  // all notes off (omni off / on act the same): held notes keep sounding
        case 124:
        case 125:
            c.allNotesOff();
            break;
        case 121:
            c.resetControllers();
            break;
        case 71: c.sound[int(SoundParam::Resonance)] = d2; break;
        case 72: c.sound[int(SoundParam::Release)] = d2; break;
        case 73: c.sound[int(SoundParam::Attack)] = d2; break;
        case 74: c.sound[int(SoundParam::Cutoff)] = d2; break;
        case 75: c.sound[int(SoundParam::Decay)] = d2; break;
        case 76: c.sound[int(SoundParam::VibRate)] = d2; break;
        case 77: c.sound[int(SoundParam::VibDepth)] = d2; break;
        case 78: c.sound[int(SoundParam::VibDelay)] = d2; break;
        default: break;
        }
        break;
    }
    case 0xC0:
        programChange(port, ch, d1);
        break;
    case 0xD0:
        c.pressure = d1;
        break;
    case 0xE0:
        c.pitchBend = uint16_t(d1 | (d2 << 7));
        break;
    default:
        break;
    }
}

void SynthState::programChange(int port, int ch, uint8_t program) {
    ChannelState& c = ports[port].ch[ch];
    c.program = program;
    c.bankMsb = c.cc[0];
    c.bankLsb = c.cc[32];
    switch (effectiveMode(port)) {
    case MidiStandard::XG:
        if (c.bankMsb == 127 || c.bankMsb == 126) {
            c.drum = true;
            if (c.drumMap == 0) c.drumMap = 1;
        } else {
            c.drum = false;
            c.drumMap = 0;
        }
        break;
    case MidiStandard::GM2:
        if (c.bankMsb == 120) {
            c.drum = true;
            c.drumMap = 1;
        } else if (c.bankMsb == 121) {
            c.drum = false;
            c.drumMap = 0;
        }
        break;
    default:
        break;  // GS / GM: drum parts only change via SysEx
    }
}

void SynthState::dataEntry(int port, int ch) {
    ChannelState& c = ports[port].ch[ch];
    uint8_t v = c.cc[6];
    if (c.nrpnSelected) {
        if (c.nrpnMsb != 1) return;  // drum instrument NRPNs are not tracked
        for (int i = 0; i < kSoundParamCount; i++) {
            if (soundParamNrpnLsb(SoundParam(i)) == c.nrpnLsb) {
                c.sound[i] = v;
                return;
            }
        }
        return;
    }
    if (c.rpnMsb != 0) return;
    switch (c.rpnLsb) {
    case 0: c.bendRange = v; break;
    case 1: c.fineTune = uint16_t((v << 7) | c.cc[38]); break;
    case 2: c.coarseTune = int8_t(int(v) - 64); break;
    case 5: c.modDepthRange = v; break;
    default: break;
    }
}

void SynthState::applySysex(int port, const uint8_t* m, size_t n) {
    if (n < 4 || m[0] != 0xF0) return;
    PortState& ps = ports[port];
    // Universal non-realtime
    if (m[1] == 0x7E && n >= 6 && m[3] == 0x09) {
        if (m[4] == 0x01) resetPort(port, MidiStandard::GM);
        else if (m[4] == 0x03) resetPort(port, MidiStandard::GM2);
        else if (m[4] == 0x02) ps.mode = MidiStandard::None;
        return;
    }
    // Universal realtime: device control
    if (m[1] == 0x7F && n >= 7 && m[3] == 0x04) {
        if (m[4] == 0x01 && n >= 8) {
            ps.masterVolume = uint16_t(m[5] | (m[6] << 7));
            ps.sysexSerial++;
        } else if (m[4] == 0x02 && n >= 8) {
            ps.masterPan = m[6];
            ps.sysexSerial++;
        } else if (m[4] == 0x03 && n >= 8) {
            // Master fine tuning: 14 bits, 0x2000 = 0, +-100 cents.
            int v = m[5] | (m[6] << 7);
            ps.masterTune = (v - 0x2000) * 1000 / 0x2000;
            ps.sysexSerial++;
        } else if (m[4] == 0x05 && n >= 10) {
            // Global parameter control
            int sw = m[5], pw = m[6], vw = m[7];
            size_t pos = 8;
            if (sw != 1 || pw != 1 || vw != 1 || pos + 2 > n) return;
            uint8_t s1 = m[pos], s2 = m[pos + 1];
            pos += 2;
            while (pos + 2 < n && m[pos] != 0xF7) {
                uint8_t pp = m[pos], vv = m[pos + 1];
                pos += 2;
                if (s1 == 1 && s2 == 1) {
                    if (pp == 0) ps.gm2ReverbType = vv;
                    else if (pp == 1) ps.gm2ReverbTime = vv;
                } else if (s1 == 1 && s2 == 2) {
                    if (pp == 0) ps.gm2ChorusType = vv;
                    else if (pp == 1) ps.gm2ChorusRate = vv;
                    else if (pp == 2) ps.gm2ChorusDepth = vv;
                    else if (pp == 3) ps.gm2ChorusFeedback = vv;
                    else if (pp == 4) ps.gm2ChorusSendRev = vv;
                }
            }
            ps.sysexSerial++;
        }
        return;
    }
    // Sound Canvas display DT1: F0 41 dev 45 12 10 a2 a3 data... sum F7
    if (m[1] == 0x41 && n >= 10 && m[3] == 0x45 && m[4] == 0x12 && m[5] == 0x10) {
        size_t dataEnd = n - 2;  // checksum, F7
        if (m[n - 1] != 0xF7) dataEnd = n - 1;
        const uint8_t* data = m + 8;
        size_t len = dataEnd > 8 ? dataEnd - 8 : 0;
        uint8_t a2 = m[6], a3 = m[7];
        if (a2 == 0x00) {  // display letters (up to 32 characters)
            if (a3 < 32) {
                for (size_t i = 0; i < len && a3 + i < 32; i++) ps.lcdText[a3 + i] = char(data[i] >= 0x20 && data[i] < 0x7F ? data[i] : ' ');
                if (a3 == 0) ps.lcdText[std::min<size_t>(len, 32)] = '\0';
                ps.lcdTextSerial++;
            }
        } else if (a2 >= 0x01 && a2 <= 0x0A) {
            // Dot data: 64 bytes = four strips of 16 rows, 5 dots per byte (bit 4 = left); the last
            // strip holds only the 16th column.
            uint16_t* page = ps.lcdPage[a2 - 1];
            for (size_t i = 0; i < len && a3 + i < 64; i++) {
                size_t k = a3 + i;
                int strip = int(k / 16), row = int(k % 16);
                for (int b = 0; b < 5; b++) {
                    int col = strip * 5 + (4 - b);
                    if (col > 15) continue;
                    uint16_t bit = uint16_t(1u << (15 - col));
                    if (data[i] & (1 << b)) page[row] |= bit;
                    else page[row] &= uint16_t(~bit);
                }
            }
            if (a2 == 0x01) ps.lcdShownPage = 1;  // page 1 is shown as soon as it is written
            ps.lcdDotsSerial++;
        } else if (a2 == 0x20 && a3 == 0x00 && len >= 1) {  // display page (SC-88 and later)
            ps.lcdShownPage = data[0] <= 10 ? data[0] : 0;
            ps.lcdDotsSerial++;
        }
        return;
    }
    // Roland GS DT1: F0 41 dev 42 12 a1 a2 a3 data... sum F7
    if (m[1] == 0x41 && n >= 10 && m[3] == 0x42 && m[4] == 0x12) {
        size_t dataEnd = n - 2;  // checksum, F7
        if (m[n - 1] != 0xF7) dataEnd = n - 1;
        uint32_t addr = (uint32_t(m[5]) << 14) | (uint32_t(m[6]) << 7) | m[7];
        for (size_t i = 8; i < dataEnd; i++) {
            uint32_t a = addr + uint32_t(i - 8);
            gsWrite(port, uint8_t((a >> 14) & 0x7F), uint8_t((a >> 7) & 0x7F), uint8_t(a & 0x7F), m[i]);
        }
        return;
    }
    // Yamaha XG parameter change: F0 43 1n 4C hi mid lo data... F7
    if (m[1] == 0x43 && n >= 8 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C) {
        uint32_t addr = (uint32_t(m[4]) << 14) | (uint32_t(m[5]) << 7) | m[6];
        size_t dataEnd = (m[n - 1] == 0xF7) ? n - 1 : n;
        for (size_t i = 7; i < dataEnd; i++) {
            uint32_t a = addr + uint32_t(i - 7);
            xgWrite(port, uint8_t((a >> 14) & 0x7F), uint8_t((a >> 7) & 0x7F), uint8_t(a & 0x7F), m[i]);
        }
        return;
    }
}

void SynthState::gsWrite(int port, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t v) {
    int target = port;
    if (a1 == 0x50 || a1 == 0x51) {  // SC-88: addresses of the other part group
        target = port ^ 1;
        a1 = uint8_t(a1 - 0x10);
    }
    if (target >= kMaxPorts) return;
    PortState& ps = ports[target];
    if (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F) {  // system mode set: resets both part groups
        int base = port & ~1;
        resetPort(base, MidiStandard::GS);
        if (base + 1 < kMaxPorts) resetPort(base + 1, MidiStandard::GS);
        return;
    }
    if (a1 != 0x40) return;
    ps.sysexSerial++;
    if (a2 == 0x00) {
        switch (a3) {
        case 0x7F: resetPort(target, MidiStandard::GS); break;
        case 0x04: ps.masterVolume = uint16_t(v << 7); break;
        case 0x05: ps.masterKeyShift = int(v) - 64; break;
        case 0x06: ps.masterPan = v; break;
        case 0x00: case 0x01: case 0x02: case 0x03: {
            // MASTER TUNE: four nibbles, 0x0400 = 0, 0.1 cent steps.
            int shift = (3 - a3) * 4;
            ps.masterTuneRaw = uint16_t((ps.masterTuneRaw & ~(0xF << shift)) | ((v & 0xF) << shift));
            ps.masterTune = int(ps.masterTuneRaw) - 0x400;
            break;
        }
        default: break;
        }
        return;
    }
    if (a2 == 0x01) {
        if (a3 >= 0x30 && a3 <= 0x37) ps.gsReverb[a3 - 0x30] = v;
        else if (a3 >= 0x38 && a3 <= 0x40) ps.gsChorus[a3 - 0x38] = v;
        else if (a3 >= 0x50 && a3 <= 0x5A) ps.gsDelay[a3 - 0x50] = v;
        return;
    }
    if (a2 == 0x02) {
        if (a3 <= 0x03) ps.gsEq[a3] = v;
        return;
    }
    if (a2 == 0x03) {
        if (a3 == 0x00) {
            ps.gsEfxMsb = v;
        } else if (a3 == 0x01) {
            ps.gsEfxLsb = v;
            ps.applyGsEfxDefaults();
        } else if (a3 >= 0x03 && a3 <= 0x16) {
            ps.gsEfxParam[a3 - 0x03] = v;
        } else if (a3 >= 0x17 && a3 <= 0x19) {
            ps.gsEfxSend[a3 - 0x17] = v;
        } else if (a3 >= 0x1B && a3 <= 0x1E) {
            ps.gsEfxCtrl[a3 - 0x1B] = v;
        } else if (a3 == 0x1F) {
            ps.gsEfxEqSwitch = v;
        }
        return;
    }
    if ((a2 & 0xF0) == 0x10) {
        ChannelState& c = ps.ch[gsChannelOfBlock(a2 & 0x0F)];
        int ch = gsChannelOfBlock(a2 & 0x0F);
        switch (a3) {
        case 0x00: c.cc[0] = v; break;
        case 0x01: programChange(target, ch, v); break;
        case 0x15:
            c.drum = v != 0;
            c.drumMap = v;
            break;
        case 0x16: c.keyShift = int8_t(int(v) - 64); break;
        case 0x19: c.cc[7] = v; break;
        case 0x1C:  // part pan: 0 = random, 1..127 = L63..R63
            c.randomPan = v == 0;
            c.cc[10] = v ? v : 64;
            break;
        case 0x21: c.cc[93] = v; break;
        case 0x22: c.cc[91] = v; break;
        case 0x2C: c.cc[94] = v; break;
        case 0x30: c.sound[int(SoundParam::VibRate)] = v; break;
        case 0x31: c.sound[int(SoundParam::VibDepth)] = v; break;
        case 0x32: c.sound[int(SoundParam::Cutoff)] = v; break;
        case 0x33: c.sound[int(SoundParam::Resonance)] = v; break;
        case 0x34: c.sound[int(SoundParam::Attack)] = v; break;
        case 0x35: c.sound[int(SoundParam::Decay)] = v; break;
        case 0x36: c.sound[int(SoundParam::Release)] = v; break;
        case 0x37: c.sound[int(SoundParam::VibDelay)] = v; break;
        default: break;
        }
        return;
    }
    if ((a2 & 0xF0) == 0x40) {
        ChannelState& c = ps.ch[gsChannelOfBlock(a2 & 0x0F)];
        switch (a3) {
        case 0x00: c.gsToneMap = v; break;
        case 0x01: c.gsToneMap0 = v; break;
        case 0x20: c.gsEqSwitch = v != 0; break;
        case 0x22: c.efxAssign = v != 0; break;
        default: break;
        }
    }
}

void SynthState::xgWrite(int port, uint8_t hi, uint8_t mid, uint8_t lo, uint8_t v) {
    PortState& ps = ports[port];
    if (hi == 0x00 && mid == 0x00) {
        switch (lo) {
        case 0x7E:
        case 0x7F:
            // XG System On / All Parameter Reset address all parts of the device.
            for (int p = 0; p < kMaxPorts; p++)
                if (p == port || ports[p].mode == MidiStandard::XG || p < 4) resetPort(p, MidiStandard::XG);
            break;
        case 0x04: ps.masterVolume = uint16_t(v << 7); ps.sysexSerial++; break;
        case 0x06: ps.masterKeyShift = int(v) - 64; ps.sysexSerial++; break;
        case 0x00: case 0x01: case 0x02: case 0x03: {
            // MASTER TUNE: four nibbles, 0x0400 = 0, 0.1 cent steps.
            int shift = (3 - lo) * 4;
            ps.masterTuneRaw = uint16_t((ps.masterTuneRaw & ~(0xF << shift)) | ((v & 0xF) << shift));
            ps.masterTune = int(ps.masterTuneRaw) - 0x400;
            ps.sysexSerial++;
            break;
        }
        default: break;
        }
        return;
    }
    if (hi == 0x02 && mid == 0x01) {
        ps.xgEffect[lo & 0x7F] = v;
        ps.sysexSerial++;
        return;
    }
    if (hi == 0x02 && mid == 0x40) {
        if (lo < sizeof ps.xgEq) ps.xgEq[lo] = v;
        ps.sysexSerial++;
        return;
    }
    if (hi == 0x03 && mid <= 0x01) {
        if (lo < 0x30) ps.xgIns[mid][lo] = v;
        ps.sysexSerial++;
        return;
    }
    if (hi == 0x08 || hi == 0x0A) {
        int part = mid;
        int p = part >> 4;
        if (p >= kMaxPorts) return;
        ChannelState& c = ports[p].ch[part & 0x0F];
        if (hi == 0x0A) {
            if (lo == 0x20) c.sound[int(SoundParam::HpfCutoff)] = v;
            return;
        }
        switch (lo) {
        case 0x01: c.cc[0] = v; break;
        case 0x02: c.cc[32] = v; break;
        case 0x03: programChange(p, part & 0x0F, v); break;
        case 0x07:
            c.drum = v != 0;
            c.drumMap = v;
            break;
        case 0x08: c.keyShift = int8_t(int(v) - 64); break;
        case 0x0B: c.cc[7] = v; break;
        case 0x0E:  // part pan: 0 = random, 1..127 = L63..R63
            c.randomPan = v == 0;
            c.cc[10] = v ? v : 64;
            break;
        case 0x11: c.xgDryLevel = v; break;
        case 0x12: c.cc[93] = v; break;
        case 0x13: c.cc[91] = v; break;
        case 0x14: c.cc[94] = v; break;
        case 0x15: c.sound[int(SoundParam::VibRate)] = v; break;
        case 0x16: c.sound[int(SoundParam::VibDepth)] = v; break;
        case 0x17: c.sound[int(SoundParam::VibDelay)] = v; break;
        case 0x18: c.sound[int(SoundParam::Cutoff)] = v; break;
        case 0x19: c.sound[int(SoundParam::Resonance)] = v; break;
        case 0x1A: c.sound[int(SoundParam::Attack)] = v; break;
        case 0x1B: c.sound[int(SoundParam::Decay)] = v; break;
        case 0x1C: c.sound[int(SoundParam::Release)] = v; break;
        case 0x23: c.bendRange = uint8_t(std::max(0, int(v) - 64)); break;
        case 0x67: c.cc[65] = v ? 127 : 0; break;
        case 0x68: c.cc[5] = v; break;
        case 0x72: c.sound[int(SoundParam::EqBassGain)] = v; break;
        case 0x73: c.sound[int(SoundParam::EqTrebleGain)] = v; break;
        case 0x76: c.sound[int(SoundParam::EqBassFreq)] = v; break;
        case 0x77: c.sound[int(SoundParam::EqTrebleFreq)] = v; break;
        default: break;
        }
    }
}

} // namespace immidi

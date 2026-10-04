#include "FileAnalysis.h"
#include "SynthState.h"

#include <algorithm>
#include <memory>

namespace immidi {

namespace {

// Appends the code point at s[i] (UTF-8) to `out` with full-width ASCII folded to ASCII and letters
// upper-cased. Other non-ASCII characters become a space (Japanese text around a module name, such
// as "対応" or "音源", acts as a delimiter). Returns the byte length consumed.
size_t foldChar(const std::string& s, size_t i, std::string& out) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        out += (c >= 'a' && c <= 'z') ? char(c - 32) : char(c);
        return 1;
    }
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = 0;
    if (len == 3 && i + 2 < s.size()) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
    else if (len == 2 && i + 1 < s.size()) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        char a = char(cp - 0xFF01 + 0x21);
        out += (a >= 'a' && a <= 'z') ? char(a - 32) : a;
    } else if (cp == 0x2212 || cp == 0x30FC || cp == 0x2010) {
        out += '-';  // minus sign / prolonged sound mark / hyphen used as a dash
    } else if (cp == 0x2161) {
        out += "II";  // Roman numeral two (SC-55mkⅡ)
    } else {
        out += ' ';
    }
    return len;
}

// Keyword table after TMIDI Player's module.def [keyword] section, reduced to what ImMidi can use.
// Tokens are compared after folding (upper case, full-width to ASCII).
struct ModuleKeyword {
    const char* token;
    const char* module;
    MidiStandard family;
    DeviceKind device;
};
const ModuleKeyword kModuleKeywords[] = {
    {"SC8850", "SC-8850", MidiStandard::GS, DeviceKind::SC8850},
    {"SC-8850", "SC-8850", MidiStandard::GS, DeviceKind::SC8850},
    {"8850", "SC-8850", MidiStandard::GS, DeviceKind::SC8850},
    {"SC8820", "SC-8820", MidiStandard::GS, DeviceKind::SC8850},
    {"SC-8820", "SC-8820", MidiStandard::GS, DeviceKind::SC8850},
    {"8820", "SC-8820", MidiStandard::GS, DeviceKind::SC8850},
    {"SC88PRO", "SC-88Pro", MidiStandard::GS, DeviceKind::SC88Pro},
    {"SC-88PRO", "SC-88Pro", MidiStandard::GS, DeviceKind::SC88Pro},
    {"88PRO", "SC-88Pro", MidiStandard::GS, DeviceKind::SC88Pro},
    {"SC88", "SC-88", MidiStandard::GS, DeviceKind::SC88},
    {"SC-88", "SC-88", MidiStandard::GS, DeviceKind::SC88},
    {"88MAP", "SC-88", MidiStandard::GS, DeviceKind::SC88},
    {"SC88VL", "SC-88VL", MidiStandard::GS, DeviceKind::SC88},
    {"SC-88VL", "SC-88VL", MidiStandard::GS, DeviceKind::SC88},
    {"88", "SC-88", MidiStandard::GS, DeviceKind::SC88},
    {"55", "SC-55", MidiStandard::GS, DeviceKind::SC55},
    {"SC55", "SC-55", MidiStandard::GS, DeviceKind::SC55},
    {"SC-55", "SC-55", MidiStandard::GS, DeviceKind::SC55},
    {"SC-155", "SC-155", MidiStandard::GS, DeviceKind::SC55},
    {"55MAP", "SC-55", MidiStandard::GS, DeviceKind::SC55},
    {"CM-300", "CM-300", MidiStandard::GS, DeviceKind::SC55},
    {"CM300", "CM-300", MidiStandard::GS, DeviceKind::SC55},
    {"CM-500", "CM-500", MidiStandard::GS, DeviceKind::SC55},
    {"CM500", "CM-500", MidiStandard::GS, DeviceKind::SC55},
    {"SC-55ST", "SC-55ST", MidiStandard::GS, DeviceKind::SC55},
    {"SC55ST", "SC-55ST", MidiStandard::GS, DeviceKind::SC55},
    {"SC-55MK2", "SC-55mkII", MidiStandard::GS, DeviceKind::SC55},
    {"SC-55MKII", "SC-55mkII", MidiStandard::GS, DeviceKind::SC55},
    {"SC55MK2", "SC-55mkII", MidiStandard::GS, DeviceKind::SC55},
    {"SC55MKII", "SC-55mkII", MidiStandard::GS, DeviceKind::SC55},
    {"55MK2", "SC-55mkII", MidiStandard::GS, DeviceKind::SC55},
    {"XG", "XG", MidiStandard::XG, DeviceKind::XG},
    {"MU50", "MU50", MidiStandard::XG, DeviceKind::XG},
    {"MU80", "MU80", MidiStandard::XG, DeviceKind::XG},
    {"MU90", "MU90", MidiStandard::XG, DeviceKind::XG},
    {"MU100", "MU100", MidiStandard::XG, DeviceKind::XG},
    {"MU128", "MU128", MidiStandard::XG, DeviceKind::XG},
    {"MU1000", "MU1000", MidiStandard::XG, DeviceKind::XG},
    {"MU2000", "MU2000", MidiStandard::XG, DeviceKind::XG},
};

// Finds the first known module name in the song's text (TMIDI looks at the first 2048 characters).
const ModuleKeyword* findModuleKeyword(const MidiFile& f) {
    std::string text;
    for (const TextItem& t : f.texts) {
        if (t.type < 1 || t.type > 3) continue;  // text, copyright, track name
        for (size_t i = 0; i < t.text.size() && text.size() < 2048;) i += foldChar(t.text, i, text);
        text += ' ';
        if (text.size() >= 2048) break;
    }
    static const char* const kDelims = " \t\r\n+_/()<>:|?\"#$%&'[],.;!*=~{}@^`";
    size_t i = 0;
    while (i < text.size()) {
        size_t a = text.find_first_not_of(kDelims, i);
        if (a == std::string::npos) break;
        size_t b = text.find_first_of(kDelims, a);
        if (b == std::string::npos) b = text.size();
        std::string tok = text.substr(a, b - a);
        while (!tok.empty() && tok.back() == '-') tok.pop_back();
        for (const ModuleKeyword& k : kModuleKeywords)
            if (tok == k.token) return &k;
        i = b;
    }
    return nullptr;
}

int gsDeviceRank(DeviceKind d) {
    switch (d) {
    case DeviceKind::SC55: return 1;
    case DeviceKind::SC88: return 2;
    case DeviceKind::SC88Pro: return 3;
    case DeviceKind::SC8850: return 4;
    default: return 0;
    }
}

} // namespace

FileInfo analyzeFile(const MidiFile& f) {
    FileInfo info;
    bool xgBank = false, gm2Bank = false;
    for (const MidiEvent& e : f.events) {
        if (e.status == 0xF0) {
            const uint8_t* m = f.payload(e);
            size_t n = f.payloadLen(e);
            if (n >= 6 && m[1] == 0x7E && m[3] == 0x09) {
                if (m[4] == 0x01) info.hasGmOn = true;
                if (m[4] == 0x03) info.hasGm2On = true;
            } else if (n >= 10 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12) {
                info.hasGsSysex = true;
                uint8_t a1 = m[5], a2 = m[6], a3 = m[7];
                if ((a1 == 0x40 || a1 == 0x50) && a2 == 0x00 && a3 == 0x7F) info.hasGsReset = true;
                if (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F) info.hasModeSet = true;
                if ((a1 == 0x40 || a1 == 0x50) && a2 == 0x03) info.usesGsEfx = true;
                if ((a1 == 0x40 || a1 == 0x50) && (a2 & 0xF0) == 0x40 && a3 == 0x22 && m[8]) info.usesGsEfx = true;
                if ((a1 == 0x40 || a1 == 0x50) && a2 == 0x01 && a3 >= 0x50 && a3 <= 0x5A) info.usesGsDelay = true;
                if ((a1 == 0x40 || a1 == 0x50) && (a2 & 0xF0) == 0x40 && a3 == 0x00) {
                    info.usesGsToneMap = true;
                    if (m[8] <= 4) info.maxGsMap = std::max<int>(info.maxGsMap, m[8]);
                }
            } else if (n >= 8 && m[1] == 0x43 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C) {
                info.hasXgSysex = true;
                if (m[4] == 0 && m[5] == 0 && (m[6] == 0x7E || m[6] == 0x7F)) info.hasXgOn = true;
            }
        } else if (e.isChannel() && e.type() == 0xB0) {
            if (e.d1 == 0 && (e.d2 == 126 || e.d2 == 127) && e.channel() != 9) xgBank = true;
            if (e.d1 == 0 && e.d2 == 64) xgBank = true;
            if (e.d1 == 0 && (e.d2 == 120 || e.d2 == 121)) gm2Bank = true;
            if (e.d1 == 32 && e.d2 >= 1 && e.d2 <= 4) info.maxGsMap = std::max<int>(info.maxGsMap, e.d2);
        }
    }
    if (info.hasXgOn || info.hasXgSysex) info.standard = MidiStandard::XG;
    else if (info.hasGsReset || info.hasGsSysex || info.hasModeSet) info.standard = MidiStandard::GS;
    else if (info.hasGm2On || gm2Bank) info.standard = MidiStandard::GM2;
    else if (xgBank) info.standard = MidiStandard::XG;
    else info.standard = MidiStandard::GM;

    switch (info.standard) {
    case MidiStandard::XG: info.suggestedDevice = DeviceKind::XG; break;
    case MidiStandard::GM2: info.suggestedDevice = DeviceKind::GM2; break;
    case MidiStandard::GS: {
        // Hard evidence: a module can't play features it doesn't have.
        DeviceKind least = DeviceKind::SC55;
        if (info.maxGsMap >= 4) least = DeviceKind::SC8850;
        else if (info.usesGsEfx || info.maxGsMap == 3) least = DeviceKind::SC88Pro;
        else if (info.usesGsDelay || info.maxGsMap == 2) least = DeviceKind::SC88;
        // Weaker hints of a newer module.
        DeviceKind guess = least;
        if (least == DeviceKind::SC55 && info.usesGsToneMap) guess = DeviceKind::SC88Pro;
        else if (least == DeviceKind::SC55 && (info.hasModeSet || f.numPorts > 1)) guess = DeviceKind::SC88;
        info.suggestedDevice = guess;
        break;
    }
    default: info.suggestedDevice = DeviceKind::GM; break;
    }

    // A module named in the song's text (as TMIDI Player does) replaces the weak hints, but never
    // goes below what the song's messages require.
    if (const ModuleKeyword* k = findModuleKeyword(f)) {
        info.moduleKeyword = k->module;
        if (info.standard == MidiStandard::GM && (k->family == MidiStandard::GS || k->family == MidiStandard::XG)) {
            // Nothing in the messages says more than GM (no reset): the named module tells.
            info.standard = k->family;
            info.suggestedDevice = k->device;
        } else if (info.standard == MidiStandard::GS && k->family == MidiStandard::GS) {
            DeviceKind least = info.maxGsMap >= 4 ? DeviceKind::SC8850
                               : (info.usesGsEfx || info.maxGsMap == 3) ? DeviceKind::SC88Pro
                               : (info.usesGsDelay || info.maxGsMap == 2) ? DeviceKind::SC88
                                                                          : DeviceKind::SC55;
            info.suggestedDevice = gsDeviceRank(k->device) >= gsDeviceRank(least) ? k->device : least;
        }
    }

    // Simulate the whole song to find every channel that acts as a drum part.
    auto st = std::make_unique<SynthState>();
    st->assumedStandard = info.standard;
    st->resetAll(MidiStandard::None);
    uint8_t buf[3];
    for (const MidiEvent& e : f.events) {
        if (e.status == 0xF0) {
            st->apply(e.port, f.payload(e), f.payloadLen(e));
        } else if (e.isChannel()) {
            uint8_t t = e.type();
            if (t != 0xB0 && t != 0xC0) {
                if (t == 0x90) {
                    const ChannelState& c = st->ports[e.port].ch[e.channel()];
                    if (c.drum) info.drumChannels[e.port] |= uint16_t(1u << e.channel());
                }
                continue;
            }
            buf[0] = e.status;
            buf[1] = e.d1;
            buf[2] = e.d2;
            st->apply(e.port, buf, t == 0xC0 ? 2 : 3);
        }
    }
    info.detectedStandard = info.standard;
    info.detectedDevice = info.suggestedDevice;
    return info;
}

std::string FileInfo::summary() const {
    std::string s = standardName(moduleChosen ? detectedStandard : standard);
    if (hasGsReset) s += " (GS Reset)";
    if (hasXgOn) s += " (XG On)";
    if (hasGm2On) s += " (GM2 On)";
    if (hasGmOn && !hasGsReset && !hasXgOn) s += " (GM On)";
    if (moduleChosen) s = std::string(deviceShortName(suggestedDevice)) + " (set by you; detected " + s + ")";
    return s;
}

FileInfo withSongModule(const FileInfo& detected, DeviceKind module) {
    FileInfo f = detected;
    if (!detected.moduleChosen) {
        f.detectedStandard = detected.standard;
        f.detectedDevice = detected.suggestedDevice;
    }
    f.moduleChosen = true;
    f.standard = deviceProfile(module).family;
    f.suggestedDevice = deviceProfile(module).kind;
    return f;
}

} // namespace immidi

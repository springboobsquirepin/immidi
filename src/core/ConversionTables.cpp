#include "ConversionTables.h"
#include "Json.h"
#include "Util.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace immidi {

ConvNoteMap::ConvNoteMap() {
    for (int i = 0; i < 128; i++) {
        note[i] = -1;
        velocity[i] = 0;
    }
}

int ConvRule::apply(int v) const {
    if (!set) return v;
    int r = v * scale / 100 + offset;
    return std::clamp(r, 0, 127);
}

std::string ConvSetup::describe() const {
    switch (direction) {
    case ConvDirection::GsToXg: return "gs-to-xg.json";
    case ConvDirection::XgToGs:
        return std::string("xg-to-gs.json (") +
               (targetSc55 ? "SC-55" : targetMaxMap >= 4 ? "SC-8850 map" : targetMaxMap >= 3 ? "SC-88Pro map" : "SC-88") + ")";
    case ConvDirection::GsMaps:
        return sc55MapUp ? "SC-55 map for SC-55 sounds" : std::string("gs-maps.json (") + (targetSc55 ? "SC-55" : "SC-88") + ")";
    default: return {};
    }
}

namespace {

int deviceLevel(DeviceKind d) {
    switch (d) {
    case DeviceKind::SC55: return 1;
    case DeviceKind::SC88: return 2;
    case DeviceKind::SC88Pro: return 3;
    case DeviceKind::SC8850: return 4;
    default: return 0;
    }
}

const char* levelKey(int level) { return level <= 1 ? "sc55" : level == 2 ? "sc88" : "sc88pro"; }

// Reading entries: every value is checked, so a mistyped table names the entry instead of
// converting to the wrong sound.
struct Reader {
    std::string file;
    std::string error;

    bool fail(const std::string& where, const std::string& what) {
        if (error.empty()) error = file + ", " + where + ": " + what;
        return false;
    }
    bool num(const JsonValue& v, const char* name, int lo, int hi, int& out, const std::string& where) {
        const JsonValue& x = v[name];
        if (!x.isNumber() || x.number() != double(x.integer()) || x.integer() < lo || x.integer() > hi)
            return fail(where, std::string("\"") + name + "\" must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
        out = x.integer();
        return true;
    }
    bool optNum(const JsonValue& v, const char* name, int lo, int hi, int& out, const std::string& where) {
        if (!v.has(name)) return true;
        return num(v, name, lo, hi, out, where);
    }
    // GS sound {"map", "bank", "pc"} or XG sound {"msb", "lsb", "pc"}; pc 1-128 becomes the program 0-127.
    bool gsVoice(const JsonValue& v, int& map, int& bank, int& prog, const std::string& where) {
        if (!v.isObject()) return fail(where, "expected a GS sound");
        if (!num(v, "map", 1, 4, map, where) || !num(v, "bank", 0, 127, bank, where) || !num(v, "pc", 1, 128, prog, where)) return false;
        prog--;
        return true;
    }
    bool xgVoice(const JsonValue& v, int& msb, int& lsb, int& prog, const std::string& where) {
        if (!v.isObject()) return fail(where, "expected an XG sound");
        if (!num(v, "msb", 0, 127, msb, where) || !num(v, "lsb", 0, 127, lsb, where) || !num(v, "pc", 1, 128, prog, where)) return false;
        prog--;
        return true;
    }
    bool gsKit(const JsonValue& v, int& map, int& prog, const std::string& where) {
        if (!v.isObject()) return fail(where, "expected a GS drum set");
        if (!num(v, "map", 1, 4, map, where) || !num(v, "pc", 1, 128, prog, where)) return false;
        prog--;
        return true;
    }
    bool xgKit(const JsonValue& v, int& msb, int& prog, const std::string& where) {
        if (!v.isObject()) return fail(where, "expected an XG drum kit");
        if (!num(v, "msb", 126, 127, msb, where) || !num(v, "pc", 1, 128, prog, where)) return false;
        prog--;
        return true;
    }
    bool volume(const JsonValue& v, ConvTarget& t, const std::string& where) {
        int vol = 0, attack = 0;
        if (!optNum(v, "volume", -127, 127, vol, where) || !optNum(v, "attack", -64, 63, attack, where)) return false;
        t.volume = int8_t(vol);
        t.attack = int8_t(attack);
        return true;
    }
};

} // namespace

ConvSetup ConversionTables::setupFor(MidiStandard source, DeviceKind sourceDevice, DeviceKind target, int forcedToneMap) {
    ConvSetup s;
    const DeviceProfile& t = deviceProfile(target);
    if (source == MidiStandard::GS) {
        int sl = std::max(1, deviceLevel(sourceDevice));
        if (t.family == MidiStandard::XG) {
            s.direction = ConvDirection::GsToXg;
            s.sourceMap = sl;
            s.rulesKey = levelKey(std::min(sl, 3));
        } else if (t.family == MidiStandard::GS) {
            int tl = std::max(1, deviceLevel(target));
            s.sourceMap = sl;
            s.targetMaxMap = std::max(1, t.gsMaxMap);
            s.targetSc55 = target == DeviceKind::SC55;
            if (sl > s.targetMaxMap) {
                s.direction = ConvDirection::GsMaps;
                s.rulesKey = std::string(levelKey(std::min(sl, 3))) + "-" + levelKey(tl);
            } else if (sl == 1 && tl > 1) {
                // An SC-55 song on a later module: its sounds come from the SC-55 map, as the
                // module's own SC-55 map button would select them.
                s.direction = ConvDirection::GsMaps;
                s.sc55MapUp = true;
                s.rulesKey = std::string("sc55-") + levelKey(std::min(tl, 3));
            } else {
                // A song using maps the module has: explicit higher maps (CC#32) still get converted.
                s.direction = s.targetMaxMap < 3 ? ConvDirection::GsMaps : ConvDirection::None;
            }
        }
    } else if (source == MidiStandard::XG && t.family == MidiStandard::GS) {
        s.direction = ConvDirection::XgToGs;
        s.targetMaxMap = std::max(1, t.gsMaxMap);
        // A forced tone map plays every part with that map: its sounds are the ones to use.
        if (forcedToneMap > 0 && forcedToneMap < s.targetMaxMap) s.targetMaxMap = forcedToneMap;
        s.targetSc55 = target == DeviceKind::SC55 || s.targetMaxMap == 1;
        s.rulesKey = target == DeviceKind::SC55 ? "sc55" : "sc88";
    }
    return s;
}

std::string ConversionTables::userFolder() { return pathToUtf8(pathFromUtf8(configDirectory()) / "conversion"); }

std::string ConversionTables::defaultFolder() {
    for (const std::string& d : {userFolder(), resourceDirectory() + "/conversion", std::string("conversion")}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(pathFromUtf8(d) / kFiles[0], ec))
            return pathToUtf8(std::filesystem::absolute(pathFromUtf8(d), ec).lexically_normal());
    }
    return {};
}

bool ConversionTables::load(const std::string& dir, std::string& error) {
    std::string texts[3];
    for (int f = 0; f < 3; f++) {
        std::vector<uint8_t> bytes;
        std::string path = pathToUtf8(pathFromUtf8(dir) / pathFromUtf8(kFiles[f]));
        if (!readWholeFile(path, bytes)) {
            error = std::string(kFiles[f]) + ": cannot read the file";
            return false;
        }
        texts[f].assign(bytes.begin(), bytes.end());
    }
    if (!loadTexts(texts, error)) return false;
    folder_ = dir;
    return true;
}

bool ConversionTables::loadTexts(const std::string (&texts)[3], std::string& error) {
    folder_.clear();
    gsXgVoices_.clear();
    gsXgKits_.clear();
    xgGsVoices_.clear();
    xgGsKits_.clear();
    mapVoices_.clear();
    mapKits_.clear();
    noteMaps_.clear();
    for (auto& r : rules_) r.clear();

    for (int f = 0; f < 3; f++) {
        Reader rd;
        rd.file = kFiles[f];
        JsonValue doc;
        std::string perr;
        if (!parseJson(texts[f], doc, perr)) {
            error = std::string(kFiles[f]) + ", " + perr;
            return false;
        }
        if (doc["format"].string() != "immidi-conversion" || doc["version"].integer() != 1) {
            error = std::string(kFiles[f]) + ": not an ImMidi conversion table (version 1)";
            return false;
        }
        // Note maps first: drum kits refer to them by name.
        for (auto& [name, list] : doc["noteMaps"].items()) {
            auto nm = std::make_unique<ConvNoteMap>();
            for (size_t i = 0; i < list.size(); i++) {
                const JsonValue& e = list[i];
                std::string where = "note map \"" + name + "\" entry " + std::to_string(i + 1);
                int note = 0, through = 0, to = -2, vel = 0;
                if (!rd.num(e, "note", 0, 127, note, where)) break;
                through = note;  // "through": the same for the notes up to this one
                if (!rd.optNum(e, "through", note, 127, through, where)) break;
                if (!e["to"].isNull() && !rd.num(e, "to", 0, 127, to, where)) break;
                if (!rd.optNum(e, "velocity", -127, 127, vel, where)) break;
                for (int n = note; n <= through; n++) {
                    nm->note[n] = int16_t(to);
                    nm->velocity[n] = int8_t(vel);
                }
            }
            noteMaps_[std::string(kFiles[f]) + "/" + name] = std::move(nm);
        }
        auto notesOf = [&](const JsonValue& t, ConvTarget& out, const std::string& where) {
            if (!t.has("notes")) return true;
            auto it = noteMaps_.find(std::string(kFiles[f]) + "/" + t["notes"].string());
            if (it == noteMaps_.end()) return rd.fail(where, "unknown note map \"" + t["notes"].string() + "\"");
            out.notes = it->second.get();
            return true;
        };
        // GS targets of xg-to-gs / gs-maps entries: {"sc8850": {...}, "sc88pro": {...}, "sc88": {...}, "sc55": {...}}.
        auto pairOf = [&](const JsonValue& e, bool kit, Pair& p, const std::string& where) {
            static const char* const tags[4] = {"sc8850", "sc88pro", "sc88", "sc55"};
            static const int maxMap[4] = {4, 3, 2, 1};
            static const char* const maps[4] = {"1, 2, 3 or 4", "1, 2 or 3", "1 or 2", "1"};
            ConvTarget* out[4] = {&p.sc8850, &p.sc88pro, &p.sc88, &p.sc55};
            bool* has[4] = {&p.hasSc8850, &p.hasSc88pro, &p.hasSc88, &p.hasSc55};
            for (int i = 0; i < 4; i++) {
                const JsonValue& t = e[tags[i]];
                if (t.isNull()) continue;
                ConvTarget ct;
                int map = 0, bank = 0, prog = 0;
                if (kit ? !rd.gsKit(t, map, prog, where) : !rd.gsVoice(t, map, bank, prog, where)) return false;
                if (map > maxMap[i]) return rd.fail(where, std::string("the ") + tags[i] + " sound must be on map " + maps[i]);
                ct.msb = uint8_t(bank);
                ct.lsb = uint8_t(map);
                ct.program = uint8_t(prog);
                if (!rd.volume(t, ct, where) || (kit && !notesOf(t, ct, where))) return false;
                *out[i] = ct;
                *has[i] = true;
            }
            if (!p.hasSc8850 && !p.hasSc88pro && !p.hasSc88 && !p.hasSc55)
                return rd.fail(where, "no \"sc8850\", \"sc88pro\", \"sc88\" or \"sc55\" sound");
            return true;
        };
        const JsonValue& voices = doc["voices"];
        for (size_t i = 0; i < voices.size() && rd.error.empty(); i++) {
            const JsonValue& e = voices[i];
            std::string where = "voice " + std::to_string(i + 1);
            int a = 0, b = 0, prog = 0;
            if (f == 1) {
                if (!rd.xgVoice(e["from"], a, b, prog, where)) break;
                Pair p;
                if (pairOf(e, false, p, where)) xgGsVoices_[key(a, b, prog)] = p;
            } else {
                if (!rd.gsVoice(e["from"], b, a, prog, where)) break;  // b = map, a = bank
                if (f == 0) {
                    ConvTarget t;
                    int msb = 0, lsb = 0, tp = 0;
                    if (!rd.xgVoice(e["to"], msb, lsb, tp, where) || !rd.volume(e, t, where)) break;
                    t.msb = uint8_t(msb);
                    t.lsb = uint8_t(lsb);
                    t.program = uint8_t(tp);
                    gsXgVoices_[key(b, a, prog)] = t;
                } else {
                    Pair p;
                    if (pairOf(e, false, p, where)) mapVoices_[key(b, a, prog)] = p;
                }
            }
        }
        const JsonValue& kits = doc["drumKits"];
        for (size_t i = 0; i < kits.size() && rd.error.empty(); i++) {
            const JsonValue& e = kits[i];
            std::string where = "drum kit " + std::to_string(i + 1);
            int a = 0, prog = 0;
            if (f == 1) {
                if (!rd.xgKit(e["from"], a, prog, where)) break;
                Pair p;
                if (pairOf(e, true, p, where)) xgGsKits_[key(a, 0, prog)] = p;
            } else {
                if (!rd.gsKit(e["from"], a, prog, where)) break;  // a = map
                if (f == 0) {
                    ConvTarget t;
                    int msb = 0, tp = 0;
                    if (!rd.xgKit(e["to"], msb, tp, where) || !rd.volume(e, t, where) || !notesOf(e, t, where)) break;
                    t.msb = uint8_t(msb);
                    t.program = uint8_t(tp);
                    gsXgKits_[key(a, 0, prog)] = t;
                } else {
                    Pair p;
                    if (pairOf(e, true, p, where)) mapKits_[key(a, 0, prog)] = p;
                }
            }
        }
        for (auto& [name, r] : doc["rules"].items()) {
            ConvRules& out = rules_[f][name];
            std::string where = "rules \"" + name + "\"";
            for (auto& [_, c] : r["controllers"].items()) {
                int cc = 0, scale = 100, offset = 0;
                if (!rd.num(c, "cc", 0, 127, cc, where) || !rd.num(c, "scale", 0, 1000, scale, where) ||
                    !rd.optNum(c, "offset", -127, 127, offset, where))
                    break;
                out.cc[cc] = {true, scale, offset};
            }
            for (auto& [_, n] : r["nrpn"].items()) {
                int msb = 0, lsb = 0, scale = 100, offset = 0;
                if (!rd.num(n, "msb", 0, 127, msb, where) || !rd.num(n, "lsb", 0, 127, lsb, where) ||
                    !rd.num(n, "scale", 0, 1000, scale, where) || !rd.optNum(n, "offset", -127, 127, offset, where))
                    break;
                out.nrpn[uint16_t((msb << 7) | lsb)] = {true, scale, offset};
            }
            if (r.has("velocity")) {
                int scale = 100, offset = 0;
                if (rd.num(r["velocity"], "scale", 0, 1000, scale, where) && rd.optNum(r["velocity"], "offset", -127, 127, offset, where))
                    out.velocity = {true, scale, offset};
            }
        }
        if (!rd.error.empty()) {
            error = rd.error;
            return false;
        }
    }
    error.clear();
    return true;
}

const ConvTarget* ConversionTables::Pair::pick(const ConvSetup& s) const {
    if (s.targetSc55) {
        if (hasSc55) return &sc55;
        return hasSc88 && sc88.lsb == 1 ? &sc88 : nullptr;
    }
    // The SC-8850 plays its own (SC-8850 map) sounds where the tables have them, the SC-88Pro (and an
    // SC-8850 playing the SC-88Pro map) the SC-88Pro map's. A module plays the sounds of itself and of the
    // older modules only: an "sc88pro" sound on the SC-88 map is still the SC-88Pro's choice.
    if (hasSc8850 && s.targetMaxMap >= 4) return &sc8850;
    if (hasSc88pro && s.targetMaxMap >= 3 && sc88pro.lsb <= s.targetMaxMap) return &sc88pro;
    if (hasSc88 && sc88.lsb <= s.targetMaxMap) return &sc88;
    return hasSc55 ? &sc55 : nullptr;
}

bool ConversionTables::gsVoiceExists(int map, int variation, int program) const {
    return gsXgVoices_.count(key(map, variation, program)) != 0;
}

bool ConversionTables::gsKitExists(int map, int program) const {
    return gsXgKits_.count(key(map, 0, program)) != 0;
}

const ConvTarget* ConversionTables::gsToXgVoice(const ConvSetup& s, int bank, int map, int program) const {
    // CC#32 = 0: the song's own map. A sound missing there may come from another map (songs for an
    // SC-88 are often taken for SC-55 songs); an explicit map stays, as on the module.
    bool explicitMap = map != 0;
    int m = explicitMap ? map : s.sourceMap;
    static const int others[5][3] = {{0, 0, 0}, {2, 3, 0}, {3, 1, 0}, {2, 1, 0}, {3, 2, 1}};
    auto find = [&](int mm, int b) -> const ConvTarget* {
        auto it = gsXgVoices_.find(key(mm, b, program));
        return it == gsXgVoices_.end() ? nullptr : &it->second;
    };
    if (m < 1 || m > 4) m = s.sourceMap;
    if (const ConvTarget* t = find(m, bank)) return t;
    if (!explicitMap || m == 4)  // no table has the SC-8850 map: use its predecessors'
        for (int mm : others[m])
            if (mm)
                if (const ConvTarget* t = find(mm, bank)) return t;
    // A variation the module does not have: its sub-capital or capital tone.
    int cm = m == 4 ? 3 : m;
    if (bank < 126) {
        if (const ConvTarget* t = find(cm, bank & ~7)) return t;
        if (const ConvTarget* t = find(cm, 0)) return t;
    }
    return nullptr;
}

const ConvTarget* ConversionTables::gsMapsVoice(const ConvSetup& s, int bank, int map, int program) const {
    int m = map ? map : s.sourceMap;
    if (m <= s.targetMaxMap) {
        // The module has the map. A sound missing from the song's own map (songs for an SC-88 are
        // often taken for SC-55 songs) uses the conversion of the later map that has it.
        if (map || bank >= 126 || gsVoiceExists(m, bank, program)) return nullptr;
        for (int mm = s.targetMaxMap + 1; mm <= 3; mm++) {
            auto it = mapVoices_.find(key(mm, bank, program));
            if (it != mapVoices_.end()) return it->second.pick(s);
        }
        return nullptr;
    }
    for (int mm = m; mm > s.targetMaxMap; mm--) {
        auto it = mapVoices_.find(key(mm, bank, program));
        if (it != mapVoices_.end()) return it->second.pick(s);
        if (mm < 4) break;  // only the SC-8850 map borrows the SC-88Pro map's entries
    }
    int cm = std::min(m, 3);
    for (int b : {bank & ~7, 0}) {
        auto it = mapVoices_.find(key(cm, b, program));
        if (it != mapVoices_.end()) return it->second.pick(s);
    }
    return nullptr;
}

const ConvTarget* ConversionTables::voice(const ConvSetup& s, int msb, int lsb, int program) const {
    switch (s.direction) {
    case ConvDirection::GsToXg: return gsToXgVoice(s, msb, lsb, program);
    case ConvDirection::XgToGs: {
        auto it = xgGsVoices_.find(key(msb, lsb, program));
        if (it == xgGsVoices_.end() && lsb != 0) it = xgGsVoices_.find(key(msb, 0, program));  // XG plays the bank 0 sound
        return it == xgGsVoices_.end() ? nullptr : it->second.pick(s);
    }
    case ConvDirection::GsMaps:
        if (s.sc55MapUp) return nullptr;
        return gsMapsVoice(s, msb, lsb, program);
    default: return nullptr;
    }
}

const ConvTarget* ConversionTables::drumKit(const ConvSetup& s, int msb, int lsb, int program) const {
    switch (s.direction) {
    case ConvDirection::GsToXg: {
        bool explicitMap = lsb != 0;
        int m = explicitMap ? lsb : s.sourceMap;
        // The SC-8850 map: its own entries (as for voices), else the SC-88Pro map's and older.
        static const int order[5][4] = {{0, 0, 0, 0}, {1, 2, 3, 0}, {2, 3, 1, 0}, {3, 2, 1, 0}, {4, 3, 2, 1}};
        for (int mm : order[std::clamp(m, 1, 4)]) {
            if (!mm || (explicitMap && mm != m && m != 4)) continue;
            auto it = gsXgKits_.find(key(mm, 0, program));
            if (it != gsXgKits_.end()) return &it->second;
        }
        return nullptr;
    }
    case ConvDirection::XgToGs: {
        auto it = xgGsKits_.find(key(msb, 0, program));
        return it == xgGsKits_.end() ? nullptr : it->second.pick(s);
    }
    case ConvDirection::GsMaps: {
        if (s.sc55MapUp) return nullptr;
        int m = lsb ? lsb : s.sourceMap;
        if (m <= s.targetMaxMap) {
            if (lsb || gsKitExists(m, program)) return nullptr;
            for (int mm = s.targetMaxMap + 1; mm <= 3; mm++) {  // as for voices
                auto it = mapKits_.find(key(mm, 0, program));
                if (it != mapKits_.end()) return it->second.pick(s);
            }
            return nullptr;
        }
        // As for voices: the SC-8850 map's own entries, else the SC-88Pro map's.
        auto it = mapKits_.find(key(std::min(m, 4), 0, program));
        if (it == mapKits_.end() && m >= 4) it = mapKits_.find(key(3, 0, program));
        return it == mapKits_.end() ? nullptr : it->second.pick(s);
    }
    default: return nullptr;
    }
}

const ConvRules* ConversionTables::rules(const ConvSetup& s) const {
    int f = s.direction == ConvDirection::GsToXg ? 0 : s.direction == ConvDirection::XgToGs ? 1 : s.direction == ConvDirection::GsMaps ? 2 : -1;
    if (f < 0 || s.rulesKey.empty()) return nullptr;
    auto it = rules_[f].find(s.rulesKey);
    return it == rules_[f].end() ? nullptr : &it->second;
}

} // namespace immidi

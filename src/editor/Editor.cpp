#include "Editor.h"
#include "ConversionTables.h"
#include "Util.h"
#include "Widgets.h"

#include "imgui.h"
#include "imgui_internal.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <tuple>

namespace immidi {

namespace {

using Clock = std::chrono::steady_clock;

// Files in ConversionTables::kFiles order: gs-to-xg, xg-to-gs, gs-maps.
const char* const kDocTitle[3] = {"Sound Canvas to XG", "XG to Sound Canvas", "Sound Canvas maps"};
const char* const kDocWhat[3] = {
    "GS songs on XG modules: the XG sound and drum kit for each Sound Canvas sound and drum set (SC-55, SC-88 and SC-88Pro maps).",
    "XG songs on Sound Canvases: for each XG sound and drum kit, the sound that plays it on the SC-8850, SC-88Pro, SC-88 and SC-55.",
    "Songs for a newer Sound Canvas on an older one: for SC-88Pro and SC-88 map sounds, the sound that plays them on the SC-88 and SC-55.",
};
const int kDocOrder[3] = {1, 0, 2};  // tabs: XG to Sound Canvas first

struct Slot {
    const char* key;
    const char* label;
    int maxMap;
};
const Slot kSlots[4] = {{"sc8850", "SC-8850", 4}, {"sc88pro", "SC-88Pro", 3}, {"sc88", "SC-88", 2}, {"sc55", "SC-55", 1}};
const char* const kMapNames[5] = {"-", "SC-55", "SC-88", "SC-88Pro", "SC-8850"};

const std::initializer_list<const char*> kGsOrder = {"map", "bank", "pc", "name", "volume", "attack", "notes", "note"};
const std::initializer_list<const char*> kXgOrder = {"msb", "lsb", "pc", "name", "volume", "attack", "notes", "note"};
const std::initializer_list<const char*> kEntryOrder = {"from", "to", "sc88", "sc88pro", "sc55", "sc8850", "volume", "attack", "notes", "note"};
const std::initializer_list<const char*> kNoteOrder = {"note", "through", "to", "velocity", "name"};
const std::initializer_list<const char*> kRuleOrder = {"cc", "msb", "lsb", "scale", "offset", "name", "note"};

int geti(const JsonValue& o, const char* k, int def = 0) { return o.has(k) ? o[k].integer(def) : def; }
void seti(JsonValue& o, const char* k, int v, std::initializer_list<const char*> order) { o.set(k, JsonValue::makeNumber(v), order); }
void sets(JsonValue& o, const char* k, const std::string& v, std::initializer_list<const char*> order) { o.set(k, JsonValue::makeString(v), order); }

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string noteLabel(int n) {
    static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

bool numeric(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }

// Text fields edit a copy: while a field is active, ImGui keeps its own text. A text too long for the
// field shows in full in a tooltip.
bool inputString(const char* label, std::string& s, float width = -FLT_MIN) {
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputText(label, buf, sizeof buf);
    if (changed) s = buf;
    if (!ImGui::IsItemActive() && ImGui::CalcTextSize(s.c_str()).x > ImGui::GetItemRectSize().x - ImGui::GetStyle().FramePadding.x * 2 &&
        ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(s.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return changed;
}

// A field's label before it: the forms read left to right.
void fieldLabel(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine();
}

// A comment: one paragraph, wrapped to the field and as tall as its text (up to four lines). Enter ends
// the edit.
bool noteField(const char* id, std::string& s) {
    static char buf[4096];
    snprintf(buf, sizeof buf, "%s", s.c_str());
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = ImGui::GetContentRegionAvail().x;
    const float line = ImGui::GetTextLineHeight();
    float textH = ImGui::CalcTextSize(buf, nullptr, false, std::max(1.0f, w - st.FramePadding.x * 2 - st.ScrollbarSize)).y;
    ImVec2 size(w, std::clamp(textH, line, line * 4) + st.FramePadding.y * 2);
    auto oneLine = [](ImGuiInputTextCallbackData* data) { return data->EventChar == '\n' || data->EventChar == '\r' ? 1 : 0; };
    bool changed = ImGui::InputTextMultiline(id, buf, sizeof buf, size,
                                             ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CtrlEnterForNewLine |
                                                 ImGuiInputTextFlags_CallbackCharFilter,
                                             oneLine);
    if (changed) s = buf;
    return changed;
}

const ImVec4 kWarn(1.0f, 0.75f, 0.35f, 1.0f);
const ImVec4 kBad(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kGood(0.55f, 0.9f, 0.55f, 1.0f);

// The order the tables keep their entries in (new entries go to their place).
int compareSource(int file, bool kit, const JsonValue& a, const JsonValue& b) {
    auto cmp = [](std::initializer_list<int> x, std::initializer_list<int> y) {
        return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end()) ? -1
               : std::lexicographical_compare(y.begin(), y.end(), x.begin(), x.end()) ? 1
                                                                                      : 0;
    };
    if (file == 1) {
        if (kit) return cmp({-geti(a, "msb"), geti(a, "pc")}, {-geti(b, "msb"), geti(b, "pc")});
        return cmp({geti(a, "msb"), geti(a, "pc"), geti(a, "lsb")}, {geti(b, "msb"), geti(b, "pc"), geti(b, "lsb")});
    }
    int sign = file == 2 ? -1 : 1;  // gs-maps lists the SC-88Pro map first
    if (kit) return cmp({sign * geti(a, "map"), geti(a, "pc")}, {sign * geti(b, "map"), geti(b, "pc")});
    return cmp({sign * geti(a, "map"), geti(a, "pc"), geti(a, "bank")}, {sign * geti(b, "map"), geti(b, "pc"), geti(b, "bank")});
}

// The target module `si` plays (as ConversionTables::Pair::pick): its own; else the next older module's
// (that has the map); the SC-55 its own or an SC-88 sound on the SC-55 map. -1: none (generic conversion).
int playedSlot(const JsonValue& e, int si) {
    if (e.has(kSlots[si].key)) return si;
    if (si == 3) return e.has("sc88") && geti(e["sc88"], "map", 1) == 1 ? 2 : -1;
    for (int sj = si + 1; sj < 4; sj++)
        if (e.has(kSlots[sj].key) && geti(e[kSlots[sj].key], "map", 1) <= kSlots[si].maxMap) return sj;
    return -1;
}

bool sameSource(const JsonValue& a, const JsonValue& b) {
    for (const char* k : {"map", "bank", "msb", "lsb", "pc"})
        if (geti(a, k, -1) != geti(b, k, -1)) return false;
    return true;
}

// Where a loading error of ConversionTables points ("<file>, voice 12: ...", "<file>, note map \"x\" entry 3: ...").
struct ErrorPlace {
    int file = -1, view = -1, entry = -1;  // view: 0 voices, 1 drum kits, 2 note maps, 3 rules
    std::string name;
};

ErrorPlace errorPlace(const std::string& e) {
    ErrorPlace p;
    for (int f = 0; f < 3; f++) {
        std::string pre = std::string(ConversionTables::kFiles[f]) + ", ";
        if (e.compare(0, pre.size(), pre) != 0) continue;
        std::string rest = e.substr(pre.size());
        auto starts = [&](const char* what) { return rest.compare(0, strlen(what), what) == 0; };
        if (starts("voice ") || starts("drum kit ")) {
            p.view = starts("voice ") ? 0 : 1;
            p.entry = atoi(rest.c_str() + (p.view ? 9 : 6)) - 1;
        } else if (starts("note map \"") || starts("rules \"")) {
            p.view = starts("rules \"") ? 3 : 2;
            size_t from = p.view == 3 ? 7 : 10, to = rest.find('"', from);
            if (to != std::string::npos) p.name = rest.substr(from, to - from);
            else p.view = -1;
        }
        if (p.view >= 0) p.file = f;
    }
    return p;
}

bool sameFolder(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    return std::filesystem::equivalent(pathFromUtf8(a), pathFromUtf8(b), ec);
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Instrument definitions

std::string SoundNames::load() {
    std::string found;
    for (const std::string& d : {resourceDirectory() + "/insdef", std::string("insdef"), configDirectory() + "/insdef"})
        if (lib_.loadDirectory(d) > 0) {
            found = d;
            break;
        }
    gs_ = lib_.find("Roland SC-88 Pro");
    sc8850_ = lib_.find("Roland SC-8850");
    gsKits_ = lib_.find("Roland SC-88 Pro Drumsets");
    sc8850Kits_ = lib_.find("Roland SC-8850 (Drum Set)");
    xg_ = lib_.find("Yamaha XG");
    mu2000_ = lib_.find("Yamaha MU2000");
    xgKits_ = lib_.find("Yamaha XG Drums");
    mu2000Kits_ = lib_.find("Yamaha MU2000 Drums");
    return found;
}

namespace {
// The name a definition gives (bank, program), without the generic fallbacks (Patch[*], "1..128").
std::string exactName(const InsInstrument* ins, int bank, int pc0) {
    if (!ins) return {};
    auto it = ins->patchByBank.find(bank);
    if (it == ins->patchByBank.end() || !it->second) return {};
    auto n = it->second->find(pc0);
    if (n == it->second->end() || numeric(n->second)) return {};
    return n->second;
}
} // namespace

std::string SoundNames::gsVoice(int map, int var, int pc0) const {
    return exactName(map == 4 ? sc8850_ : gs_, var * 128 + map, pc0);
}

std::string SoundNames::gsKit(int map, int pc0) const { return exactName(map == 4 ? sc8850Kits_ : gsKits_, map, pc0); }

std::string SoundNames::xgVoice(int msb, int lsb, int pc0) const {
    std::string n = exactName(xg_, msb * 128 + lsb, pc0);
    return n.empty() ? exactName(mu2000_, msb * 128 + lsb, pc0) : n;
}

bool SoundNames::xgVoiceNamed(int msb, int lsb, int pc0, const std::string& name) const {
    return exactName(xg_, msb * 128 + lsb, pc0) == name || exactName(mu2000_, msb * 128 + lsb, pc0) == name;
}

std::string SoundNames::xgKit(int msb, int pc0) const {
    std::string n = exactName(xgKits_, msb * 128, pc0);
    return n.empty() ? exactName(mu2000Kits_, msb * 128, pc0) : n;
}

bool SoundNames::xgKitNamed(int msb, int pc0, const std::string& name) const {
    return exactName(xgKits_, msb * 128, pc0) == name || exactName(mu2000Kits_, msb * 128, pc0) == name;
}

std::string SoundNames::gsNote(int map, int pc0, int note) const {
    std::string n = InsLibrary::noteName(map == 4 ? sc8850Kits_ : gsKits_, map, pc0, note);
    return numeric(n) ? std::string() : n;
}

std::string SoundNames::xgNote(int msb, int pc0, int note) const {
    std::string n = InsLibrary::noteName(mu2000Kits_, msb * 128, pc0, note);
    if (n.empty() || numeric(n)) n = InsLibrary::noteName(xgKits_, msb * 128, pc0, note);
    return numeric(n) ? std::string() : n;
}

std::vector<SoundNames::Item> SoundNames::gsVoices(int map) const {
    std::vector<Item> v;
    const InsInstrument* ins = map == 4 ? sc8850_ : gs_;
    if (!ins) return v;
    for (auto& [bank, list] : ins->patchByBank) {
        if (!list || bank % 128 != map || bank / 128 >= 126 + (map == 1 ? 2 : 0)) continue;
        for (auto& [pc0, name] : *list)
            if (!numeric(name)) v.push_back({map, bank / 128, pc0, name});
    }
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return a.pc0 != b.pc0 ? a.pc0 < b.pc0 : a.b < b.b; });
    return v;
}

std::vector<SoundNames::Item> SoundNames::gsKits(int map) const {
    std::vector<Item> v;
    const InsInstrument* ins = map == 4 ? sc8850Kits_ : gsKits_;
    auto it = ins ? ins->patchByBank.find(map) : decltype(ins->patchByBank.end()){};
    if (!ins || it == ins->patchByBank.end() || !it->second) return v;
    for (auto& [pc0, name] : *it->second)
        if (!numeric(name)) v.push_back({map, 0, pc0, name});
    return v;
}

std::vector<SoundNames::Item> SoundNames::xgVoices() const {
    std::vector<Item> v;
    std::set<std::tuple<int, int, int>> seen;  // XG's name first, then MU2000's for the sounds XG lacks
    for (const InsInstrument* ins : {xg_, mu2000_}) {
        if (!ins) continue;
        for (auto& [bank, list] : ins->patchByBank) {
            if (!list || bank / 128 >= 126) continue;  // the drum banks
            for (auto& [pc0, name] : *list)
                if (!numeric(name) && seen.insert({bank / 128, bank % 128, pc0}).second) v.push_back({bank / 128, bank % 128, pc0, name});
        }
    }
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) {
        return a.a != b.a ? a.a < b.a : a.pc0 != b.pc0 ? a.pc0 < b.pc0 : a.b < b.b;
    });
    return v;
}

std::vector<SoundNames::Item> SoundNames::xgKits() const {
    std::vector<Item> v;
    std::set<std::pair<int, int>> seen;
    for (const InsInstrument* ins : {xgKits_, mu2000Kits_}) {
        if (!ins) continue;
        for (int msb : {127, 126}) {
            auto it = ins->patchByBank.find(msb * 128);
            if (it == ins->patchByBank.end() || !it->second) continue;
            for (auto& [pc0, name] : *it->second)
                if (!numeric(name) && seen.insert({msb, pc0}).second) v.push_back({msb, 0, pc0, name});
        }
    }
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return a.a != b.a ? a.a > b.a : a.pc0 < b.pc0; });
    return v;
}

// ---------------------------------------------------------------------------------------------------
// Setup, files

ConvEditor::ConvEditor(GLFWwindow* window)
    : window_(window), outputs_("ImMidi Editor", "ImMidi Editor Out", "[Virtual port] ImMidi Editor Out") {
    cfg_.load(configDirectory() + "/convedit.ini");
    insdefFolder_ = names_.load();
    for (int p = 0; p < 2; p++) {
        outName_[p] = cfg_.get(p ? "gsOut" : "xgOut", MidiOutputs::kNone);
        if (outName_[p] != MidiOutputs::kNone && !outputs_.setPortDevice(p, outName_[p])) outName_[p] = MidiOutputs::kNone;
    }
    std::string dir = cfg_.get("folder");
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_regular_file(pathFromUtf8(dir) / ConversionTables::kFiles[0], ec))
        dir = ConversionTables::defaultFolder();  // the tables ImMidi reads
    int tab = cfg_.getInt("table", 1);
    current_ = tab >= 0 && tab < 3 ? tab : 1;
    openFolder(dir);
    savedCfg_ = cfg_.serialize();
}

ConvEditor::~ConvEditor() = default;

void ConvEditor::saveSettings() {
    cfg_.set("folder", folder_);
    cfg_.set("xgOut", outName_[0]);
    cfg_.set("gsOut", outName_[1]);
    cfg_.setInt("table", current_);
    std::string t = cfg_.serialize();
    if (t != savedCfg_ && cfg_.save(configDirectory() + "/convedit.ini")) savedCfg_ = t;
}

void ConvEditor::openFolder(const std::string& dir) {
    pendingDoc_ = nullptr;
    folder_ = dir;
    for (int f = 0; f < 3; f++) {
        Doc& d = docs_[f];
        int view = d.view;
        d = Doc();
        d.file = f;
        d.view = view;
        if (dir.empty()) {
            d.loadError = "no folder";
            continue;
        }
        d.path = pathToUtf8(pathFromUtf8(dir) / ConversionTables::kFiles[f]);
        d.loaded = d.doc.load(d.path, d.loadError);
        if (d.loaded) {
            for (const char* sec : {"voices", "drumKits"})
                if (!d.doc.root().find(sec)) d.doc.root().set(sec, JsonValue::makeArray());
            for (const char* sec : {"noteMaps", "rules"})
                if (!d.doc.root().find(sec)) d.doc.root().set(sec, JsonValue::makeObject());
        }
    }
    validateSoon_ = true;
    validateAt_ = Clock::now();
    locate();
    saveSettings();
}

void ConvEditor::locate() {
    readFolder_ = ConversionTables::defaultFolder();
    sourceFolder_ = sourceFolderFor(folder_);
    isRead_ = sameFolder(folder_, readFolder_);
    isSourceOfRead_ = !isRead_ && sameFolder(sourceFolderFor(readFolder_), folder_);
    std::string f = folder_;
    std::replace(f.begin(), f.end(), '\\', '/');
    inApp_ = f.find(".app/Contents/Resources/") != std::string::npos;
}

// The tables inside a macOS app cannot be changed (an update replaces them, and a change breaks the app's
// signature): the user's own copy, which every program reads instead.
void ConvEditor::saveUserCopy() {
    commitPending();
    const std::string dir = ConversionTables::userFolder();
    std::error_code ec;
    std::filesystem::create_directories(pathFromUtf8(dir), ec);
    std::string err;
    for (Doc& d : docs_) {
        std::string path = pathToUtf8(pathFromUtf8(dir) / ConversionTables::kFiles[d.file]);
        std::vector<uint8_t> old;
        if (readWholeFile(path, old)) writeFileAtomic(path + ".bak", std::string(old.begin(), old.end()));
        if (!writeFileAtomic(path, d.doc.serialize())) err += (err.empty() ? "" : ", ") + path;
    }
    messageUntil_ = Clock::now() + std::chrono::seconds(8);
    if (!err.empty()) {
        message_ = "Could not write " + err;
        return;
    }
    for (Doc& d : docs_) {
        d.path = pathToUtf8(pathFromUtf8(dir) / ConversionTables::kFiles[d.file]);
        d.savedVersion = d.version;
        d.backedUp = true;
    }
    folder_ = dir;
    locate();
    saveSettings();
    message_ = "Saved as your own copy: ImMidi and ImMidi Bridge read it when they start";
}

bool ConvEditor::dirty() const {
    for (const Doc& d : docs_)
        if (d.loaded && d.version != d.savedVersion) return true;
    return false;
}

bool ConvEditor::save() {
    commitPending();
    int saved = 0;
    std::string err;
    for (Doc& d : docs_) {
        if (!d.loaded || d.version == d.savedVersion) continue;
        std::string text = d.doc.serialize();
        // The first save of a session keeps the file as it was in <name>.bak.
        if (!d.backedUp) {
            std::vector<uint8_t> old;
            if (readWholeFile(d.path, old)) writeFileAtomic(d.path + ".bak", std::string(old.begin(), old.end()));
            d.backedUp = true;
        }
        if (writeFileAtomic(d.path, text)) {
            d.savedVersion = d.version;
            saved++;
        } else {
            err += (err.empty() ? "" : ", ") + fileNameOf(d.path);
        }
    }
    message_ = !err.empty() ? "Could not write " + err
               : saved ? "Saved " + std::to_string(saved) + (saved == 1 ? " table" : " tables") + " (restart ImMidi to use them)"
                       : "Nothing to save";
    messageUntil_ = Clock::now() + std::chrono::seconds(6);
    return err.empty();
}

void ConvEditor::requestSave(std::function<void()> then) {
    commitPending();
    if (dirty()) validate();  // the tables as they would be saved
    if (!dirty() || valid_) {
        if (save() && then) then();
        return;
    }
    afterSave_ = std::move(then);
    confirmInvalidSave_ = true;
}

void ConvEditor::revert() { openFolder(folder_); }

// The repository's conversion folder when `dir` is the copy a build puts next to the programs
// (bin/<system>-<config>/conversion), which the next build replaces.
std::string ConvEditor::sourceFolderFor(const std::string& dir) const {
    std::error_code ec;
    auto here = std::filesystem::weakly_canonical(pathFromUtf8(dir), ec);
    auto copy = std::filesystem::weakly_canonical(pathFromUtf8(resourceDirectory()) / "conversion", ec);
    if (here.empty() || here != copy) return {};
    auto src = (pathFromUtf8(resourceDirectory()) / ".." / ".." / "conversion").lexically_normal();
    if (std::filesystem::is_regular_file(src / ConversionTables::kFiles[0], ec)) return pathToUtf8(src);
    return {};
}

// ---------------------------------------------------------------------------------------------------
// Edits, undo, checks

// Called before the document changes. One undo step per edited item: the state before its first change
// is kept until the item lets go (a text field while typing; a button or a choice at once).
void ConvEditor::edit(Doc& d) {
    unsigned id = ImGui::GetActiveID();
    if (pendingDoc_ != &d || id == 0 || id != pendingId_) {
        commitPending();
        pendingDoc_ = &d;
        pendingId_ = id;
        pendingText_ = d.doc.serialize();
        pendingVersion_ = d.version;
    }
    d.version = ++lastVersion_;
    validateSoon_ = true;
    validateAt_ = Clock::now() + std::chrono::milliseconds(400);
}

void ConvEditor::commitPending() {
    if (!pendingDoc_) return;
    auto& u = pendingDoc_->undo;
    u.push_back({std::move(pendingText_), pendingVersion_});
    if (u.size() > 100) u.erase(u.begin());
    pendingDoc_->redo.clear();
    pendingDoc_ = nullptr;
    pendingId_ = 0;
}

void ConvEditor::undo(Doc& d) {
    commitPending();
    if (d.undo.empty()) return;
    d.redo.push_back({d.doc.serialize(), d.version});
    std::string err;
    d.doc.parse(d.undo.back().text, err);
    d.version = d.undo.back().version;
    d.undo.pop_back();
    validateSoon_ = true;
    validateAt_ = Clock::now();
}

void ConvEditor::redo(Doc& d) {
    commitPending();
    if (d.redo.empty()) return;
    d.undo.push_back({d.doc.serialize(), d.version});
    std::string err;
    d.doc.parse(d.redo.back().text, err);
    d.version = d.redo.back().version;
    d.redo.pop_back();
    validateSoon_ = true;
    validateAt_ = Clock::now();
}

// ImMidi's own loader on the tables as they would be saved, and the sound names against the definitions.
void ConvEditor::validate() {
    validateSoon_ = false;
    std::string texts[3];
    bool all = true;
    for (int f = 0; f < 3; f++) {
        if (docs_[f].loaded) texts[f] = docs_[f].doc.serialize();
        else all = false;
    }
    if (!all) {
        valid_ = false;
        validError_.clear();
        for (const Doc& d : docs_)
            if (!d.loaded) validError_ += (validError_.empty() ? "" : "; ") + std::string(ConversionTables::kFiles[d.file]) + ": " + d.loadError;
    } else {
        ConversionTables t;
        valid_ = t.loadTexts(texts, validError_);
    }
    nameIssues_.clear();
    if (!names_.loaded()) return;
    for (const Doc& d : docs_) {
        if (!d.loaded) continue;
        for (int k = 0; k < 2; k++) {
            const JsonValue& list = d.doc.root()[k ? "drumKits" : "voices"];
            for (size_t i = 0; i < list.size(); i++) {
                const JsonValue& e = list[i];
                auto check = [&](const JsonValue& s, bool gs, const char* slot) {
                    if (!s.isObject() || nameMatches(s, gs, k == 1)) return;
                    nameIssues_.push_back({d.file, k == 1, int(i), slot, s["name"].string(), expectedName(s, gs, k == 1)});
                };
                check(e["from"], d.file != 1, "from");
                if (d.file == 0) check(e["to"], false, "to");
                else
                    for (const Slot& sl : kSlots) check(e[sl.key], true, sl.key);
            }
        }
    }
}

std::string ConvEditor::expectedName(const JsonValue& s, bool gs, bool kit) const {
    int pc0 = geti(s, "pc", 1) - 1;
    if (gs) return kit ? names_.gsKit(geti(s, "map", 1), pc0) : names_.gsVoice(geti(s, "map", 1), geti(s, "bank"), pc0);
    return kit ? names_.xgKit(geti(s, "msb"), pc0) : names_.xgVoice(geti(s, "msb"), geti(s, "lsb"), pc0);
}

bool ConvEditor::nameMatches(const JsonValue& s, bool gs, bool kit) const {
    if (!names_.loaded()) return true;
    const std::string& n = s["name"].string();
    if (!gs) {
        int pc0 = geti(s, "pc", 1) - 1;
        return kit ? names_.xgKitNamed(geti(s, "msb"), pc0, n) : names_.xgVoiceNamed(geti(s, "msb"), geti(s, "lsb"), pc0, n);
    }
    return expectedName(s, gs, kit) == n;
}

// ---------------------------------------------------------------------------------------------------
// The window

std::string ConvEditor::windowTitle() const {
    return std::string("ImMidi Conversion Editor") + (folder_.empty() ? "" : " - " + folder_) + (dirty() ? " *" : "");
}

bool ConvEditor::busy() const { return !noteOffs_.empty() || validateSoon_ || folderDialog_ || Clock::now() < messageUntil_; }

void ConvEditor::requestClose() {
    if (dirty()) closeAsked_ = true;
    else quit_ = true;
}

void ConvEditor::frame() {
    auto now = Clock::now();
    // Audition: notes end on time.
    for (size_t i = 0; i < noteOffs_.size();) {
        if (noteOffs_[i].at <= now) {
            send(noteOffs_[i].port, {uint8_t(0x80 | noteOffs_[i].ch), noteOffs_[i].note, 0});
            noteOffs_.erase(noteOffs_.begin() + long(i));
        } else {
            i++;
        }
    }
    if (folderDialog_) {
        std::vector<std::string> paths;
        if (dialog_.poll(paths)) {
            folderDialog_ = false;
            if (!paths.empty()) {
                if (dirty()) {
                    pendingFolder_ = paths[0];
                    confirmDiscard_ = true;
                } else {
                    openFolder(paths[0]);
                }
            }
        }
    }
    if (validateSoon_ && now >= validateAt_) validate();

    Doc& cur = docs_[current_];
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) requestSave();
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) undo(cur);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
            redo(cur);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##editor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);
    menuBar();
    statusLine();
    if (ImGui::BeginTabBar("tables")) {
        for (int f : kDocOrder) {
            Doc& d = docs_[f];
            std::string label = std::string(kDocTitle[f]) + (d.loaded && d.version != d.savedVersion ? " *" : "") + "###table" + std::to_string(f);
            ImGuiTabItemFlags flags = selectTab_ && f == current_ ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
                if (current_ != f && !selectTab_) {
                    commitPending();
                    current_ = f;
                    saveSettings();
                }
                docView(d);
                ImGui::EndTabItem();
            }
        }
        selectTab_ = false;
        ImGui::EndTabBar();
    }
    ImGui::End();
    pickerPopup();
    namesPopup();
    dialogs();
    if (pendingDoc_ && (pendingId_ == 0 || ImGui::GetActiveID() != pendingId_)) commitPending();
}

void ConvEditor::menuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open folder...")) {
            DialogRequest req;
            req.kind = DialogKind::SelectFolder;
            req.title = "The folder with the conversion tables";
            req.startDir = folder_;
            if (NativeDialogs::available() && dialog_.start(req, nullptr)) {
                folderDialog_ = true;
            } else {
                snprintf(folderInput_, sizeof folderInput_, "%s", folder_.c_str());
                askFolderPath_ = true;
            }
        }
        if (!sourceFolder_.empty() && ImGui::MenuItem("Open the source folder")) {
            std::string src = sourceFolder_;
            if (dirty()) {
                pendingFolder_ = src;
                confirmDiscard_ = true;
            } else {
                openFolder(src);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, dirty())) requestSave();
        if (ImGui::MenuItem("Revert to the saved files", nullptr, false, dirty())) {
            pendingFolder_ = folder_;
            confirmDiscard_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) requestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        Doc& d = docs_[current_];
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !d.undo.empty() || pendingDoc_ == &d)) undo(d);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !d.redo.empty())) redo(d);
        ImGui::Separator();
        if (ImGui::MenuItem("Check the names...", nullptr, false, names_.loaded())) showNames_ = true;
        ImGui::EndMenu();
    }
    auditionMenu();
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About the conversion tables")) showAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void ConvEditor::statusLine() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Folder:");
    ImGui::SameLine();
    ImGui::TextUnformatted(folder_.empty() ? "(none: File > Open folder)" : folder_.c_str());
    if (Clock::now() < messageUntil_) {
        ImGui::SameLine();
        ImGui::TextColored(kGood, "  %s", message_.c_str());
    }
    // Where these tables are, and the ones ImMidi reads.
    if (inApp_) {
        ImGui::TextColored(kWarn, "These are the tables inside the app: an update replaces them, and a change breaks the app's signature.");
        ImGui::SameLine();
        bool all = std::all_of(docs_.begin(), docs_.end(), [](const Doc& d) { return d.loaded; });
        ImGui::BeginDisabled(!all);
        if (ImGui::SmallButton("Save as your own copy")) saveUserCopy();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Saves the tables, with your changes, to %s: ImMidi, ImMidi Bridge and this editor read them from there.",
                              ConversionTables::userFolder().c_str());
    } else if (!sourceFolder_.empty()) {
        ImGui::TextColored(kWarn, "These are the tables a build copies next to the programs: the next build replaces them.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open the source folder")) {
            std::string src = sourceFolder_;
            if (dirty()) {
                pendingFolder_ = src;
                confirmDiscard_ = true;
            } else {
                openFolder(src);
            }
        }
    } else if (isSourceOfRead_) {
        ImGui::TextDisabled("A build copies these tables next to the programs, where ImMidi reads them.");
    }
    if (!isRead_ && !isSourceOfRead_ && !folder_.empty()) {
        if (readFolder_.empty()) ImGui::TextDisabled("ImMidi finds no tables of its own: it reads them next to the program or from %s.",
                                                     ConversionTables::userFolder().c_str());
        else ImGui::TextDisabled("ImMidi reads the tables in %s.", readFolder_.c_str());
    }
    if (validateSoon_) {
        ImGui::TextDisabled("Checking...");
    } else if (valid_) {
        ImGui::TextColored(kGood, "ImMidi can load these tables.");
    } else {
        ImGui::TextColored(kBad, "ImMidi cannot load these tables: %s", validError_.c_str());
        ErrorPlace ep = errorPlace(validError_);
        if (ep.file >= 0 && docs_[ep.file].loaded) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Show##error")) {
                Doc& d = docs_[ep.file];
                commitPending();
                current_ = ep.file;
                selectTab_ = true;
                d.view = ep.view;
                d.selectView = true;
                if (ep.view <= 1) {
                    d.sel[ep.view] = ep.entry;
                    d.scrollTo[ep.view] = ep.entry;
                } else if (ep.view == 2) {
                    d.selMap = ep.name;
                } else {
                    d.selRules = ep.name;
                }
            }
        }
    }
    if (!nameIssues_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%d %s not match the instrument definitions.", int(nameIssues_.size()),
                           nameIssues_.size() == 1 ? "name does" : "names do");
        ImGui::SameLine();
        if (ImGui::SmallButton("Show")) showNames_ = true;
    }
    if (!names_.loaded()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "  The instrument definitions (insdef) were not found: no sound names.");
    }
}

void ConvEditor::docView(Doc& d) {
    if (!d.loaded) {
        ImGui::TextColored(kBad, "%s: %s", ConversionTables::kFiles[d.file], d.loadError.c_str());
        return;
    }
    ImGui::TextDisabled("%s  (%s)", kDocWhat[d.file], ConversionTables::kFiles[d.file]);
    static const char* const views[5] = {"Voices", "Drum kits", "Note maps", "Rules", "About"};
    if (ImGui::BeginTabBar("views")) {
        const bool select = d.selectView;
        d.selectView = false;
        for (int v = 0; v < 5; v++) {
            ImGuiTabItemFlags fl = select && d.view == v ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(views[v], nullptr, fl)) {
                if (d.view != v && !select) {
                    commitPending();
                    d.view = v;
                }
                switch (v) {
                case 0: entriesView(d, false); break;
                case 1: entriesView(d, true); break;
                case 2: noteMapsView(d); break;
                case 3: rulesView(d); break;
                default: aboutView(d); break;
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------------------------------
// Voices and drum kits

// "Name  (numbers)"; a Sound Canvas sound names its map unless it is `ownMap` (the module's own).
std::string ConvEditor::soundLabel(const JsonValue& s, bool gs, bool kit, int ownMap) const {
    if (!s.isObject()) return {};
    char b[160];
    const std::string& n = s["name"].string();
    if (gs) {
        int map = std::clamp(geti(s, "map", 1), 0, 4);
        std::string where = map == ownMap ? std::string() : std::string(kMapNames[map]) + ", ";
        if (kit) snprintf(b, sizeof b, "%s  (%s%d)", n.c_str(), where.c_str(), geti(s, "pc"));
        else snprintf(b, sizeof b, "%s  (%s%d/%d)", n.c_str(), where.c_str(), geti(s, "bank"), geti(s, "pc"));
    } else {
        if (kit) snprintf(b, sizeof b, "%s  (%d/%d)", n.c_str(), geti(s, "msb"), geti(s, "pc"));
        else snprintf(b, sizeof b, "%s  (%d/%d/%d)", n.c_str(), geti(s, "msb"), geti(s, "lsb"), geti(s, "pc"));
    }
    return b;
}

std::string ConvEditor::rowText(const Doc& d, const JsonValue& e, bool kits) const {
    std::string t = soundLabel(e["from"], d.file != 1, kits);
    if (d.file == 0) t += " " + soundLabel(e["to"], false, kits);
    else
        for (const Slot& s : kSlots)
            if (e.has(s.key)) t += " " + soundLabel(e[s.key], true, kits);
    return lower(t);
}

void ConvEditor::entriesView(Doc& d, bool kits) {
    const int k = kits ? 1 : 0;
    JsonValue& list = *d.doc.root().find(kits ? "drumKits" : "voices");
    // Which target columns the list shows: the ones this table uses.
    bool slotUsed[4] = {d.file == 1, d.file == 1, true, true};
    for (size_t i = 0; i < list.size(); i++)
        for (int s = 0; s < 4; s++)
            if (list[i].has(kSlots[s].key)) slotUsed[s] = true;
    std::string f = lower(d.filter[k]);
    if (d.rowsVersion[k] != d.version || d.rowsFilter[k] != f) {
        d.rows[k].clear();
        for (size_t i = 0; i < list.size(); i++)
            if (f.empty() || rowText(d, list[i], kits).find(f) != std::string::npos) d.rows[k].push_back(int(i));
        d.rowsVersion[k] = d.version;
        d.rowsFilter[k] = f;
    }
    // An entry to show that the search hides: show all.
    if (d.scrollTo[k] >= 0 && d.filter[k][0] && std::find(d.rows[k].begin(), d.rows[k].end(), d.scrollTo[k]) == d.rows[k].end()) {
        d.filter[k][0] = 0;
        d.rows[k].resize(list.size());
        for (size_t i = 0; i < list.size(); i++) d.rows[k][i] = int(i);
        d.rowsFilter[k].clear();
    }
    if (d.sel[k] >= int(list.size())) d.sel[k] = int(list.size()) - 1;

    if (!ImGui::BeginTable("split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.56f);
    ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch, 0.44f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    // The list
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputTextWithHint("##filter", "Search (names, numbers)", d.filter[k], sizeof d.filter[k]);
    ImGui::SameLine();
    ImGui::TextDisabled("%d of %d", int(d.rows[k].size()), int(list.size()));
    ImGui::SameLine();
    if (ImGui::Button(kits ? "Add a drum kit..." : "Add a voice...")) {
        const bool fromGs = d.file != 1;
        int file = d.file;
        openPicker(fromGs ? (kits ? PickKind::GsKit : PickKind::GsVoice) : (kits ? PickKind::XgKit : PickKind::XgVoice), fromGs ? 3 : 0, 4,
                   SoundNames::Item{0, 0, -1, {}}, [this, file, kits](const SoundNames::Item& it) {
                       Doc& dd = docs_[file];
                       const bool gsSrc = file != 1;
                       JsonValue& lst = *dd.doc.root().find(kits ? "drumKits" : "voices");
                       JsonValue from = JsonValue::makeObject();
                       if (gsSrc) {
                           seti(from, "map", it.a, kGsOrder);
                           if (!kits) seti(from, "bank", it.b, kGsOrder);
                       } else {
                           seti(from, "msb", it.a, kXgOrder);
                           if (!kits) seti(from, "lsb", it.b, kXgOrder);
                       }
                       seti(from, "pc", it.pc0 + 1, gsSrc ? kGsOrder : kXgOrder);
                       sets(from, "name", it.name, gsSrc ? kGsOrder : kXgOrder);
                       for (size_t i = 0; i < lst.size(); i++)
                           if (sameSource(lst[i]["from"], from)) {  // already there
                               dd.sel[kits ? 1 : 0] = int(i);
                               dd.scrollTo[kits ? 1 : 0] = int(i);
                               message_ = "That sound already has an entry";
                               messageUntil_ = Clock::now() + std::chrono::seconds(4);
                               return;
                           }
                       // Targets: as for the capital sound of the program (bank 0), else the same program.
                       JsonValue e = JsonValue::makeObject();
                       e.set("from", from, kEntryOrder);
                       const JsonValue* cap = nullptr;
                       for (size_t i = 0; i < lst.size() && !kits; i++) {
                           const JsonValue& fr = lst[i]["from"];
                           if (geti(fr, "pc") == it.pc0 + 1 && geti(fr, gsSrc ? "bank" : "lsb") == 0 && geti(fr, gsSrc ? "map" : "msb") == it.a)
                               cap = &lst[i];
                       }
                       std::string added = "Added " + it.name;
                       if (cap) {
                           for (auto& [key, v] : cap->items()) {
                               if (key == "from" || key == "note") continue;
                               JsonValue t = v;
                               if (t.isObject()) t.remove("note");  // about the other sound
                               e.set(key, t, kEntryOrder);
                           }
                           added += ": its sounds start as " + (*cap)["from"]["name"].string() + "'s, the sound of bank 0";
                       } else if (file == 0) {
                           JsonValue to = JsonValue::makeObject();
                           seti(to, "msb", kits ? 127 : 0, kXgOrder);
                           if (!kits) seti(to, "lsb", 0, kXgOrder);
                           seti(to, "pc", kits ? 1 : it.pc0 + 1, kXgOrder);
                           sets(to, "name", kits ? names_.xgKit(127, 0) : names_.xgVoice(0, 0, it.pc0), kXgOrder);
                           e.set("to", to, kEntryOrder);
                       } else {
                           JsonValue t = JsonValue::makeObject();
                           seti(t, "map", 1, kGsOrder);
                           if (!kits) seti(t, "bank", 0, kGsOrder);
                           seti(t, "pc", kits ? 1 : it.pc0 + 1, kGsOrder);
                           sets(t, "name", kits ? names_.gsKit(1, 0) : names_.gsVoice(1, 0, it.pc0), kGsOrder);
                           e.set("sc55", t, kEntryOrder);
                       }
                       if (!cap) added += file == 0 ? ": it starts as the XG sound of the same program" : ": it starts as the SC-55 sound of the same program";
                       message_ = added;
                       messageUntil_ = Clock::now() + std::chrono::seconds(8);
                       edit(dd);
                       size_t at = lst.size();
                       for (size_t i = 0; i < lst.size(); i++)
                           if (compareSource(file, kits, from, lst[i]["from"]) < 0) {
                               at = i;
                               break;
                           }
                       auto& items = lst.mutableItems();
                       items.insert(items.begin() + long(at), {std::string(), std::move(e)});
                       dd.sel[kits ? 1 : 0] = int(at);
                       dd.scrollTo[kits ? 1 : 0] = int(at);
                   });
    }
    float listH = ImGui::GetContentRegionAvail().y;
    int ncols = 1;
    if (d.file == 0) ncols = 2;
    else
        for (bool u : slotUsed) ncols += u ? 1 : 0;
    if (ImGui::BeginTable("rows", ncols,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(d.file == 1 ? "XG" : "Sound Canvas", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        if (d.file == 0) ImGui::TableSetupColumn("XG", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        else
            for (int s = 0; s < 4; s++)
                if (slotUsed[s]) ImGui::TableSetupColumn(kSlots[s].label, ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        int scrollRow = -1;
        if (d.scrollTo[k] >= 0)
            for (size_t r = 0; r < d.rows[k].size(); r++)
                if (d.rows[k][r] == d.scrollTo[k]) scrollRow = int(r);
        ImGuiListClipper clip;
        clip.Begin(int(d.rows[k].size()));
        if (scrollRow >= 0) clip.IncludeItemByIndex(scrollRow);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                int i = d.rows[k][size_t(r)];
                const JsonValue& e = list[size_t(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                std::string from = soundLabel(e["from"], d.file != 1, kits);
                if (ImGui::Selectable(from.c_str(), d.sel[k] == i, ImGuiSelectableFlags_SpanAllColumns)) {
                    commitPending();
                    d.sel[k] = i;
                }
                if (r == scrollRow) {
                    ImGui::SetScrollHereY(0.4f);
                    d.scrollTo[k] = -1;
                }
                if (d.file == 0) {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(soundLabel(e["to"], false, kits).c_str());
                } else {
                    for (int s = 0; s < 4; s++) {
                        if (!slotUsed[s]) continue;
                        ImGui::TableNextColumn();
                        // Without its own sound: the one the module plays, dimmed.
                        int ps = playedSlot(e, s);
                        if (ps == s) ImGui::TextUnformatted(soundLabel(e[kSlots[s].key], true, kits, kSlots[s].maxMap).c_str());
                        else if (ps >= 0) ImGui::TextDisabled("%s", soundLabel(e[kSlots[ps].key], true, kits, kSlots[s].maxMap).c_str());
                        else ImGui::TextDisabled("-");
                        if (ps != s && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
                            if (ps >= 0) ImGui::SetTooltip("No %s sound: the %s plays the %s's.", kSlots[s].label, kSlots[s].label, kSlots[ps].label);
                            else ImGui::SetTooltip("No %s sound: the %s converts it generically.", kSlots[s].label, kSlots[s].label);
                        }
                    }
                }
                ImGui::PopID();
            }
        }
        if (scrollRow < 0) d.scrollTo[k] = -1;
        ImGui::EndTable();
    }
    // The details
    ImGui::TableNextColumn();
    if (ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_None)) {
        if (d.sel[k] >= 0 && d.sel[k] < int(list.size())) entryDetails(d, kits, d.sel[k]);
        else ImGui::TextDisabled("Choose an entry in the list, or add one.");
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

// Number, name, choose and play: a sound or drum set of a target (or the source).
bool ConvEditor::soundFields(Doc& d, JsonValue& s, bool gs, bool kit, int maxMap, const char* id, bool allowMapChange) {
    ImGui::PushID(id);
    auto& order = gs ? kGsOrder : kXgOrder;
    bool numbersChanged = false;
    const float w = ImGui::GetFontSize() * 6.5f;
    auto num = [&](const char* label, const char* key, int lo, int hi, int def) {
        int v = geti(s, key, def);
        fieldLabel(label);
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt((std::string("##") + key).c_str(), &v)) {
            v = std::clamp(v, lo, hi);
            edit(d);
            seti(s, key, v, order);
            numbersChanged = true;
        }
    };
    if (gs) {
        int map = std::clamp(geti(s, "map", 1), 1, 4);
        fieldLabel("Map");
        ImGui::SetNextItemWidth(w * 1.25f);
        ImGui::BeginDisabled(!allowMapChange);
        if (ImGui::BeginCombo("##map", kMapNames[map])) {
            for (int m = 1; m <= maxMap; m++)
                if (ImGui::Selectable(kMapNames[m], m == map)) {
                    edit(d);
                    seti(s, "map", m, order);
                    numbersChanged = true;
                }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!kit) {
            num("Bank", "bank", 0, 127, 0);
            ImGui::SameLine();
        }
        num("PC", "pc", 1, 128, 1);
    } else {
        num("MSB", "msb", 0, 127, kit ? 127 : 0);
        ImGui::SameLine();
        if (!kit) {
            num("LSB", "lsb", 0, 127, 0);
            ImGui::SameLine();
        }
        num("PC", "pc", 1, 128, 1);
    }
    if (numbersChanged) {
        std::string n = expectedName(s, gs, kit);
        if (!n.empty()) sets(s, "name", n, order);
    }
    std::string name = s["name"].string();
    fieldLabel("Name");
    float bw = ImGui::CalcTextSize("Choose...").x + ImGui::CalcTextSize("Play").x + ImGui::GetStyle().FramePadding.x * 4 +
               ImGui::GetStyle().ItemSpacing.x * 2;
    if (inputString("##name", name, ImGui::GetContentRegionAvail().x - bw)) {
        edit(d);
        sets(s, "name", name, order);
    }
    if (!nameMatches(s, gs, kit)) {
        std::string ex = expectedName(s, gs, kit);
        if (ex.empty()) ImGui::SetItemTooltip("The instrument definitions have no sound with these numbers.");
        else ImGui::SetItemTooltip("The instrument definitions call this sound \"%s\".", ex.c_str());
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(kWarn));
    }
    ImGui::SameLine();
    if (ImGui::Button("Choose...")) {
        // Chosen later: find the sound again by where it is.
        int file = d.file, entry = d.sel[kit ? 1 : 0];
        std::string slot = id;
        PickKind pk = gs ? (kit ? PickKind::GsKit : PickKind::GsVoice) : (kit ? PickKind::XgKit : PickKind::XgVoice);
        int map = gs ? std::min(geti(s, "map", 1), maxMap) : 0;
        SoundNames::Item now{gs ? geti(s, "map", 1) : geti(s, "msb"), kit ? 0 : geti(s, gs ? "bank" : "lsb"), geti(s, "pc", 1) - 1, {}};
        openPicker(pk, map, maxMap, now, [this, file, entry, slot, gs, kit](const SoundNames::Item& it) {
            Doc& dd = docs_[file];
            JsonValue* lst = dd.doc.root().find(kit ? "drumKits" : "voices");
            if (!lst || entry < 0 || entry >= int(lst->size())) return;
            JsonValue* t = lst->at(size_t(entry)).find(slot);
            if (!t) return;
            edit(dd);
            auto& o = gs ? kGsOrder : kXgOrder;
            if (gs) {
                seti(*t, "map", it.a, o);
                if (!kit) seti(*t, "bank", it.b, o);
            } else {
                seti(*t, "msb", it.a, o);
                if (!kit) seti(*t, "lsb", it.b, o);
            }
            seti(*t, "pc", it.pc0 + 1, o);
            sets(*t, "name", it.name, o);
        });
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(outName_[gs ? 1 : 0] == MidiOutputs::kNone);
    if (ImGui::Button("Play")) {
        if (kit) playDrum(gs, gs ? geti(s, "map", 1) : geti(s, "msb", 127), geti(s, "pc", 1) - 1, 38);
        else playVoice(gs, gs ? geti(s, "bank") : geti(s, "msb"), gs ? geti(s, "map", 1) : geti(s, "lsb"), geti(s, "pc", 1) - 1);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Plays the sound on the %s module chosen in the Audition menu.", gs ? "GS" : "XG");
    ImGui::PopID();
    return numbersChanged;
}

void ConvEditor::entryDetails(Doc& d, bool kits, int index) {
    JsonValue& list = *d.doc.root().find(kits ? "drumKits" : "voices");
    JsonValue& e = list.at(size_t(index));
    const bool fromGs = d.file != 1;
    ImGui::PushID(index);
    if (!e.find("from")) e.set("from", JsonValue::makeObject(), kEntryOrder);
    ImGui::SeparatorText(fromGs ? "Sound Canvas sound" : "XG sound");
    soundFields(d, *e.find("from"), fromGs, kits, 4, "from", true);
    for (size_t i = 0; i < list.size(); i++)
        if (int(i) != index && sameSource(list[i]["from"], e["from"])) {
            ImGui::TextColored(kWarn, "Entry %d has the same sound: ImMidi uses the later one.", int(i) + 1);
            ImGui::SameLine();
            if (ImGui::SmallButton("Show it")) {
                d.sel[kits ? 1 : 0] = int(i);
                d.scrollTo[kits ? 1 : 0] = int(i);
            }
            break;
        }

    auto extras = [&](JsonValue& t, bool withAttack, const char* id) {
        ImGui::PushID(id);
        const float w = ImGui::GetFontSize() * 6.5f;
        auto& order = t.has("map") ? kGsOrder : t.has("msb") ? kXgOrder : kEntryOrder;
        int vol = geti(t, "volume");
        fieldLabel("Volume");
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt("##volume", &vol)) {
            edit(d);
            vol = std::clamp(vol, -127, 127);
            if (vol) seti(t, "volume", vol, order);
            else t.remove("volume");
        }
        ImGui::SetItemTooltip("Added to the part volume (CC#7) while this sound plays.");
        if (withAttack) {
            ImGui::SameLine();
            int att = geti(t, "attack");
            fieldLabel("Attack");
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt("##attack", &att)) {
                edit(d);
                att = std::clamp(att, -64, 63);
                if (att) seti(t, "attack", att, order);
                else t.remove("attack");
            }
            ImGui::SetItemTooltip("Added to the part's attack time while this sound plays (negative: a faster attack).");
        }
        if (kits) {
            // The note map: which note of the target kit each note plays.
            std::string cur = t["notes"].string();
            ImGui::SameLine();
            fieldLabel("Note map");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Edit").x - ImGui::GetStyle().FramePadding.x * 2 -
                                    ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::BeginCombo("##notemap", cur.empty() ? "(none: the notes stay)" : cur.c_str())) {
                if (ImGui::Selectable("(none: the notes stay)", cur.empty())) {
                    edit(d);
                    t.remove("notes");
                }
                for (auto& [name, _] : d.doc.root()["noteMaps"].items())
                    if (ImGui::Selectable(name.c_str(), name == cur)) {
                        edit(d);
                        sets(t, "notes", name, order);
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(cur.empty());
            if (ImGui::Button("Edit")) {
                d.selMap = cur;
                d.view = 2;
                d.selectView = true;
            }
            ImGui::EndDisabled();
        }
        std::string note = t["note"].string();
        fieldLabel("Note");
        if (noteField("##note", note)) {
            edit(d);
            if (note.empty()) t.remove("note");
            else sets(t, "note", note, order);
        }
        ImGui::PopID();
    };

    if (d.file == 0) {
        ImGui::SeparatorText(kits ? "XG drum kit" : "XG sound");
        if (!e.find("to")) e.set("to", JsonValue::makeObject(), kEntryOrder);
        soundFields(d, *e.find("to"), false, kits, 0, "to", false);
        extras(e, !kits, "entry");
    } else {
        // Targets, in the order ImMidi picks them: a module plays its own entry, else the next older one.
        for (int si = 0; si < 4; si++) {
            const Slot& sl = kSlots[si];
            bool has = e.has(sl.key);
            if (d.file == 2 && !has && si < 2) continue;  // the SC-88Pro and SC-8850 have every map
            ImGui::PushID(sl.key);
            ImGui::SeparatorText(sl.label);
            if (ImGui::Checkbox(has ? "Its own sound:###own" : "Its own sound###own", &has)) {
                edit(d);
                if (has) {
                    // Start from the sound the module plays now.
                    JsonValue t;
                    if (int ps = playedSlot(e, si); ps >= 0) t = e[kSlots[ps].key];
                    if (t.isNull()) {
                        t = JsonValue::makeObject();
                        seti(t, "map", 1, kGsOrder);
                        if (!kits) seti(t, "bank", 0, kGsOrder);
                        seti(t, "pc", geti(e["from"], "pc", 1), kGsOrder);
                        sets(t, "name", expectedName(t, true, kits), kGsOrder);
                    }
                    t.remove("note");
                    e.set(sl.key, t, kEntryOrder);
                } else {
                    e.remove(sl.key);
                }
            }
            if (has && e.find(sl.key)) {
                soundFields(d, *e.find(sl.key), true, kits, sl.maxMap, sl.key, true);
                extras(*e.find(sl.key), !kits, sl.key);
            } else {
                int ps = playedSlot(e, si);
                ImGui::SameLine();
                if (ps >= 0) ImGui::TextDisabled("(the %s plays the %s's sound)", sl.label, kSlots[ps].label);
                else ImGui::TextDisabled("(no sound: the %s converts it generically)", sl.label);
            }
            ImGui::PopID();
        }
        if (e.has("note")) {
            ImGui::SeparatorText("Entry note");
            std::string note = e["note"].string();
            if (noteField("##entrynote", note)) {
                edit(d);
                if (note.empty()) e.remove("note");
                else sets(e, "note", note, kEntryOrder);
            }
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Duplicate")) {
        edit(d);
        JsonValue copy = e;
        auto& items = list.mutableItems();
        items.insert(items.begin() + index + 1, {std::string(), copy});
        d.sel[kits ? 1 : 0] = index + 1;
        d.scrollTo[kits ? 1 : 0] = index + 1;
    }
    ImGui::SetItemTooltip("A copy right after this entry: change its sound to make it the entry of another one.");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        edit(d);
        auto& items = list.mutableItems();
        items.erase(items.begin() + index);
        if (d.sel[kits ? 1 : 0] >= int(items.size())) d.sel[kits ? 1 : 0] = int(items.size()) - 1;
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------------
// Note maps

std::vector<std::pair<int, std::string>> ConvEditor::kitUsers(const Doc& d, const std::string& noteMap) const {
    std::vector<std::pair<int, std::string>> v;
    const JsonValue& kits = d.doc.root()["drumKits"];
    for (size_t i = 0; i < kits.size(); i++) {
        const JsonValue& e = kits[i];
        if (d.file == 0) {
            if (e["notes"].string() == noteMap) v.push_back({int(i), "to"});
        } else {
            for (const Slot& s : kSlots)
                if (e[s.key]["notes"].string() == noteMap) v.push_back({int(i), s.key});
        }
    }
    return v;
}

void ConvEditor::noteMapsView(Doc& d) {
    JsonValue& maps = *d.doc.root().find("noteMaps");
    if (!ImGui::BeginTable("split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.28f);
    ImGui::TableSetupColumn("notes", ImGuiTableColumnFlags_WidthStretch, 0.72f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##mapfilter", "Search", d.filter[2], sizeof d.filter[2]);
    if (ImGui::Button("New")) {
        edit(d);
        std::string name = "new-note-map";
        for (int i = 2; maps.find(name); i++) name = "new-note-map-" + std::to_string(i);
        maps.set(name, JsonValue::makeArray());
        d.selMap = name;
    }
    ImGui::SameLine();
    JsonValue* sel = d.selMap.empty() ? nullptr : maps.find(d.selMap);
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Duplicate") && sel) {
        edit(d);
        std::string name = d.selMap + "-copy";
        for (int i = 2; maps.find(name); i++) name = d.selMap + "-copy-" + std::to_string(i);
        JsonValue copy = *sel;
        maps.set(name, copy);
        d.selMap = name;
        sel = maps.find(name);
    }
    ImGui::SameLine();
    auto users = sel ? kitUsers(d, d.selMap) : std::vector<std::pair<int, std::string>>();
    ImGui::BeginDisabled(!users.empty());
    if (ImGui::Button("Delete") && sel) {
        edit(d);
        maps.remove(d.selMap);
        d.selMap.clear();
        sel = nullptr;
    }
    ImGui::EndDisabled();
    if (!users.empty()) ImGui::SetItemTooltip("Drum kits use this note map.");
    ImGui::EndDisabled();
    if (ImGui::BeginChild("maps", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        std::string f = lower(d.filter[2]);
        for (auto& [name, _] : maps.items()) {
            if (!f.empty() && lower(name).find(f) == std::string::npos) continue;
            if (ImGui::Selectable(name.c_str(), name == d.selMap)) {
                commitPending();
                d.selMap = name;
            }
            if (name == d.selMap && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.4f);
        }
    }
    ImGui::EndChild();

    ImGui::TableNextColumn();
    sel = d.selMap.empty() ? nullptr : maps.find(d.selMap);
    if (!sel) {
        ImGui::TextDisabled("Choose a note map. Drum kits name the note map that moves their notes to the target kit's.");
        ImGui::EndTable();
        return;
    }
    // Rename
    std::string name = d.selMap;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    {
        static char buf[256];
        snprintf(buf, sizeof buf, "%s", name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::InputText("##rename", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) && buf[0] && !maps.find(buf)) {
            edit(d);
            for (auto& [k, v] : maps.mutableItems())
                if (k == name) k = buf;
            // The drum kits that use it
            JsonValue& kits = *d.doc.root().find("drumKits");
            for (size_t i = 0; i < kits.size(); i++) {
                JsonValue& e = kits.at(i);
                if (d.file == 0 && e["notes"].string() == name) sets(e, "notes", buf, kEntryOrder);
                for (const Slot& s : kSlots)
                    if (JsonValue* t = e.find(s.key))
                        if ((*t)["notes"].string() == name) sets(*t, "notes", buf, kGsOrder);
            }
            d.selMap = buf;
            name = buf;
            sel = maps.find(name);
        }
        ImGui::SetItemTooltip("Press Enter to rename (the drum kits follow).");
    }
    // Who uses it: their kits name the notes.
    int srcMsbMap = -1, srcPc0 = 0, dstMsbMap = -1, dstPc0 = 0;
    std::string usedBy;
    const JsonValue& kits = d.doc.root()["drumKits"];
    for (auto& [i, slot] : users) {
        const JsonValue& e = kits[size_t(i)];
        const JsonValue& from = e["from"];
        const JsonValue& to = d.file == 0 ? e["to"] : e[slot.c_str()];
        if (srcMsbMap < 0) {
            srcMsbMap = d.file == 1 ? geti(from, "msb", 127) : geti(from, "map", 1);
            srcPc0 = geti(from, "pc", 1) - 1;
            dstMsbMap = d.file == 0 ? geti(to, "msb", 127) : geti(to, "map", 1);
            dstPc0 = geti(to, "pc", 1) - 1;
        }
        if (usedBy.size() < 300)
            usedBy += (usedBy.empty() ? "" : ", ") + from["name"].string() + " -> " + to["name"].string() +
                      (d.file == 0 ? "" : std::string(" (") + kMapNames[std::clamp(geti(to, "map", 1), 0, 4)] + ")");
    }
    ImGui::TextDisabled("Used by: %s", usedBy.empty() ? "no drum kit" : usedBy.c_str());
    const bool srcGs = d.file != 1, dstGs = d.file != 0;
    auto srcName = [&](int n) {
        if (srcMsbMap < 0) return std::string();
        return srcGs ? names_.gsNote(srcMsbMap, srcPc0, n) : names_.xgNote(srcMsbMap, srcPc0, n);
    };
    auto dstName = [&](int n) {
        if (dstMsbMap < 0) return std::string();
        return dstGs ? names_.gsNote(dstMsbMap, dstPc0, n) : names_.xgNote(dstMsbMap, dstPc0, n);
    };
    // The entry of each note (an entry can cover a range: "through").
    JsonValue& entries = *sel;
    std::array<int, 128> at;
    auto reindex = [&]() {
        at.fill(-1);
        for (size_t i = 0; i < entries.size(); i++) {
            int n = geti(entries[i], "note", -1), t = geti(entries[i], "through", n);
            for (int x = std::max(0, n); x <= std::min(127, t); x++) at[size_t(x)] = int(i);
        }
    };
    reindex();
    // The entry of one note alone: a range is split around it (the notes before, the note, the notes after);
    // a new entry plays the same note.
    auto own = [&](int note) -> int {
        int i = at[size_t(note)];
        auto& items = entries.mutableItems();
        if (i >= 0 && entries[size_t(i)].has("through")) {
            const JsonValue e = entries[size_t(i)];
            int n = geti(e, "note"), t = geti(e, "through");
            auto part = [&](int a, int b) {
                JsonValue c = e;
                seti(c, "note", a, kNoteOrder);
                if (b > a) seti(c, "through", b, kNoteOrder);
                else c.remove("through");
                return c;
            };
            items.erase(items.begin() + i);
            if (note < t) items.insert(items.begin() + i, {std::string(), part(note + 1, t)});
            items.insert(items.begin() + i, {std::string(), part(note, note)});
            if (note > n) items.insert(items.begin() + i++, {std::string(), part(n, note - 1)});
        }
        if (i < 0) {
            JsonValue c = JsonValue::makeObject();
            seti(c, "note", note, kNoteOrder);
            c.set("to", JsonValue::makeNumber(note), kNoteOrder);
            size_t pos = 0;
            while (pos < items.size() && geti(items[pos].second, "note") < note) pos++;
            items.insert(items.begin() + long(pos), {std::string(), c});
            i = int(pos);
        }
        reindex();
        return i;
    };
    ImGui::TextDisabled("Notes without an entry play their own key; \"Not played\" silences a note. Click a sound to hear it.");
    static const char* const modes[3] = {"Same note", "Not played", "Plays"};
    if (ImGui::BeginTable("notes", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        const float fs = ImGui::GetFontSize();
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, fs * 4.2f);
        ImGui::TableSetupColumn(srcGs ? "Sound Canvas sound" : "XG sound", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Converts to", ImGuiTableColumnFlags_WidthFixed, fs * 7.5f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, fs * 6.5f);
        ImGui::TableSetupColumn(dstGs ? "Sound Canvas sound" : "XG sound", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Velocity", ImGuiTableColumnFlags_WidthFixed, fs * 6.5f);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(128);
        while (clip.Step()) {
            for (int n = clip.DisplayStart; n < clip.DisplayEnd; n++) {
                ImGui::TableNextRow();
                ImGui::PushID(n);
                auto entryOf = [&]() -> const JsonValue* { return at[size_t(n)] >= 0 ? &entries[size_t(at[size_t(n)])] : nullptr; };
                const JsonValue* e = entryOf();
                int mode = !e ? 0 : (*e)["to"].isNull() ? 1 : 2;
                int to = mode == 2 ? geti(*e, "to", n) : n;
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%3d %s", n, noteLabel(n).c_str());
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                std::string sn = srcName(n);
                if (sn.empty()) ImGui::TextDisabled("-");
                else ImGui::TextUnformatted(sn.c_str());
                if (!sn.empty() && outName_[srcGs ? 1 : 0] != MidiOutputs::kNone && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
                    playDrum(srcGs, srcMsbMap, srcPc0, n);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##mode", modes[mode])) {
                    for (int m = 0; m < 3; m++)
                        if (ImGui::Selectable(modes[m], m == mode) && m != mode) {
                            edit(d);
                            int i = own(n);
                            if (m == 0) {
                                auto& items = entries.mutableItems();
                                items.erase(items.begin() + i);
                                reindex();
                            } else {
                                JsonValue& x = entries.at(size_t(i));
                                if (m == 1) x.set("to", JsonValue(), kNoteOrder);
                                else x.set("to", JsonValue::makeNumber(n), kNoteOrder);
                                if (!x.has("name") && !sn.empty()) sets(x, "name", sn, kNoteOrder);
                            }
                            e = entryOf();
                            mode = !e ? 0 : (*e)["to"].isNull() ? 1 : 2;
                            to = mode == 2 ? geti(*e, "to", n) : n;
                        }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn();
                if (mode == 2) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    int v = to;
                    if (ImGui::InputInt("##to", &v)) {
                        v = std::clamp(v, 0, 127);
                        edit(d);
                        entries.at(size_t(own(n))).set("to", JsonValue::makeNumber(v), kNoteOrder);
                        e = entryOf();
                        to = v;
                    }
                }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (mode != 1) {
                    std::string dn = dstName(to);
                    std::string label = noteLabel(to) + "  " + (dn.empty() ? std::string("(no sound)") : dn);
                    if (dn.empty()) ImGui::TextDisabled("%s", label.c_str());
                    else ImGui::TextUnformatted(label.c_str());
                    if (!dn.empty() && outName_[dstGs ? 1 : 0] != MidiOutputs::kNone && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
                        playDrum(dstGs, dstMsbMap, dstPc0, to);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                if (mode != 0 && e) {
                    int vel = geti(*e, "velocity");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputInt("##vel", &vel)) {
                        edit(d);
                        JsonValue& x = entries.at(size_t(own(n)));
                        vel = std::clamp(vel, -127, 127);
                        if (vel) seti(x, "velocity", vel, kNoteOrder);
                        else x.remove("velocity");
                        e = entryOf();
                    }
                }
                ImGui::TableNextColumn();
                if (mode != 0 && e) {
                    std::string c = (*e)["name"].string();
                    if (inputString("##comment", c)) {
                        edit(d);
                        JsonValue& x = entries.at(size_t(own(n)));
                        if (c.empty()) x.remove("name");
                        else sets(x, "name", c, kNoteOrder);
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------------------------------
// Rules

void ConvEditor::rulesView(Doc& d) {
    JsonValue& rules = *d.doc.root().find("rules");
    if (!ImGui::BeginTable("split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableSetupColumn("rules", ImGuiTableColumnFlags_WidthStretch, 0.75f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("Controller rules scale what a song sends (value * scale / 100 + offset). ImMidi picks a set by the modules:");
    static const char* const keys[3] = {"sc55, sc88, sc88pro (the song's map)", "sc55 (the SC-55), sc88 (the others)",
                                        "sc88pro-sc88, sc88pro-sc55, sc88-sc55, sc55-sc88, sc55-sc88pro (song map - module map)"};
    ImGui::TextDisabled("%s", keys[d.file]);
    if (ImGui::Button("New set")) {
        edit(d);
        std::string name = "new";
        for (int i = 2; rules.find(name); i++) name = "new-" + std::to_string(i);
        rules.set(name, JsonValue::makeObject());
        d.selRules = name;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!rules.find(d.selRules));
    if (ImGui::Button("Delete set")) {
        edit(d);
        rules.remove(d.selRules);
        d.selRules.clear();
    }
    ImGui::EndDisabled();
    if (ImGui::BeginChild("sets", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (auto& [name, _] : rules.items())
            if (ImGui::Selectable(name.c_str(), name == d.selRules)) {
                commitPending();
                d.selRules = name;
            }
    }
    ImGui::EndChild();
    ImGui::TableNextColumn();
    JsonValue* set = rules.find(d.selRules);
    if (!set) {
        ImGui::TextDisabled("Choose a set of rules.");
        ImGui::EndTable();
        return;
    }
    {
        static char buf[128];
        snprintf(buf, sizeof buf, "%s", d.selRules.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::InputText("##setname", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) && buf[0] && !rules.find(buf)) {
            edit(d);
            for (auto& [k, v] : rules.mutableItems())
                if (k == d.selRules) k = buf;
            d.selRules = buf;
            set = rules.find(d.selRules);
        }
        ImGui::SetItemTooltip("Press Enter to rename.");
    }
    auto table = [&](const char* key, bool nrpn) {
        ImGui::SeparatorText(nrpn ? "NRPN (MSB, LSB: data entry values)" : "Controllers");
        JsonValue* list = set->find(key);
        int del = -1;
        if (list && list->size() && ImGui::BeginTable(key, nrpn ? 7 : 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            const float fs = ImGui::GetFontSize();
            if (nrpn) {
                ImGui::TableSetupColumn("MSB", ImGuiTableColumnFlags_WidthFixed, fs * 6);
                ImGui::TableSetupColumn("LSB", ImGuiTableColumnFlags_WidthFixed, fs * 6);
            } else {
                ImGui::TableSetupColumn("CC", ImGuiTableColumnFlags_WidthFixed, fs * 6);
            }
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Scale %", ImGuiTableColumnFlags_WidthFixed, fs * 6);
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, fs * 6);
            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 2.5f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < list->size(); i++) {
                JsonValue& r = list->at(i);
                ImGui::TableNextRow();
                ImGui::PushID(int(i));
                auto num = [&](const char* k, int lo, int hi) {
                    ImGui::TableNextColumn();
                    int v = geti(r, k);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputInt((std::string("##") + k).c_str(), &v)) {
                        edit(d);
                        seti(r, k, std::clamp(v, lo, hi), kRuleOrder);
                    }
                };
                if (nrpn) {
                    num("msb", 0, 127);
                    num("lsb", 0, 127);
                } else {
                    num("cc", 0, 127);
                }
                ImGui::TableNextColumn();
                std::string n = r["name"].string();
                if (inputString("##name", n)) {
                    edit(d);
                    sets(r, "name", n, kRuleOrder);
                }
                num("scale", 0, 1000);
                num("offset", -127, 127);
                ImGui::TableNextColumn();
                std::string note = r["note"].string();
                if (inputString("##note", note)) {
                    edit(d);
                    if (note.empty()) r.remove("note");
                    else sets(r, "note", note, kRuleOrder);
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) del = int(i);
                ImGui::SetItemTooltip("Delete this rule");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (del >= 0) {
            edit(d);
            auto& items = list->mutableItems();
            items.erase(items.begin() + del);
            if (items.empty()) set->remove(key);
        }
        if (ImGui::Button(nrpn ? "Add an NRPN rule" : "Add a controller rule")) {
            edit(d);
            if (!set->find(key)) set->set(key, JsonValue::makeArray(), {"controllers", "nrpn", "velocity"});
            JsonValue r = JsonValue::makeObject();
            if (nrpn) {
                seti(r, "msb", 1, kRuleOrder);
                seti(r, "lsb", 99, kRuleOrder);
            } else {
                seti(r, "cc", 7, kRuleOrder);
            }
            seti(r, "scale", 100, kRuleOrder);
            seti(r, "offset", 0, kRuleOrder);
            sets(r, "name", "", kRuleOrder);
            set->find(key)->append(r);
        }
    };
    table("controllers", false);
    table("nrpn", true);
    ImGui::SeparatorText("Note velocity");
    bool vel = set->has("velocity");
    if (ImGui::Checkbox("Scale note velocities", &vel)) {
        edit(d);
        if (vel) {
            JsonValue v = JsonValue::makeObject();
            seti(v, "scale", 100, kRuleOrder);
            seti(v, "offset", 0, kRuleOrder);
            set->set("velocity", v, {"controllers", "nrpn", "velocity"});
        } else {
            set->remove("velocity");
        }
    }
    if (JsonValue* v = set->find("velocity")) {
        const float w = ImGui::GetFontSize() * 6.5f;
        int sc = geti(*v, "scale", 100), of = geti(*v, "offset");
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt("Scale %", &sc)) {
            edit(d);
            seti(*v, "scale", std::clamp(sc, 0, 1000), kRuleOrder);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt("Offset", &of)) {
            edit(d);
            seti(*v, "offset", std::clamp(of, -127, 127), kRuleOrder);
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------------------------------
// About

void ConvEditor::aboutView(Doc& d) {
    ImGui::TextDisabled("The table's description: one paragraph per line.");
    std::string text;
    const JsonValue& about = d.doc.root()["about"];
    for (size_t i = 0; i < about.size(); i++) text += (i ? "\n" : "") + about[i].string();
    static std::vector<char> buf;
    buf.assign(text.begin(), text.end());
    buf.resize(text.size() + 16384, '\0');
    if (ImGui::InputTextMultiline("##about", buf.data(), buf.size(), ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_WordWrap)) {
        edit(d);
        JsonValue arr = JsonValue::makeArray();
        std::string all(buf.data());
        size_t p = 0;
        while (p <= all.size()) {
            size_t e = all.find('\n', p);
            if (e == std::string::npos) e = all.size();
            arr.append(JsonValue::makeString(all.substr(p, e - p)));
            p = e + 1;
        }
        d.doc.root().set("about", arr, {"format", "version", "from", "to", "about"});
    }
}

// ---------------------------------------------------------------------------------------------------
// Picker, names, dialogs

void ConvEditor::openPicker(PickKind kind, int map, int maxMap, const SoundNames::Item& current,
                            std::function<void(const SoundNames::Item&)> done) {
    pickKind_ = kind;
    pickMaxMap_ = std::clamp(maxMap, 1, 4);
    pickMap_ = std::clamp(map, 1, pickMaxMap_);
    switch (kind) {
    case PickKind::GsVoice: pickItems_ = names_.gsVoices(pickMap_); break;
    case PickKind::GsKit: pickItems_ = names_.gsKits(pickMap_); break;
    case PickKind::XgVoice: pickItems_ = names_.xgVoices(); break;
    case PickKind::XgKit: pickItems_ = names_.xgKits(); break;
    }
    pickDone_ = std::move(done);
    pickFilter_[0] = 0;
    pickSel_ = -1;
    for (size_t i = 0; i < pickItems_.size() && current.pc0 >= 0; i++) {
        const SoundNames::Item& it = pickItems_[i];
        if (it.a == current.a && it.b == current.b && it.pc0 == current.pc0) pickSel_ = int(i);
    }
    pickScroll_ = pickSel_ >= 0 ? 1 : 0;
    pickerOpen_ = true;
}

void ConvEditor::pickerPopup() {
    if (pickerOpen_) {
        ImGui::OpenPopup("Choose a sound");
        pickerOpen_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.55f, vp->WorkSize.y * 0.75f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Choose a sound", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const bool gs = pickKind_ == PickKind::GsVoice || pickKind_ == PickKind::GsKit;
    const bool kit = pickKind_ == PickKind::GsKit || pickKind_ == PickKind::XgKit;
    if (gs) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Map");
        for (int m = 1; m <= std::max(1, pickMaxMap_); m++) {
            ImGui::SameLine();
            if (ImGui::RadioButton(kMapNames[m], pickMap_ == m)) {
                pickMap_ = m;
                pickItems_ = kit ? names_.gsKits(m) : names_.gsVoices(m);
                pickSel_ = -1;
            }
        }
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##pickfilter", "Search (names, numbers)", pickFilter_, sizeof pickFilter_);
    std::string f = lower(pickFilter_);
    std::vector<int> rows;
    for (size_t i = 0; i < pickItems_.size(); i++) {
        const SoundNames::Item& it = pickItems_[i];
        if (!f.empty()) {
            char nums[32];
            snprintf(nums, sizeof nums, "%d/%d", it.b, it.pc0 + 1);
            if (lower(it.name).find(f) == std::string::npos && std::string(nums).find(f) == std::string::npos) continue;
        }
        rows.push_back(int(i));
    }
    bool chosen = false;
    // Up and Down move through the list; Enter chooses (the only sound the search finds, too).
    if (!rows.empty() && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) {
        int r = int(std::find(rows.begin(), rows.end(), pickSel_) - rows.begin());
        if (r >= int(rows.size())) r = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 0 : int(rows.size()) - 1;
        else r = std::clamp(r + (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : -1), 0, int(rows.size()) - 1);
        pickSel_ = rows[size_t(r)];
        pickScroll_ = 2;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (rows.size() == 1) pickSel_ = rows[0];
        chosen = pickSel_ >= 0;
    }
    int scrollRow = -1;
    for (size_t r = 0; r < rows.size() && pickScroll_; r++)
        if (rows[r] == pickSel_) scrollRow = int(r);
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.3f;
    if (ImGui::BeginTable("items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(gs ? (kit ? "Map" : "Bank") : (kit ? "MSB" : "MSB/LSB"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
        ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(rows.size()));
        if (scrollRow >= 0) clip.IncludeItemByIndex(scrollRow);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const SoundNames::Item& it = pickItems_[size_t(rows[size_t(r)])];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char b[32];
                if (gs) snprintf(b, sizeof b, "%d", kit ? it.a : it.b);
                else if (kit) snprintf(b, sizeof b, "%d", it.a);
                else snprintf(b, sizeof b, "%d/%d", it.a, it.b);
                ImGui::PushID(r);
                if (ImGui::Selectable(b, pickSel_ == rows[size_t(r)], ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    pickSel_ = rows[size_t(r)];
                    if (ImGui::IsMouseDoubleClicked(0)) chosen = true;
                }
                if (r == scrollRow) ImGui::ScrollToItem(pickScroll_ == 1 ? ImGuiScrollFlags_AlwaysCenterY : ImGuiScrollFlags_KeepVisibleEdgeY);
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%d", it.pc0 + 1);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(it.name.c_str());
            }
        ImGui::EndTable();
    }
    pickScroll_ = 0;
    ImGui::BeginDisabled(pickSel_ < 0);
    if (ImGui::Button("Choose")) chosen = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pickDone_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    if (chosen && pickSel_ >= 0 && pickSel_ < int(pickItems_.size())) {
        auto done = std::move(pickDone_);
        SoundNames::Item it = pickItems_[size_t(pickSel_)];
        ImGui::CloseCurrentPopup();
        if (done) done(it);
    }
    ImGui::EndPopup();
}

void ConvEditor::namesPopup() {
    if (showNames_) {
        ImGui::OpenPopup("Names and the instrument definitions");
        showNames_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.6f, vp->WorkSize.y * 0.6f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Names and the instrument definitions", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextWrapped("The tables name each sound as the instrument definitions (insdef) do, so a wrong number shows up. These names do "
                       "not match the sounds their numbers select:");
    if (ImGui::BeginChild("issues", ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.3f), ImGuiChildFlags_Borders)) {
        if (nameIssues_.empty()) ImGui::TextDisabled("None.");
        for (size_t i = 0; i < nameIssues_.size() && i < 500; i++) {
            const NameIssue& n = nameIssues_[i];
            ImGui::PushID(int(i));
            if (ImGui::SmallButton("Show")) {
                Doc& d = docs_[n.file];
                current_ = n.file;
                selectTab_ = true;
                d.view = n.kit ? 1 : 0;
                d.selectView = true;
                d.sel[n.kit ? 1 : 0] = n.entry;
                d.scrollTo[n.kit ? 1 : 0] = n.entry;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::Text("%s, %s %d, %s: \"%s\" - the definitions: %s", kDocTitle[n.file], n.kit ? "drum kit" : "voice", n.entry + 1, n.slot.c_str(),
                        n.stored.c_str(), n.expected.empty() ? "no sound" : ("\"" + n.expected + "\"").c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Use the definitions' names")) {
        for (const NameIssue& n : nameIssues_) {
            if (n.expected.empty()) continue;
            Doc& d = docs_[n.file];
            JsonValue* list = d.doc.root().find(n.kit ? "drumKits" : "voices");
            if (!list || n.entry >= int(list->size())) continue;
            JsonValue* s = list->at(size_t(n.entry)).find(n.slot);
            if (!s) continue;
            if (pendingDoc_ != &d) {
                commitPending();
                pendingDoc_ = &d;
                pendingId_ = 0;
                pendingText_ = d.doc.serialize();
                pendingVersion_ = d.version;
            }
            d.version = ++lastVersion_;
            sets(*s, "name", n.expected, s->has("map") ? kGsOrder : kXgOrder);
        }
        commitPending();
        validate();
    }
    ImGui::SetItemTooltip("Renames the sounds the definitions know (the others keep their names).");
    ImGui::SameLine();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ConvEditor::dialogs() {
    if (confirmDiscard_) {
        ImGui::OpenPopup("Unsaved changes##discard");
        confirmDiscard_ = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes##discard", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("The tables have changes that are not saved.");
        if (ImGui::Button("Save them")) {
            std::string dir = pendingFolder_;
            requestSave([this, dir]() { openFolder(dir); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard them")) {
            openFolder(pendingFolder_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (closeAsked_) {
        ImGui::OpenPopup("Unsaved changes##close");
        closeAsked_ = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes##close", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Save the changes to the tables before quitting?");
        if (ImGui::Button("Save and quit")) {
            requestSave([this]() { quit_ = true; });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit without saving")) {
            quit_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (confirmInvalidSave_) {
        ImGui::OpenPopup("Save tables ImMidi cannot load?");
        confirmInvalidSave_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Save tables ImMidi cannot load?", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", validError_.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("With tables it cannot load, ImMidi converts without them (its settings show this error) until they are "
                           "corrected.");
        if (ImGui::Button("Save anyway")) {
            auto then = std::move(afterSave_);
            afterSave_ = nullptr;
            if (save() && then) then();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            afterSave_ = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (askFolderPath_) {
        ImGui::OpenPopup("Open folder");
        askFolderPath_ = false;
    }
    if (ImGui::BeginPopupModal("Open folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("The folder with gs-to-xg.json, xg-to-gs.json and gs-maps.json:");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 36);
        ImGui::InputText("##folder", folderInput_, sizeof folderInput_);
        if (ImGui::Button("Open")) {
            if (dirty()) {
                pendingFolder_ = folderInput_;
                confirmDiscard_ = true;
            } else {
                openFolder(folderInput_);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (showAbout_) {
        ImGui::OpenPopup("About the conversion tables");
        showAbout_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("About the conversion tables", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            "ImMidi's emulation converts songs for one sound module to another with these tables (Settings > Sound module > Use the "
            "instrument conversion tables). For each sound and drum kit they give the sound that plays it on the other modules, with a "
            "volume and attack correction; the note maps move drum notes to the target kit's keys; the rules scale controllers.");
        ImGui::Spacing();
        ImGui::TextWrapped("Program numbers (PC) are 1-128 as in the manuals. Sound Canvas sounds: map = CC#32 (1 SC-55, 2 SC-88, 3 "
                           "SC-88Pro, 4 SC-8850), bank = CC#0. XG sounds: MSB = CC#0, LSB = CC#32.");
        ImGui::Spacing();
        ImGui::TextWrapped("Saving keeps each file's previous version as <name>.json.bak (once per session). ImMidi and ImMidi Bridge "
                           "read the tables when they start.");
        ImGui::Spacing();
        ImGui::TextDisabled("Instrument definitions: %s", insdefFolder_.empty() ? "not found" : insdefFolder_.c_str());
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------------
// Audition

void ConvEditor::auditionMenu() {
    if (!ImGui::BeginMenu("Audition")) return;
    if (ImGui::IsWindowAppearing()) devices_ = outputs_.availableDevices();
    static const char* const labels[2] = {"XG module", "GS module (Sound Canvas)"};
    for (int p = 0; p < 2; p++) {
        if (ImGui::BeginMenu(labels[p])) {
            for (const std::string& dev : devices_) {
                if (ImGui::MenuItem(dev.c_str(), nullptr, dev == outName_[p])) {
                    std::string err;
                    if (outputs_.setPortDevice(p, dev, &err)) outName_[p] = dev;
                    else {
                        outName_[p] = MidiOutputs::kNone;
                        message_ = "Could not open " + dev + ": " + err;
                        messageUntil_ = Clock::now() + std::chrono::seconds(6);
                    }
                    saveSettings();
                }
            }
            ImGui::EndMenu();
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset the modules")) {
        if (outName_[0] != MidiOutputs::kNone) outputs_.send(0, xgSystemOn());
        if (outName_[1] != MidiOutputs::kNone) outputs_.send(1, gsReset());
    }
    ImGui::TextDisabled("Play buttons sound a voice on channel 1 and a drum");
    ImGui::TextDisabled("kit's notes on channel 10 (click a note's sound).");
    ImGui::EndMenu();
}

void ConvEditor::send(int port, std::initializer_list<uint8_t> m) {
    std::vector<uint8_t> v(m);
    outputs_.send(port, v);
}

void ConvEditor::playVoice(bool gs, int a, int b, int pc0) {
    int port = gs ? 1 : 0;
    send(port, {0xB0, 0, uint8_t(a)});
    send(port, {0xB0, 32, uint8_t(b)});
    send(port, {0xC0, uint8_t(pc0)});
    send(port, {0x90, 60, 100});
    noteOffs_.push_back({Clock::now() + std::chrono::milliseconds(900), port, 0, 60});
}

void ConvEditor::playDrum(bool gs, int a, int pc0, int note) {
    int port = gs ? 1 : 0;
    send(port, {0xB9, 0, uint8_t(gs ? 0 : a)});
    send(port, {0xB9, 32, uint8_t(gs ? a : 0)});
    send(port, {0xC9, uint8_t(pc0)});
    send(port, {0x99, uint8_t(note), 100});
    noteOffs_.push_back({Clock::now() + std::chrono::milliseconds(600), port, 9, uint8_t(note)});
}

} // namespace immidi

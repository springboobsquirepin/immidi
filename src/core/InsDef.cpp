#include "InsDef.h"
#include "TextCodec.h"
#include "Util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace immidi {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

// Parses "123" or "*" (returns -1 for wildcard).
bool parseIndex(const std::string& s, int& out) {
    std::string t = trim(s);
    if (t == "*") {
        out = -1;
        return true;
    }
    if (t.empty()) return false;
    char* end = nullptr;
    long v = strtol(t.c_str(), &end, 10);
    if (!end || *end) return false;
    out = int(v);
    return true;
}

} // namespace

int InsLibrary::loadDirectory(const std::string& dir) {
    std::error_code ec;
    std::vector<std::string> paths;
    for (auto& entry : std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) {
        if (!entry.is_regular_file()) continue;
        std::string p = pathToUtf8(entry.path());
        if (endsWithNoCase(p, ".ins")) paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());
    int n = 0;
    for (const std::string& p : paths) {
        // The same definition file may exist in several search folders; load it once.
        std::string name = lowerAscii(fileNameOf(p));
        bool dup = false;
        for (auto& f : files_)
            if (lowerAscii(fileNameOf(f->path)) == name) dup = true;
        if (!dup && loadFile(p)) n++;
    }
    return n;
}

const NameList* InsLibrary::generatedList(const std::string& spec) {
    // "1..128" or "0..127": numeric names.
    size_t dots = spec.find("..");
    if (dots == std::string::npos) return nullptr;
    int a = atoi(spec.substr(0, dots).c_str());
    int b = atoi(spec.substr(dots + 2).c_str());
    if (b < a || b - a > 16384) return nullptr;
    auto& slot = generated_[spec];
    if (!slot) {
        slot = std::make_unique<NameList>();
        for (int i = 0; i <= b - a && i < 128; i++) (*slot)[i] = std::to_string(a + i);
    }
    return slot.get();
}

const NameList* InsLibrary::resolvePatchList(const FileData& f, const std::string& name) {
    if (const NameList* g = generatedList(name)) return g;
    auto it = f.patchLists.find(name);
    if (it != f.patchLists.end()) return it->second.get();
    for (auto& other : files_) {
        auto jt = other->patchLists.find(name);
        if (jt != other->patchLists.end()) return jt->second.get();
    }
    return nullptr;
}

const NameList* InsLibrary::resolveNoteList(const FileData& f, const std::string& name) {
    if (const NameList* g = generatedList(name)) return g;
    auto it = f.noteLists.find(name);
    if (it != f.noteLists.end()) return it->second.get();
    for (auto& other : files_) {
        auto jt = other->noteLists.find(name);
        if (jt != other->noteLists.end()) return jt->second.get();
    }
    return nullptr;
}

bool InsLibrary::loadFile(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) return false;
    std::string text = decodeText(bytes.data(), bytes.size(), TextEncoding::Auto);

    auto file = std::make_unique<FileData>();
    file->path = path;

    enum class Sec { None, Patch, Note, Other, Instrument } sec = Sec::None;
    NameList* curList = nullptr;
    std::map<std::string, std::string> basedOnPatch, basedOnNote;
    std::string curName;
    struct PendingInstrument {
        std::string name;
        std::vector<std::pair<std::string, std::string>> lines;
    };
    std::vector<PendingInstrument> pendingInstruments;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '.') {
            std::string s = lowerAscii(line);
            if (s.rfind(".patch names", 0) == 0) sec = Sec::Patch;
            else if (s.rfind(".note names", 0) == 0) sec = Sec::Note;
            else if (s.rfind(".instrument definitions", 0) == 0) sec = Sec::Instrument;
            else sec = Sec::Other;
            curList = nullptr;
            continue;
        }
        if (line[0] == '[') {
            size_t e = line.find(']');
            curName = trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            curList = nullptr;
            if (sec == Sec::Patch) {
                auto& slot = file->patchLists[curName];
                if (!slot) slot = std::make_unique<NameList>();
                curList = slot.get();
            } else if (sec == Sec::Note) {
                auto& slot = file->noteLists[curName];
                if (!slot) slot = std::make_unique<NameList>();
                curList = slot.get();
            } else if (sec == Sec::Instrument) {
                pendingInstruments.push_back({curName, {}});
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (sec == Sec::Patch || sec == Sec::Note) {
            if (!curList) continue;
            if (lowerAscii(key) == "basedon") {
                (sec == Sec::Patch ? basedOnPatch : basedOnNote)[curName] = val;
                continue;
            }
            int idx;
            if (parseIndex(key, idx) && idx >= 0 && idx < 128) (*curList)[idx] = val;
        } else if (sec == Sec::Instrument && !pendingInstruments.empty()) {
            pendingInstruments.back().lines.push_back({key, val});
        }
    }

    FileData* raw = file.get();
    files_.push_back(std::move(file));
    // Apply BasedOn inheritance (base entries fill gaps).
    auto applyBase = [&](std::map<std::string, std::unique_ptr<NameList>>& lists, std::map<std::string, std::string>& based, bool note) {
        for (int pass = 0; pass < 4; pass++) {
            for (auto& [name, baseName] : based) {
                auto it = lists.find(name);
                if (it == lists.end()) continue;
                const NameList* base = note ? resolveNoteList(*raw, baseName) : resolvePatchList(*raw, baseName);
                if (!base) {
                    auto jt = lists.find(baseName);
                    if (jt != lists.end()) base = jt->second.get();
                }
                if (!base || base == it->second.get()) continue;
                for (auto& [k, v] : *base) it->second->emplace(k, v);
            }
        }
    };
    applyBase(raw->patchLists, basedOnPatch, false);
    applyBase(raw->noteLists, basedOnNote, true);

    for (PendingInstrument& pi : pendingInstruments) {
        auto ins = std::make_unique<InsInstrument>();
        ins->name = pi.name;
        ins->file = path;
        definitions_.push_back({ins.get(), raw, std::move(pi.lines)});
        instruments_.push_back(std::move(ins));
    }
    // This file may complete definitions of earlier files as well as its own.
    for (Definition& d : definitions_)
        if (!d.complete) resolve(d);
    return true;
}

void InsLibrary::resolve(Definition& d) {
    InsInstrument& ins = *d.ins;
    ins.patchByBank.clear();
    ins.patchAnyBank = nullptr;
    ins.keys.clear();
    ins.drums.clear();
    bool complete = true;
    for (auto& [key, val] : d.lines) {
        std::string k = lowerAscii(key);
        size_t lb = k.find('['), rb = k.find(']');
        if (lb == std::string::npos || rb == std::string::npos || rb < lb) continue;
        std::string head = k.substr(0, lb);
        std::string args = key.substr(lb + 1, rb - lb - 1);
        if (head == "patch") {
            int bank;
            if (!parseIndex(args, bank)) continue;
            const NameList* l = resolvePatchList(*d.file, val);
            if (!l) {
                complete = false;
                continue;
            }
            if (bank < 0) ins.patchAnyBank = l;
            else ins.patchByBank[bank] = l;
        } else if (head == "key" || head == "drum") {
            size_t comma = args.find(',');
            if (comma == std::string::npos) continue;
            int bank, prog;
            if (!parseIndex(args.substr(0, comma), bank) || !parseIndex(args.substr(comma + 1), prog)) continue;
            if (head == "key") {
                if (const NameList* l = resolveNoteList(*d.file, val)) ins.keys[{bank, prog}] = l;
                else complete = false;
            } else {
                ins.drums[{bank, prog}] = atoi(val.c_str()) != 0;
            }
        }
    }
    d.complete = complete;
}

const InsInstrument* InsLibrary::find(const std::string& name) const {
    for (auto& i : instruments_)
        if (i->name == name) return i.get();
    return nullptr;
}

std::vector<std::string> InsLibrary::instrumentNames() const {
    std::vector<std::string> v;
    for (auto& i : instruments_) v.push_back(i->name);
    return v;
}

const NameList* InsLibrary::patchList(const InsInstrument* ins, int bank) {
    if (!ins) return nullptr;
    auto it = ins->patchByBank.find(bank);
    if (it != ins->patchByBank.end()) return it->second;
    return ins->patchAnyBank;
}

std::string InsLibrary::patchName(const InsInstrument* ins, int bank, int program) {
    if (!ins) return {};
    const NameList* l = patchList(ins, bank);
    if (l) {
        auto it = l->find(program);
        if (it != l->end()) return it->second;
    }
    return {};
}

const NameList* InsLibrary::noteList(const InsInstrument* ins, int bank, int program) {
    if (!ins) return nullptr;
    const std::pair<int, int> tries[4] = {{bank, program}, {bank, -1}, {-1, program}, {-1, -1}};
    for (auto& t : tries) {
        auto it = ins->keys.find(t);
        if (it != ins->keys.end()) return it->second;
    }
    return nullptr;
}

std::string InsLibrary::noteName(const InsInstrument* ins, int bank, int program, int note) {
    const NameList* l = noteList(ins, bank, program);
    if (!l) return {};
    auto it = l->find(note);
    return it != l->end() ? it->second : std::string();
}

} // namespace immidi

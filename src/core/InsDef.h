#pragma once
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace immidi {

// Name list from a .ins "[section]": program/note number -> name.
using NameList = std::map<int, std::string>;

struct InsInstrument {
    std::string name;
    std::string file;
    std::map<int, const NameList*> patchByBank;  // bank = MSB*128 + LSB
    const NameList* patchAnyBank = nullptr;       // Patch[*]
    std::map<std::pair<int, int>, const NameList*> keys;  // (bank|-1, prog|-1) -> note names
    std::map<std::pair<int, int>, bool> drums;            // Drum[bank|-1, prog|-1]
    bool isDrumDefinition() const { return !drums.empty(); }
};

class InsLibrary {
public:
    // Loads every *.ins file in a directory (non-recursive). Returns the number of files loaded.
    int loadDirectory(const std::string& dir);
    bool loadFile(const std::string& path);

    const std::vector<std::unique_ptr<InsInstrument>>& instruments() const { return instruments_; }
    const InsInstrument* find(const std::string& name) const;
    std::vector<std::string> instrumentNames() const;

    // Name lookup helpers (return empty string when unknown).
    static std::string patchName(const InsInstrument* ins, int bank, int program);
    static const NameList* patchList(const InsInstrument* ins, int bank);
    static std::string noteName(const InsInstrument* ins, int bank, int program, int note);
    static const NameList* noteList(const InsInstrument* ins, int bank, int program);

private:
    struct FileData {
        std::string path;
        std::map<std::string, std::unique_ptr<NameList>> patchLists;
        std::map<std::string, std::unique_ptr<NameList>> noteLists;
    };
    // An instrument definition's lines, kept until every list it names has been found: a definition
    // may use lists of a file loaded after its own (Yamaha MU2000.ins uses Yamaha XG.ins's drum notes).
    struct Definition {
        InsInstrument* ins;
        const FileData* file;
        std::vector<std::pair<std::string, std::string>> lines;
        bool complete = false;
    };
    const NameList* resolvePatchList(const FileData& f, const std::string& name);
    const NameList* resolveNoteList(const FileData& f, const std::string& name);
    const NameList* generatedList(const std::string& spec);
    void resolve(Definition& d);

    std::vector<std::unique_ptr<FileData>> files_;
    std::vector<std::unique_ptr<InsInstrument>> instruments_;
    std::vector<Definition> definitions_;
    std::map<std::string, std::unique_ptr<NameList>> generated_;
};

} // namespace immidi

#pragma once
#include "Config.h"
#include "ConversionDoc.h"
#include "InsDef.h"
#include "MidiOut.h"
#include "NativeDialogs.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace immidi {

// The instrument definitions (insdef/) the editor names sounds with, as ImMidi's checks do: maps 1-3 of
// the SC-88 Pro definitions, map 4 of the SC-8850's, XG sounds from Yamaha XG and MU2000.
class SoundNames {
public:
    struct Item {
        int a = 0, b = 0, pc0 = 0;  // GS: map, variation; XG: MSB, LSB (kits: b unused)
        std::string name;
    };
    std::string load();  // the folder the definitions came from ("" when none was found)
    bool loaded() const { return gs_ && xg_; }

    std::string gsVoice(int map, int var, int pc0) const;
    std::string gsKit(int map, int pc0) const;
    std::string xgVoice(int msb, int lsb, int pc0) const;  // XG, then MU2000
    bool xgVoiceNamed(int msb, int lsb, int pc0, const std::string& name) const;
    std::string xgKit(int msb, int pc0) const;
    bool xgKitNamed(int msb, int pc0, const std::string& name) const;
    std::string gsNote(int map, int pc0, int note) const;
    std::string xgNote(int msb, int pc0, int note) const;

    std::vector<Item> gsVoices(int map) const;
    std::vector<Item> gsKits(int map) const;
    std::vector<Item> xgVoices() const;
    std::vector<Item> xgKits() const;

private:
    InsLibrary lib_;
    const InsInstrument *gs_ = nullptr, *sc8850_ = nullptr, *gsKits_ = nullptr, *sc8850Kits_ = nullptr;
    const InsInstrument *xg_ = nullptr, *mu2000_ = nullptr, *xgKits_ = nullptr, *mu2000Kits_ = nullptr;
};

// ImMidi Conversion Editor: edits the conversion tables (conversion/*.json) — the voices, drum kits,
// note maps and controller rules ImMidi's emulation converts songs with.
class ConvEditor {
public:
    explicit ConvEditor(GLFWwindow* window);
    ~ConvEditor();

    void frame();
    bool dirty() const;
    // Something changes on its own soon (audition notes to end, a check to run, a dialog).
    bool busy() const;
    void requestClose();  // the window's close button
    bool quitRequested() const { return quit_; }
    std::string windowTitle() const;

private:
    struct Doc {
        int file = 0;  // index in ConversionTables::kFiles
        ConversionDoc doc;
        std::string path, loadError;
        bool loaded = false, backedUp = false;
        // Each state of the document has its own version: undo and redo bring the state's version back.
        uint64_t version = 1, savedVersion = 1;
        struct Step {
            std::string text;
            uint64_t version;
        };
        std::vector<Step> undo, redo;
        // View state
        int view = 0;
        int sel[2] = {-1, -1};  // voices, drum kits
        std::string selMap, selRules;
        char filter[3][96] = {};  // voices, drum kits, note maps
        std::vector<int> rows[2];
        uint64_t rowsVersion[2] = {0, 0};
        std::string rowsFilter[2];
        int scrollTo[2] = {-1, -1};
        bool selectView = true;  // show the view `view` (set from code)
    };
    enum class PickKind { GsVoice, GsKit, XgVoice, XgKit };
    struct NoteOff {
        std::chrono::steady_clock::time_point at;
        int port;
        uint8_t ch, note;
    };
    struct NameIssue {
        int file;
        bool kit;
        int entry;
        std::string slot;  // "from", "to" or a target key
        std::string stored, expected;
    };

    // Files
    void openFolder(const std::string& dir);
    bool save();
    // Saves (Ctrl+S, the menu, the dialogs): tables ImMidi cannot load are saved only when the user
    // confirms. `then` runs after the save.
    void requestSave(std::function<void()> then = nullptr);
    void revert();
    std::string sourceFolderFor(const std::string& dir) const;
    void locate();          // where the open tables are (when a folder opens)
    void saveUserCopy();    // the tables, as edited, to ConversionTables::userFolder()
    // Edits and undo
    void edit(Doc& d);
    void commitPending();
    void undo(Doc& d);
    void redo(Doc& d);
    void validate();
    // Views
    void menuBar();
    void statusLine();
    void docView(Doc& d);
    void entriesView(Doc& d, bool kits);
    void entryDetails(Doc& d, bool kits, int index);
    bool soundFields(Doc& d, JsonValue& s, bool gs, bool kit, int maxMap, const char* id, bool allowMapChange);
    void noteMapsView(Doc& d);
    void rulesView(Doc& d);
    void aboutView(Doc& d);
    void pickerPopup();
    void namesPopup();
    void dialogs();
    // Helpers
    std::string rowText(const Doc& d, const JsonValue& e, bool kits) const;
    std::string soundLabel(const JsonValue& s, bool gs, bool kit, int ownMap = 0) const;
    std::string expectedName(const JsonValue& s, bool gs, bool kit) const;
    bool nameMatches(const JsonValue& s, bool gs, bool kit) const;
    // `current`: the sound to show first (pc0 < 0: none).
    void openPicker(PickKind kind, int map, int maxMap, const SoundNames::Item& current, std::function<void(const SoundNames::Item&)> done);
    std::vector<std::pair<int, std::string>> kitUsers(const Doc& d, const std::string& noteMap) const;
    // Audition
    void auditionMenu();
    void playVoice(bool gs, int a, int b, int pc0);
    void playDrum(bool gs, int a, int pc0, int note);
    void send(int port, std::initializer_list<uint8_t> m);
    void saveSettings();

    GLFWwindow* window_;
    Config cfg_;
    std::string savedCfg_;
    SoundNames names_;
    std::string insdefFolder_;
    std::string folder_;
    // Where folder_ is: the tables ImMidi reads (ConversionTables::defaultFolder()), the copy a build puts
    // next to the programs (sourceFolder_: the repository's), their source, or the ones inside an app bundle.
    std::string readFolder_, sourceFolder_;
    bool isRead_ = false, isSourceOfRead_ = false, inApp_ = false;
    std::array<Doc, 3> docs_;
    int current_ = 1;  // the document shown (xg-to-gs first)
    bool selectTab_ = true;  // show `current_` (set from code)
    // Pending edit (one undo step per edited item)
    Doc* pendingDoc_ = nullptr;
    unsigned pendingId_ = 0;
    std::string pendingText_;
    uint64_t pendingVersion_ = 0;
    uint64_t lastVersion_ = 1;  // the newest version given to an edited state
    // Checks
    bool validateSoon_ = true;
    std::chrono::steady_clock::time_point validateAt_{};
    bool valid_ = false;
    std::string validError_;
    std::vector<NameIssue> nameIssues_;
    bool showNames_ = false;
    // Picker
    bool pickerOpen_ = false;
    PickKind pickKind_ = PickKind::GsVoice;
    int pickMap_ = 1, pickMaxMap_ = 4;
    std::vector<SoundNames::Item> pickItems_;
    std::function<void(const SoundNames::Item&)> pickDone_;
    char pickFilter_[96] = {};
    int pickSel_ = -1;
    int pickScroll_ = 0;  // bring the selection into view: 1 centred (on opening), 2 at the nearest edge (keys)
    // Dialogs and messages
    NativeDialogs dialog_;
    bool folderDialog_ = false;
    bool askFolderPath_ = false;
    char folderInput_[1024] = {};
    std::string pendingFolder_;
    bool confirmDiscard_ = false, closeAsked_ = false, quit_ = false, showAbout_ = false;
    bool confirmInvalidSave_ = false;
    std::function<void()> afterSave_;
    std::string message_;
    std::chrono::steady_clock::time_point messageUntil_{};
    // Audition
    MidiOutputs outputs_;
    std::string outName_[2];
    std::vector<std::string> devices_;
    std::vector<NoteOff> noteOffs_;
};

} // namespace immidi

#pragma once
#include "Config.h"
#include "FileAnalysis.h"
#include "FileBrowser.h"
#include "InsDef.h"
#include "MidiFile.h"
#include "MidiIn.h"
#include "MidiOut.h"
#include "NativeDialogs.h"
#include "Player.h"
#include "Playlist.h"
#include "SongModules.h"
#include "imgui.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow;

namespace immidi {

enum class ViewMode : uint8_t { Normal = 0, Compact, PortTabs, SideBySide, Count };
const char* viewModeName(ViewMode m);

enum class Tab : uint8_t { Channels = 0, SoundEdit, Effects, Lyrics, Playlist, Info, Settings, Count };

class OffscreenTarget;

// Sound Canvas display colors: the display, and the lit dots of the level matrix.
struct LcdColors {
    ImU32 backlight = IM_COL32(255, 111, 15, 255);  // the SC-55's orange
    ImU32 ink = IM_COL32(0, 0, 0, 255);             // lit dots, segments and characters
    ImU32 unlit = IM_COL32(200, 80, 0, 255);        // unlit dots
};
enum class LcdMatrixMode : uint8_t { Display, One, TopRow, TopDot, Gradient, Count };
struct LcdMatrixColors {
    LcdMatrixMode mode = LcdMatrixMode::Display;  // Display: lit dots as the display's
    ImU32 lit = IM_COL32(40, 200, 70, 255);       // One; the other rows / dots of TopRow and TopDot
    ImU32 top = IM_COL32(255, 45, 30, 255);       // TopRow: the top row; TopDot: each bar's top dot
    ImU32 low = IM_COL32(40, 200, 70, 255);       // Gradient: the bottom row ...
    ImU32 high = IM_COL32(255, 45, 30, 255);      // ... to the top row, through the hues between
};

class App {
public:
    explicit App(GLFWwindow* window);
    ~App();

    void frame();
    // Physical key events (GLFW key codes) for the virtual keyboard, called from the key callback.
    void onKey(int key, int action, int mods);
    // Offscreen passes (the Sound Canvas display texture); call after the frame's draw data was rendered.
    void renderOffscreen();
    // insertAt: playlist index for the new entries (-1 = append).
    void openPaths(const std::vector<std::string>& paths, bool replacePlaylist, bool play, int insertAt = -1);
    // Files dropped onto the window at (x, y) (window coordinates). Onto a playlist view they are
    // inserted before or after the entry under the cursor (or appended) and playback goes on;
    // anywhere else they are appended and the first one plays (unless playDroppedFiles_ is off).
    void dropPaths(const std::vector<std::string>& paths, float x, float y);
    // Files dragged over the window from another program (shows the playlist insertion mark).
    void setExternalDrag(bool inside, float x, float y) {
        extDragInside_ = inside;
        extDragPos_ = ImVec2(x, y);
    }
    bool quitRequested() const { return quit_; }
    std::string windowTitle() const;

    // Automation (used by --screenshot / command line)
    void selectTab(Tab t) { requestedTab_ = t; }
    void setViewMode(ViewMode m) { viewMode_ = m; }
    void setDevice(int deviceIndex);  // -1 = follow file
    Player& player() { return *player_; }
    void setOutputForAllPorts(const std::string& device);
    void showInstrumentPicker(int port, int ch) { openInstrumentPicker(port, ch); }
    // Seek applied as soon as the file that is currently loading is ready (command line --seek).
    void seekAfterLoad(int64_t us) { pendingSeekUs_ = us; }
    bool isLoading() const { return !loadJobs_.empty(); }
    // True while something on screen changes by itself (playing, loading, meters falling, display
    // animations, drags); otherwise the main loop sleeps until an event arrives.
    bool needsContinuousFrames() const;
    // Seconds between redraws without input: 0 = every display refresh (level meters, keyboards,
    // karaoke or a drag on screen), more when only slower things change (the transport on the other
    // tabs and in the mini player), -1 = nothing changes by itself.
    double frameInterval() const;
    void setMiniMode(bool on);

private:
    // ---- setup / persistence
    void loadSettings();
    void saveSettings();          // writes only when something changed
    void storeSettingsToConfig();
    void saveIfChanged();         // event driven: after interactions, playlist edits and loads
    void applyPlayerSettings();
    void refreshDevices();
    void applyTheme();

    // ---- playback control
    // Starts loading a playlist entry on a worker thread; the result is applied in pollLoading().
    void loadIndex(int index, bool play, int skipTries = 0);
    void pollLoading();
    // Playlist entries' details: queues the unread ones for the background reader, applies results.
    void pollSongInfo();
    void applyLoadedFile(int index, std::shared_ptr<MidiFile> mf, const FileInfo& info, bool play);
    void playNext(bool manual);
    void playPrevious();
    void handleShortcuts();
    void updateMeters(float dt);

    // ---- drawing
    void drawMenuBar();
    void drawTransport();
    void drawInfoBar();
    void drawPolyphony();  // sounding notes per port and in all, in the info bar
    void drawChannels();
    void drawChannelTable(int port, float height, bool compact, bool narrow);
    void drawChannelRow(int port, int ch, float rowH, bool compact, bool narrow);
    void drawChannelDetail();
    void drawSoundEdit();
    void drawEffects();
    void drawMasterEffects(int port);
    void drawGsEffects(int port);
    void drawXgEffects(int port);
    void drawGm2Effects(int port);
    void drawLyrics();
    void drawPlaylist(bool pane);
    void drawLcdWindow();
    void drawLcdColorSettings();  // Settings > Display
    void lcdColorMenus();         // the display's context menu
    void loadLcdColors();
    void storeLcdColors();
    void drawVirtualKeyboard();
    void vkNote(int note, bool on, int velocity);
    void vkReleaseAll();
    void vkSetInput(const std::string& name);
    void vkProgramStep(int delta);
    void drawInfo();
    void drawSettings();
    void drawInstrumentPicker();
    void drawAbout();
    void drawFileInfo();
    int portSelector(const char* id, int port);

    // ---- instrument naming
    const DeviceProfile& profile() const;
    const InsInstrument* melodicIns() const;
    const InsInstrument* drumIns() const;
    // The two definitions above are looked up by name; they are resolved once per frame.
    void resolveInsDefs() const;
    mutable int insDefsFrame_ = -1;
    mutable const InsInstrument* melodicInsCache_ = nullptr;
    mutable const InsInstrument* drumInsCache_ = nullptr;
    // Instrument name of each channel, kept until its definition, bank, program or drum mode changes.
    struct NameCacheEntry {
        const InsInstrument* ins = nullptr;
        int bank = -1, program = -1;
        bool drum = false, fallback = false, valid = false;
        std::string name;
    };
    mutable NameCacheEntry nameCache_[kMaxPorts][16];
    int insBank(const ChannelState& c, bool drum) const;
    std::string instrumentName(int port, int ch, bool* fallback = nullptr) const;
    std::string instrumentNameUncached(const ChannelState& c, const InsInstrument* ins, int bank, bool* fallback) const;
    std::string drumNoteName(int port, int ch, int note) const;
    void openInstrumentPicker(int port, int ch);

    GLFWwindow* window_;
    bool quit_ = false;

    // core
    std::unique_ptr<MidiOutputs> outputs_;
    std::unique_ptr<Player> player_;
    Playlist playlist_;
    InsLibrary ins_;
    Config cfg_;
    std::unique_ptr<PlayerSnapshot> snap_;
    std::shared_ptr<const MidiFile> file_;
    std::shared_ptr<MidiFile> fileMut_;
    FileInfo info_;          // the loaded song's analysis, with the module the user set for it
    FileInfo detectedInfo_;  // ... and as found
    SongModules songModules_;  // the module songs are made for, as the user set it
    std::string savedSongModulesText_;
    std::string loadError_;
    std::vector<std::string> deviceList_;

    // settings
    std::string portDevice_[kMaxPorts];
    bool mirrorEffects_ = false;  // SC-88 shared settings to every port (Player::setMirrorEffects)
    int deviceSetting_ = -1;  // -1 = follow file, else DeviceKind
    int fallbackDevice_ = int(DeviceKind::SC88Pro);
    bool emulation_ = false;
    int forcedToneMap_ = 0;  // Sound Canvas map forcer (0 = off)
    bool useConversionTables_ = true;
    std::string conversionDir_;  // instrument conversion tables (auto-detected)
    int resetMode_ = int(ResetMode::Auto);
    int resetDelayMs_ = 200;
    bool resetOnSeek_ = false;
    bool loopEnabled_ = true;
    int loopRepeats_ = 1;
    int encodingOverride_ = 0;
    int kbLo_ = 12, kbHi_ = 119;
    ViewMode viewMode_ = ViewMode::Normal;
    bool autoCompactMultiport_ = true;
    bool showPlaylistPane_ = true;
    // Mini player: menu, transport and song line only, in a window fitted to them. The channel
    // view and the other windows are not drawn meanwhile.
    bool miniMode_ = false;
    int fullWindowH_ = 0;  // window height to return to
    int miniFitH_ = 0;     // height the window was fitted to (0 = not yet)
    void fitMiniWindow(float contentHeight);
    // Sound Canvas style LCD window
    bool showLcd_ = false;
    // Virtual keyboard window: plays on one channel with the mouse, the computer keyboard (while
    // focused) and a MIDI input (while open).
    bool showVk_ = false;
    bool vkFocused_ = false;
    int vkPort_ = 0, vkCh_ = 0;
    int vkVelocity_ = 100;
    int vkOctave_ = 3;               // the lower key row starts at C of this octave (C3 = 48)
    bool vkMouseVelocityByPos_ = false;
    bool vkSustain_ = false;
    int vkBend_ = 8192, vkMod_ = 0;
    int vkMouseNote_ = -1;
    int vkKeyNote_[512];             // GLFW key -> sounding note (-1)
    std::string vkInputName_;        // saved MIDI input choice
    std::vector<std::string> vkInputList_;
    std::unique_ptr<MidiInput> vkInput_;
    bool vkRemap_ = true;            // MIDI input plays on the keyboard's channel
    std::atomic<int> vkInPort_{0}, vkInChannel_{0};  // routing read by the MIDI input thread (-1 = keep)
    std::atomic<uint32_t> vkInActivity_{0};
    std::atomic<uint32_t> vkInChannels_{0};         // channels the input played on (released on close)
    std::string vkInputError_;
    bool vkInputTried_ = false;
    std::string soundBank_;       // macOS: .sf2 / .dls for the Apple DLS Synth (empty = built-in)
    std::string rendererSetting_; // "d3d11" / "metal" / "vulkan" / "opengl" (empty = default); used at the next start
    std::string soundBankError_;
    int lcdSize_ = 2;  // 0 small (50%), 1 medium (75%), 2 large (native 724 x 300)
    ImDrawList* lcdList_ = nullptr;                  // display contents at texture resolution
    std::unique_ptr<OffscreenTarget> lcdTarget_;
    int lcdTexW_ = 0, lcdTexH_ = 0;
    bool lcdPending_ = false;
    uint32_t lcdDotsSeen_ = 0, lcdTextSeen_ = 0;
    double lcdDotsUntil_ = 0, lcdTextStart_ = -1;
    int lcdPortSeen_ = -1;
    ImVec2 lcdWinPos_, lcdWinSize_, lcdShownSize_;  // the display window last frame, and the display size in it
    LcdColors lcdColors_;
    LcdMatrixColors lcdMatrix_;
    bool showChannelDetail_ = true;
    float uiScale_ = 1.0f;
    bool darkTheme_ = true;
    float meterDecay_ = 1.6f;
    int speedPercent_ = 100;
    int transpose_ = 0;
    int masterVolume_ = 127;
    std::string insOverrideMelodic_[int(DeviceKind::Count)];
    std::string insOverrideDrum_[int(DeviceKind::Count)];
    std::string lastDir_;
    // Playlist table geometry from the last frame it was drawn, to place dropped files.
    struct PlaylistRowRect {
        int index;
        float top, bottom;
    };
    struct PlaylistGeometry {
        std::vector<PlaylistRowRect> rows;
        ImVec2 min{0, 0}, max{0, 0};
        float bodyTop = 0;   // bottom of the header row
        float bodyMaxX = 0;  // right edge of the rows (left of the scroll bar)
        ImVec2 areaMin{0, 0}, areaMax{0, 0};  // the whole view: buttons, list and status line
        int frame = -10;
    };
    PlaylistGeometry plGeom_[2];  // [0] Playlist tab, [1] side pane
    // Insertion index for a point over a playlist table (entries.size() = at the end), -1 = not over it.
    int playlistInsertIndex(const PlaylistGeometry& g, ImVec2 p) const;
    void drawInsertionMark(const PlaylistGeometry& g, int index) const;
    void handlePlaylistKeys(const PlaylistGeometry& g);
    enum PlaylistColumn { PlColNumber, PlColTitle, PlColFileName, PlColTime, PlColPath, PlColumnCount };
    void drawPlaylistHeader(int view);
    // Sorts the playlist by a column of its table (clicked header); again on the same column: the other way round.
    void sortPlaylist(int column);
    void openFileInfo(int index);
    // "Song made for": the module the given songs are made for (Device menu, playlist, File Info).
    void songModuleMenu(const std::vector<std::string>& paths);
    void setSongModule(const std::vector<std::string>& paths, int device);  // -1: as detected
    FileInfo songInfoWithModule(const std::string& path, const FileInfo& detected) const;
    void selectOnly(int index);
    void selectRange(int a, int b, bool keepOthers);
    bool extDragInside_ = false;
    ImVec2 extDragPos_{0, 0};
    // Playlist selection: the focused entry (keyboard cursor), the anchor of Shift selections, and
    // which view (0 tab, 1 pane) has the keyboard.
    int plCursor_ = -1, plAnchor_ = -1;
    int plFocusView_ = -1;
    int plScrollTo_ = -1;
    int plCollapseTo_ = -1;  // plain click on a selected entry: select only it on release (unless dragged)
    // Last sort: its column, direction, and the order it produced (the header shows an arrow while
    // the entries are still in that order).
    int plSortColumn_ = -1;
    int plHeaderPressed_[2] = {-1, -1};  // per view: header column the mouse button went down on
    bool plSortAscending_ = true;
    uint64_t plSortedOrder_ = ~0ull;
    // File info window: the entry (by path) and what the file system said when it was opened.
    struct FileInfoWindow {
        std::string path;
        bool request = false;  // open the popup this frame
        bool exists = false;
        uint64_t size = 0;
        std::string modified;
    } fileInfoWin_;
    std::unique_ptr<SongInfoReader> infoReader_;
    uint64_t infoQueuedRev_ = ~0ull;  // playlist revision the reader's queue was made for
    bool infoArrived_ = false;        // details applied since the playlist was last marked changed
    bool rememberPlaylist_ = true;
    bool playDroppedFiles_ = true;  // files dropped outside the playlist views start playing
    bool dropLateNotes_ = true;
    int maxLagMs_ = 50;
    int minVelocity_ = 0;

    // persistence
    std::string savedSettingsText_, savedPlaylistText_;
    uint64_t savedPlaylistRev_ = ~0ull;
    bool saveRequested_ = false;
    int64_t pendingSeekUs_ = -1;

    // background loading
    struct LoadJob {
        int index = -1;
        bool play = false;
        int skipTries = 0;
        std::string path;
        std::thread thread;
        std::atomic<float> progress{0.0f};
        std::atomic<bool> done{false};
        std::shared_ptr<MidiFile> file;
        FileInfo info;
        std::string error;
    };
    std::vector<std::unique_ptr<LoadJob>> loadJobs_;  // the last one is the current request

    // UI state
    Tab tab_ = Tab::Channels;
    Tab requestedTab_ = Tab::Count;
    int selPort_ = 0, selCh_ = 0;
    int portTab_ = 0;
    int heldNote_[kMaxPorts][16];
    float meter_[kMaxPorts][16] = {};
    uint32_t lastSerial_[kMaxPorts][16] = {};
    bool seeking_ = false;
    float seekSeconds_ = 0.0f;
    int effectsPort_ = 0;
    int soundEditPort_ = 0;
    bool showAbout_ = false;
    bool pickerOpen_ = false;
    bool pickerRequest_ = false;
    int pickerPort_ = 0, pickerCh_ = 0, pickerBank_ = -1;
    char pickerFilter_[64] = {};
    int playlistDragFrom_ = -1;
    bool lyricsShowAll_ = false;
    float lyricsFontScale_ = 2.0f;

    // File dialogs: native ones when available, the built-in ImGui browser otherwise.
    enum class DialogPurpose { None, OpenFiles, AddFiles, AddFolder, ImportPlaylist, AppendPlaylist, ExportPlaylist, ExportSmf, ExportSmfEmulated, SoundBank };
    // macOS: loads soundBank_ into the Apple DLS Synth and re-sends the song's state to it.
    void applySoundBank();
    // Writes the loaded song as a Standard MIDI File (loop marked the RPG Maker way).
    bool songSmfForExport(std::vector<uint8_t>& smf, std::string& error) const;
    bool exportSmf(const std::string& path, std::string& error);
    bool exportSmfEmulated(const std::string& path, std::string& error);
    void showFileDialog(DialogPurpose purpose);
    void handleDialogResult(DialogPurpose purpose, const std::vector<std::string>& paths, bool recursive);
    FileBrowser browser_;
    NativeDialogs nativeDialogs_;
    DialogPurpose dialogPurpose_ = DialogPurpose::None;
    bool useNativeDialogs_ = true;
    bool addSubfolders_ = true;
    bool exportRelative_ = true;
};

} // namespace immidi

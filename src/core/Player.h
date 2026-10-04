#pragma once
#include "Emulation.h"
#include "FileAnalysis.h"
#include "MidiFile.h"
#include "MidiOut.h"
#include "SynthState.h"

#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace immidi {

enum class ResetMode : uint8_t { None = 0, Auto, GmOn, Gm2On, GsReset, Sc88ModeSet, XgOn, Count };
const char* resetModeName(ResetMode m);

struct ChannelLocks {
    std::bitset<128> cc;
    bool program = false;
    bool bend = false;
    uint16_t sound = 0;
    bool any() const { return cc.any() || program || bend || sound; }
};

struct LogEntry {
    int64_t songUs;
    int port;
    std::string text;
};

struct PlayerSnapshot {
    int numPorts = 1;
    PortState ports[kMaxPorts];
    ChannelLocks locks[kMaxPorts][16];
    uint16_t mute[kMaxPorts] = {};
    uint16_t solo[kMaxPorts] = {};
    int64_t positionUs = 0;
    bool playing = false;
    bool starting = false;
    double speed = 1.0;
    int transpose = 0;
    DeviceKind device = DeviceKind::SC88Pro;
    MidiStandard source = MidiStandard::GM;
    bool emulating = false;
    int masterVolume = 127;
    bool loopEnabled = true;
    int loopRepeats = 1;
    int loopsDone = 0;
    uint64_t skippedNotes = 0;  // note-ons dropped because playback lagged (black MIDI)
    int lagMs = 0;              // how far behind the song time the output currently is
    int forcedToneMap = 0;      // effective forced GS tone map (0 = none)
};

class Player {
public:
    explicit Player(MidiOutputs& outputs);
    ~Player();

    // ---- file
    void setFile(std::shared_ptr<MidiFile> file);
    void setFile(std::shared_ptr<MidiFile> file, const FileInfo& info);  // analysis already done (e.g. on a loader thread)
    // A new analysis of the loaded song (e.g. the user chose the module it is made for): the emulation
    // follows it, and playback goes on from the same position.
    void setFileInfo(const FileInfo& info);
    std::shared_ptr<const MidiFile> file() const;
    FileInfo fileInfo() const;

    // ---- transport
    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void seek(int64_t us);
    bool isPlaying() const;
    int64_t positionUs() const;
    bool consumeFinished();  // true once after the song reached its end

    // ---- global controls
    void setSpeed(double factor);  // 1.0 = 100 %
    double speed() const;
    void setTranspose(int semitones);
    int transpose() const;
    void setMasterVolume(int v);  // 0..127, sent as universal master volume
    void setMute(int port, int ch, bool on);
    void setSolo(int port, int ch, bool on);
    void clearMuteSolo();
    void clearMutes();  // unmute every part
    void clearSolos();  // end every solo
    // Loop points found in the file: repeats = number of jumps back (-1 = forever).
    void setLoop(bool enabled, int repeats);
    bool loopEnabled() const;
    int loopRepeats() const;
    // Black MIDI handling: drop note-ons that are later than maxLagMs (0 = never) or softer than minVelocity.
    void setNoteSkipping(int maxLagMs, int minVelocity);
    // For SC-88 style multi-port songs played on several single-port GS synths (e.g. one Sound
    // Canvas VA per port): settings that an SC-88 in single module mode shares between all its
    // parts (master, reverb, chorus, delay, EQ, EFX, resets) go to every port, and messages for the
    // other part group (50 xx xx) go to the other port of the pair. See emitRoutedLocked().
    void setMirrorEffects(bool on);
    bool mirrorEffects() const;

    // ---- device / standards
    void setDevice(DeviceKind kind, bool followFile);
    DeviceKind device() const;       // effective target device
    bool deviceFollowsFile() const;
    void setEmulation(bool enabled);
    // Sound Canvas tone map forcer: 0 = off, 1 = SC-55, 2 = SC-88, 3 = SC-88Pro, 4 = SC-8850 map.
    void setForcedToneMap(int map);
    // Instrument conversion tables used by the emulation (folder with gs-to-xg.json, ...). Loads
    // them when the folder changes; conversionTablesError() tells why they could not be loaded.
    void setConversionTables(bool enabled, const std::string& folder);
    std::string conversionTableInUse() const;  // what applies to this song and device, empty when none
    std::shared_ptr<const ConversionTables> conversionTables() const;  // null when off or not loaded
    std::string conversionTablesError() const;
    int forcedToneMap() const;
    bool emulation() const;
    void setResetMode(ResetMode m);
    ResetMode resetMode() const;
    void setResetDelayMs(int ms);
    void setResetOnSeek(bool on);
    void sendResetNow();
    void panic();

    // ---- user edits from the GUI (messages are native to the target device)
    void userSend(int port, const Bytes& msg);
    void userSendAll(int port, const std::vector<Bytes>& msgs);
    void setChannelCC(int port, int ch, int cc, int value, bool lock = true);
    void setProgram(int port, int ch, int bankMsb, int bankLsb, int program, bool lock = true);
    void setPitchBend(int port, int ch, int value14, bool lock = true);
    void setSoundParam(int port, int ch, SoundParam p, int value, bool lock = true);
    void setDrumPart(int port, int ch, int map);  // 0 = off, 1/2 = drum map
    void setKeyShift(int port, int ch, int semitones);
    void setBendRange(int port, int ch, int semitones);
    void setEfxAssign(int port, int ch, bool on);
    void unlock(int port, int ch);  // releases every lock of the channel and restores file values
    void unlockCC(int port, int ch, int cc);
    void previewNote(int port, int ch, int note, int velocity);  // velocity 0 = off

    // Builds the message(s) for a sound parameter on the current target (empty when unsupported).
    std::vector<Bytes> soundParamMessages(int ch, int port, SoundParam p, int value) const;
    bool soundParamSupported(SoundParam p) const;

    void snapshot(PlayerSnapshot& out) const;
    std::vector<LogEntry> sysexLog() const;
    void clearSysexLog();

private:
    using Clock = std::chrono::steady_clock;

    struct FileChan {
        uint8_t cc[128];
        uint8_t prog = 0, msb = 0, lsb = 0;
        uint16_t bend = 8192;
        uint8_t sound[kSoundParamCount];
        uint8_t nrpnMsb = 127, nrpnLsb = 127;
        bool nrpnSel = false;
        void clear();
    };
    struct UserValues {
        uint8_t cc[128] = {};
        uint8_t prog = 0, msb = 0, lsb = 0;
        uint16_t bend = 8192;
        uint8_t sound[kSoundParamCount] = {};
    };

    std::unique_lock<std::mutex> uiLock() const;
    void yieldToUi(std::unique_lock<std::mutex>& lk);
    void threadMain();
    int64_t songUsLocked(Clock::time_point now) const;
    void configureLocked();
    bool startLocked(std::unique_lock<std::mutex>& lk, int64_t us);
    bool waitLocked(std::unique_lock<std::mutex>& lk, int ms);
    bool chaseLocked(std::unique_lock<std::mutex>& lk, int64_t us);
    void dispatchLocked(const MidiEvent& e);
    void channelFromFileLocked(int port, uint8_t st, uint8_t d1, uint8_t d2);
    void emitFromFileLocked(int port, const uint8_t* msg, size_t len);
    void emitDirectLocked(int port, const uint8_t* msg, size_t len);
    void emitDirectLocked(int port, const Bytes& b) { emitDirectLocked(port, b.data(), b.size()); }
    // Song and user messages that are not notes: emitDirectLocked() after the effect mirroring.
    void emitRoutedLocked(int port, const uint8_t* msg, size_t len);
    void emitRoutedLocked(int port, const Bytes& b) { emitRoutedLocked(port, b.data(), b.size()); }
    void silenceLocked(bool releaseSustain);
    void silenceChannelLocked(int port, int ch);
    void releaseNotesLocked();
    // Note-offs for every instance of a song note that is still sounding.
    void releaseSentNoteLocked(int port, int ch, int note);
    bool loopActiveLocked() const;
    void sendResetLocked();
    void resendLocksLocked();
    bool audibleLocked(int port, int ch) const;
    void applyMuteChangesLocked(uint16_t before[kMaxPorts]);
    void logLocked(int port, const uint8_t* msg, size_t len);
    int usedPortCount() const;
    const DeviceProfile& profileLocked() const;

    MidiOutputs& out_;
    mutable std::mutex m_;
    mutable std::atomic<int> uiWaiting_{0};
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_ = false;

    std::shared_ptr<MidiFile> file_;
    FileInfo info_;
    size_t nextEvent_ = 0;
    bool playing_ = false;
    bool startPending_ = false;
    bool starting_ = false;
    int64_t startUs_ = 0;
    uint64_t commandSerial_ = 0;
    int64_t anchorSongUs_ = 0;
    Clock::time_point anchorReal_;
    double speed_ = 1.0;
    int transpose_ = 0;
    int masterVolume_ = 127;
    std::atomic<bool> finished_{false};
    bool loopEnabled_ = true;
    int loopRepeats_ = 1;
    int loopsDone_ = 0;
    int maxLagMs_ = 50;
    int minVelocity_ = 0;
    bool mirrorEffects_ = false;
    bool doubleModule_[kMaxPorts / 2] = {};  // per port pair: SC-88 double module mode (System Mode Set 01)
    uint64_t skippedNotes_ = 0;
    int64_t lagUs_ = 0;

    DeviceKind deviceSetting_ = DeviceKind::SC88Pro;
    bool followFile_ = true;
    bool emulation_ = false;
    int forcedMap_ = 0;
    bool conversionEnabled_ = true;
    std::string conversionFolder_;
    std::string conversionError_;
    std::shared_ptr<const ConversionTables> conversion_;
    ResetMode resetMode_ = ResetMode::Auto;
    int resetDelayMs_ = 200;
    bool resetOnSeek_ = false;
    DeviceKind activeDevice_ = DeviceKind::SC88Pro;

    std::unique_ptr<SynthState> state_;  // state of the target device(s)
    Emulator emu_;
    FileChan fileCh_[kMaxPorts][16];
    // Notes sent from the song, by the song's note number: the pitch sent (after transposition; -1 =
    // none sounding) and how many of its note-ons still wait for a note-off. Tracks that share a
    // channel can overlap the same note: every note-off is passed on, as synths that release one
    // voice per note-off (e.g. Sound Canvas VA) would otherwise keep one sounding.
    int16_t sentNote_[kMaxPorts][16][128];
    uint8_t sentCount_[kMaxPorts][16][128];
    uint16_t mute_[kMaxPorts] = {};
    uint16_t solo_[kMaxPorts] = {};
    ChannelLocks locks_[kMaxPorts][16];
    UserValues user_[kMaxPorts][16];
    std::deque<LogEntry> log_;
    std::vector<Bytes> scratch_;
};

} // namespace immidi

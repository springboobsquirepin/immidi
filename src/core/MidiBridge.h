#pragma once
#include "ConversionTables.h"
#include "Emulation.h"
#include "MidiIn.h"
#include "MidiOut.h"
#include "Standards.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace immidi {

// ImMidi's emulation as a realtime MIDI filter (ImMidi Bridge): messages from a MIDI input are
// converted from the module they are written for (the source) to the module that plays them (the
// target), as the player converts songs, with the same conversion tables, and go to a MIDI output.
class MidiBridge {
public:
    static constexpr const char* kClient = "ImMidi Bridge";  // the name other programs see the ports under
    static constexpr const char* kNone = "(none)";
    static constexpr const char* kVirtualIn = "[Virtual port] ImMidi Bridge In";  // Linux, macOS
    static constexpr const char* kVirtualOut = "[Virtual port] ImMidi Bridge Out";

    struct Settings {
        DeviceKind source = DeviceKind::GM;       // the module the incoming messages are written for
        DeviceKind target = DeviceKind::SC88Pro;  // the module at the output
        bool followResets = true;                 // a GM, GM2, GS or XG reset in the stream switches the source
        bool useTables = true;                    // the instrument conversion tables
        int forcedToneMap = 0;                    // GS modules with tone maps: 0 = off, 1 SC-55 ... 4 SC-8850 map
    };

    MidiBridge();
    ~MidiBridge();

    // The conversion tables (a folder with gs-to-xg.json, ...); false with the reason when they cannot
    // be loaded. The bridge then converts without them.
    bool loadTables(const std::string& folder, std::string& error);
    bool tablesLoaded() const;

    // Applies the settings; the conversion starts over (its state of the parts is reset). The source
    // in use becomes the settings' source, or with keepSource stays the one a reset switched to.
    void configure(const Settings& s, bool keepSource = false);
    Settings settings() const;
    // The source in use: a reset in the stream switches it when the settings say so.
    DeviceKind source() const;
    // What converts: "xg-to-gs.json (SC-88Pro map)", "emulation (no table for these modules)", or
    // empty when the messages pass unchanged (the same standard).
    std::string conversion() const;

    // Ports by name (kNone = none). The input's messages are converted on RtMidi's thread.
    static std::vector<std::string> inputDevices();
    std::vector<std::string> outputDevices();
    bool setInput(const std::string& name, std::string* error);
    bool setOutput(const std::string& name, std::string* error);
    const std::string& input() const { return inName_; }
    const std::string& output() const { return outName_; }

    // One complete message as the input delivers it: converted and sent to the output (or to the
    // sink, which tests use instead of a port).
    void process(const uint8_t* msg, size_t len);
    using Sink = std::function<void(const uint8_t* msg, size_t len)>;
    void setSink(Sink sink);
    // Called on the input's thread when a message arrives after a pause (0.1 s or more), for the
    // window's activity lights.
    void setWake(std::function<void()> wake);

    void sendReset();    // the target's reset (and the forced tone map); the conversion starts over
    void allNotesOff();  // All Sound Off and All Notes Off on every channel

    uint64_t messagesIn() const { return in_; }
    uint64_t messagesOut() const { return out_; }
    // Seconds since the last message in / out (a large value when there was none).
    double secondsSinceIn() const;
    double secondsSinceOut() const;

private:
    void configureLocked();
    void emitLocked(const uint8_t* msg, size_t len);
    void emitLocked(const Bytes& b) { emitLocked(b.data(), b.size()); }

    mutable std::mutex mutex_;  // the conversion: the input thread and the window
    Settings settings_;
    DeviceKind source_ = DeviceKind::GM;
    DeviceKind gsModel_ = DeviceKind::SC55;  // the GS module a GS reset switches to (the last GS source chosen)
    Emulator emu_;
    ConvSetup setup_;
    std::shared_ptr<const ConversionTables> tables_;
    std::vector<Bytes> scratch_;
    Sink sink_;
    std::function<void()> wake_;
    MidiInput input_;    // only touched by the window's thread
    MidiOutputs outputs_;
    std::string inName_ = kNone, outName_ = kNone;
    std::atomic<uint64_t> in_{0}, out_{0};
    std::atomic<int64_t> lastInMs_{-1}, lastOutMs_{-1};
};

} // namespace immidi

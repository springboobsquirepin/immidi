#pragma once
#include "MidiFile.h"
#include "SoftSynth.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class RtMidiOut;

namespace immidi {

// Routes the file's MIDI ports (A, B, ...) to output devices. Several file ports may share
// one device. All methods are thread-safe.
class MidiOutputs {
public:
    static constexpr const char* kNone = "(none)";
    static constexpr const char* kVirtual = "[Virtual port] ImMidi";
    static constexpr const char* kAppleDls = "Apple DLS Synth (built-in)";  // macOS only

    // `client`: the name other programs see the outputs under; `virtualPort`: the virtual output port
    // (Linux, macOS), which availableDevices() lists as `virtualLabel`.
    explicit MidiOutputs(std::string client = "ImMidi", std::string virtualPort = "ImMidi Out", std::string virtualLabel = kVirtual);
    ~MidiOutputs();

    std::vector<std::string> availableDevices();  // includes kNone and (where supported) kAppleDls and the virtual port
    const std::string& virtualLabel() const { return virtualLabel_; }

    // Assigns a device (by name) to a file port. Returns false (and keeps the port silent) on error.
    bool setPortDevice(int port, const std::string& deviceName, std::string* error = nullptr);
    std::string portDevice(int port) const;
    // Number of distinct open devices.
    int openDeviceCount() const;
    // macOS: the SoundFont 2 / DLS file the Apple DLS Synth plays with (empty = its built-in sounds).
    // An open synth is rebuilt with the new bank. Returns false when the bank could not be loaded;
    // the synth then uses its built-in sounds and softSynthBankError() says why.
    bool setSoftSynthBank(const std::string& path);
    std::string softSynthBankError() const;
    // True when a port plays through the Apple DLS Synth.
    bool usesSoftSynth() const;
    // Loads the instruments a song selects (bank selects and program changes per port) into the
    // built-in synth ahead of playback. Only AUMIDISynth (a custom sound bank) needs it; otherwise
    // this does nothing.
    void preloadSoftSynth(const MidiFile& song);
    // True when `port` shares its device with a lower-numbered port.
    bool portSharesDevice(int port) const;

    void send(int port, const uint8_t* msg, size_t len);
    void send(int port, const std::vector<uint8_t>& msg) { send(port, msg.data(), msg.size()); }
    // Sends to every distinct open device once, using the lowest port that maps to it.
    template <typename F>
    void forEachDistinctPort(F f) const {
        std::vector<int> ports;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (int p = 0; p < kMaxPorts; p++) {
                if (!route_[p]) continue;
                bool dup = false;
                for (int q = 0; q < p; q++)
                    if (route_[q] == route_[p]) dup = true;
                if (!dup) ports.push_back(p);
            }
        }
        for (int p : ports) f(p);
    }
    uint64_t bytesSent() const { return bytesSent_; }
    uint64_t messagesDropped() const { return dropped_; }  // gave up after retrying (device gone / stalled)

private:
    struct Device {
        std::string name;
        std::unique_ptr<RtMidiOut> out;
        std::unique_ptr<SoftSynth> synth;  // instead of `out` for a built-in synthesizer
        std::mutex sendMutex;
        bool failed = false;  // set by the RtMidi error callback during sendMessage()
    };
    std::shared_ptr<Device> openDevice(const std::string& name, std::string* error);
    void closeUnused();

    const std::string client_, virtualPort_, virtualLabel_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Device>> devices_;
    std::shared_ptr<Device> route_[kMaxPorts];
    std::string routeName_[kMaxPorts];
    std::string synthBank_, synthBankError_;
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace immidi

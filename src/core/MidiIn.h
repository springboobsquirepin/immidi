#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RtMidiIn;

namespace immidi {

struct MidiInputOptions {
    std::string client = "ImMidi In";  // the name other programs see the input under
    bool timing = false;               // also MIDI clock, start / continue / stop and active sensing
};

// One MIDI input device (a keyboard, a controller, a loopback port). Messages arrive on RtMidi's
// thread and are handed to the callback there.
class MidiInput {
public:
    using Callback = std::function<void(const uint8_t* msg, size_t len)>;
    MidiInput();
    ~MidiInput();
    // Input devices by name (same naming as the outputs). The program's own ports (names starting with
    // `ownPrefix`) are left out: reading its virtual output back would feed its output into itself.
    static std::vector<std::string> availableDevices(const char* ownPrefix = "ImMidi");
    bool open(const std::string& name, Callback callback, std::string* error = nullptr,
              const MidiInputOptions& options = MidiInputOptions());
    // A virtual input port other programs send to (Linux, macOS).
    bool openVirtual(const std::string& portName, Callback callback, std::string* error = nullptr,
                     const MidiInputOptions& options = MidiInputOptions());
    static bool virtualPortsSupported();
    void close();
    bool isOpen() const { return in_ != nullptr; }
    const std::string& name() const { return name_; }

private:
    static void rtCallback(double deltaTime, std::vector<unsigned char>* message, void* user);
    std::unique_ptr<RtMidiIn> in_;
    std::string name_;
    Callback callback_;
};

} // namespace immidi

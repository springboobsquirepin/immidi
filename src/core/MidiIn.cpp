#include "MidiIn.h"
#include "MidiPortNames.h"

#include "RtMidi.h"

namespace immidi {

namespace {
bool isOwnPort(const std::string& raw, const char* prefix) { return raw.rfind(prefix, 0) == 0; }

// SysEx of any size: large messages (sample dumps, bulk data) arrive whole on Windows too.
void prepare(RtMidiIn& in, const MidiInputOptions& o) {
    in.setErrorCallback([](RtMidiError::Type, const std::string&, void*) {}, nullptr);  // no console spam
    in.setBufferSize(65536, 4);
    in.ignoreTypes(false, !o.timing, !o.timing);  // keep SysEx; MIDI clock and active sensing only when asked
}
} // namespace

MidiInput::MidiInput() = default;
MidiInput::~MidiInput() { close(); }

std::vector<std::string> MidiInput::availableDevices(const char* ownPrefix) {
    std::vector<std::string> names;
    try {
        RtMidiIn probe(RtMidi::UNSPECIFIED, "ImMidi In");
        for (const PortName& p : listMidiPorts(probe))
            if (!isOwnPort(p.raw, ownPrefix)) names.push_back(p.display);
    } catch (RtMidiError&) {
    }
    return names;
}

bool MidiInput::virtualPortsSupported() {
#if defined(_WIN32)
    return false;  // Windows MIDI has no virtual ports (loopMIDI and similar drivers provide them)
#else
    return true;
#endif
}

bool MidiInput::open(const std::string& name, Callback callback, std::string* error, const MidiInputOptions& options) {
    close();
    try {
        auto in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, options.client);
        std::vector<PortName> ports = listMidiPorts(*in);
        int found = -1;
        for (size_t i = 0; i < ports.size() && found < 0; i++)
            if (ports[i].display == name && !isOwnPort(ports[i].raw, options.client.c_str())) found = int(i);
        std::string base = stripPortNumber(name);
        for (size_t i = 0; i < ports.size() && found < 0; i++)
            if (stripPortNumber(ports[i].raw) == base && !isOwnPort(ports[i].raw, options.client.c_str())) found = int(i);
        if (found < 0) {
            if (error) *error = "MIDI input \"" + name + "\" not found";
            return false;
        }
        callback_ = std::move(callback);
        prepare(*in, options);
        in->setCallback(&MidiInput::rtCallback, this);
        in->openPort(unsigned(found), options.client);
        in_ = std::move(in);
        name_ = ports[size_t(found)].display;
        return true;
    } catch (RtMidiError& e) {
        if (error) *error = e.getMessage();
        return false;
    }
}

bool MidiInput::openVirtual(const std::string& portName, Callback callback, std::string* error, const MidiInputOptions& options) {
    close();
    if (!virtualPortsSupported()) {
        if (error) *error = "virtual MIDI ports are not available here";
        return false;
    }
    try {
        auto in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, options.client);
        callback_ = std::move(callback);
        prepare(*in, options);
        in->setCallback(&MidiInput::rtCallback, this);
        in->openVirtualPort(portName);
        in_ = std::move(in);
        name_ = portName;
        return true;
    } catch (RtMidiError& e) {
        if (error) *error = e.getMessage();
        return false;
    }
}

void MidiInput::close() {
    if (!in_) return;
    try {
        in_->cancelCallback();
        in_->closePort();
    } catch (RtMidiError&) {
    }
    in_.reset();
    name_.clear();
}

void MidiInput::rtCallback(double, std::vector<unsigned char>* message, void* user) {
    auto* self = static_cast<MidiInput*>(user);
    if (message && !message->empty() && self->callback_) self->callback_(message->data(), message->size());
}

} // namespace immidi

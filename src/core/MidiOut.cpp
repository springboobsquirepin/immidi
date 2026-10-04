#include "MidiOut.h"
#include "MidiPortNames.h"

#include "RtMidi.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace immidi {


static bool isDigits(const std::string& s, size_t a, size_t b) {
    if (a >= b) return false;
    for (size_t i = a; i < b; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// RtMidi appends a number to every port name to keep them unique: the device index on Windows
// ("Microsoft GS Wavetable Synth 0") and the ALSA client:port on Linux ("FLUID Synth:Synth input
// port 128:0"). The numbers change when devices come and go, so they are left out of the names
// ImMidi shows and saves.
std::string stripPortNumber(const std::string& raw) {
    size_t sp = raw.find_last_of(' ');
    if (sp == std::string::npos || sp == 0) return raw;
#if defined(_WIN32)
    if (isDigits(raw, sp + 1, raw.size())) return raw.substr(0, sp);
#elif defined(__linux__)
    size_t colon = raw.find(':', sp + 1);
    if (colon != std::string::npos && isDigits(raw, sp + 1, colon) && isDigits(raw, colon + 1, raw.size())) return raw.substr(0, sp);
#endif
    return raw;
}

std::vector<PortName> listMidiPorts(RtMidi& out) {
    std::vector<PortName> v;
    unsigned n = out.getPortCount();
    for (unsigned i = 0; i < n; i++) {
        PortName p;
        p.raw = out.getPortName(i);
        p.display = stripPortNumber(p.raw);
        int dup = 1;
        for (const PortName& q : v)
            if (stripPortNumber(q.raw) == p.display) dup++;
        if (dup > 1) p.display += " (" + std::to_string(dup) + ")";
        v.push_back(std::move(p));
    }
    return v;
}


MidiOutputs::MidiOutputs(std::string client, std::string virtualPort, std::string virtualLabel)
    : client_(std::move(client)), virtualPort_(std::move(virtualPort)), virtualLabel_(std::move(virtualLabel)) {}

MidiOutputs::~MidiOutputs() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& r : route_) r = nullptr;
    devices_.clear();
}

std::vector<std::string> MidiOutputs::availableDevices() {
    std::vector<std::string> names;
    names.push_back(kNone);
    try {
        RtMidiOut probe;
        for (const PortName& p : listMidiPorts(probe)) names.push_back(p.display);
    } catch (RtMidiError&) {
    }
#if defined(__APPLE__)
    names.push_back(kAppleDls);  // after the MIDI ports, so a connected module is preferred at first start
#endif
#if !defined(_WIN32)
    names.push_back(virtualLabel_);
#endif
    return names;
}

std::shared_ptr<MidiOutputs::Device> MidiOutputs::openDevice(const std::string& name, std::string* error) {
    for (auto& d : devices_)
        if (d->name == name) return d;
    auto dev = std::make_shared<Device>();
    dev->name = name;
    if (name == kAppleDls) {
        dev->synth = createAppleDlsSynth(synthBank_, error, &synthBankError_);
        if (!dev->synth) return nullptr;
        devices_.push_back(dev);
        return dev;
    }
    try {
        dev->out = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, client_);
        // RtMidi prints send errors to stderr by default. Record them instead so send() can retry:
        // ALSA's sequencer queue, for example, rejects messages while the receiver is busy.
        Device* raw = dev.get();
        dev->out->setErrorCallback([](RtMidiError::Type type, const std::string&, void* user) {
            if (type != RtMidiError::DEBUG_WARNING) static_cast<Device*>(user)->failed = true;
        }, raw);
        if (name == virtualLabel_) {
            dev->out->openVirtualPort(virtualPort_);
        } else {
            std::vector<PortName> ports = listMidiPorts(*dev->out);
            int found = -1;
            // Current names first, then names saved by older versions (with the RtMidi number,
            // which may have changed since).
            for (size_t i = 0; i < ports.size() && found < 0; i++)
                if (ports[i].display == name) found = int(i);
            for (size_t i = 0; i < ports.size() && found < 0; i++)
                if (ports[i].raw == name) found = int(i);
            std::string base = stripPortNumber(name);
            for (size_t i = 0; i < ports.size() && found < 0; i++)
                if (stripPortNumber(ports[i].raw) == base) found = int(i);
            if (found < 0) {
                if (error) *error = "MIDI output \"" + name + "\" not found";
                return nullptr;
            }
            dev->name = ports[size_t(found)].display;
            // An older name may resolve to a device that is already open.
            for (auto& d : devices_)
                if (d->name == dev->name) return d;
            dev->out->openPort(unsigned(found), client_);
        }
    } catch (RtMidiError& e) {
        if (error) *error = e.getMessage();
        return nullptr;
    }
    devices_.push_back(dev);
    return dev;
}

void MidiOutputs::closeUnused() {
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                  [&](const std::shared_ptr<Device>& d) {
                                      return std::none_of(std::begin(route_), std::end(route_), [&](const std::shared_ptr<Device>& r) { return r == d; });
                                  }),
                   devices_.end());
}

bool MidiOutputs::setPortDevice(int port, const std::string& deviceName, std::string* error) {
    if (port < 0 || port >= kMaxPorts) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    routeName_[port] = deviceName;
    route_[port] = nullptr;
    bool ok = true;
    if (!deviceName.empty() && deviceName != kNone) {
        route_[port] = openDevice(deviceName, error);
        ok = route_[port] != nullptr;
    }
    closeUnused();
    return ok;
}

std::string MidiOutputs::portDevice(int port) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (port < 0 || port >= kMaxPorts) return {};
    return route_[port] ? route_[port]->name : std::string(kNone);
}

bool MidiOutputs::setSoftSynthBank(const std::string& path) {
    std::lock_guard<std::mutex> lk(mutex_);
    synthBank_ = path;
    synthBankError_.clear();
    for (auto& d : devices_) {
        if (!d->synth) continue;
        std::string err;
        auto s = createAppleDlsSynth(synthBank_, &err, &synthBankError_);
        if (!s) {
            synthBankError_ = err;
            continue;
        }
        std::lock_guard<std::mutex> sl(d->sendMutex);  // the player thread may be sending
        d->synth = std::move(s);
    }
    return synthBankError_.empty();
}

std::string MidiOutputs::softSynthBankError() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return synthBankError_;
}

bool MidiOutputs::usesSoftSynth() const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& r : route_)
        if (r && r->synth) return true;
    return false;
}

void MidiOutputs::preloadSoftSynth(const MidiFile& song) {
    std::vector<std::shared_ptr<Device>> synths(kMaxPorts);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        bool any = false;
        for (int p = 0; p < kMaxPorts; p++)
            if (route_[p] && route_[p]->synth) {
                synths[size_t(p)] = route_[p];
                any = true;
            }
        if (!any) return;
    }
    std::vector<std::vector<ProgramRef>> programs(kMaxPorts);
    uint8_t msb[kMaxPorts][16] = {}, lsb[kMaxPorts][16] = {};
    for (const MidiEvent& e : song.events) {
        if (!e.isChannel() || e.port >= kMaxPorts || !synths[e.port]) continue;
        uint8_t ch = e.channel();
        if (e.type() == 0xB0 && e.d1 == 0) msb[e.port][ch] = e.d2;
        else if (e.type() == 0xB0 && e.d1 == 32) lsb[e.port][ch] = e.d2;
        else if (e.type() == 0xC0) programs[e.port].push_back(ProgramRef{ch, msb[e.port][ch], lsb[e.port][ch], e.d1});
    }
    for (int p = 0; p < kMaxPorts; p++) {
        const auto& d = synths[size_t(p)];
        if (!d || programs[size_t(p)].empty()) continue;
        std::lock_guard<std::mutex> sl(d->sendMutex);
        d->synth->preload(programs[size_t(p)]);
    }
}

int MidiOutputs::openDeviceCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return int(devices_.size());
}

bool MidiOutputs::portSharesDevice(int port) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (port <= 0 || port >= kMaxPorts || !route_[port]) return false;
    for (int q = 0; q < port; q++)
        if (route_[q] == route_[port]) return true;
    return false;
}

void MidiOutputs::send(int port, const uint8_t* msg, size_t len) {
    if (port < 0 || port >= kMaxPorts || len == 0) return;
    std::shared_ptr<Device> d;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        d = route_[port];
    }
    if (!d) return;
    std::lock_guard<std::mutex> lk(d->sendMutex);
    if (d->synth) {
        d->synth->send(msg, len);
        bytesSent_ += len;
        return;
    }
    if (!d->out) return;
    // A busy receiver makes the send fail; wait and retry instead of losing the message (a lost
    // note-off would hang a note). The player notices the resulting lag and drops late note-ons.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    for (;;) {
        d->failed = false;
        try {
            d->out->sendMessage(msg, len);
        } catch (RtMidiError&) {
            d->failed = true;
        }
        if (!d->failed) {
            bytesSent_ += len;
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            dropped_++;
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

} // namespace immidi

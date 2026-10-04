#include "MidiBridge.h"

#include <chrono>

namespace immidi {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The standard a reset message switches to: GM System On, GM2 System On, GS Reset (and the SC-88's
// System Mode Set) or XG System On, with any device number.
bool resetStandard(const uint8_t* m, size_t n, MidiStandard& out) {
    if (n >= 6 && m[1] == 0x7E && m[3] == 0x09 && (m[4] == 0x01 || m[4] == 0x03)) {
        out = m[4] == 0x01 ? MidiStandard::GM : MidiStandard::GM2;
        return true;
    }
    if (n >= 10 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12 &&
        ((m[5] == 0x40 && m[6] == 0x00 && m[7] == 0x7F) || (m[5] == 0x00 && m[6] == 0x00 && m[7] == 0x7F))) {
        out = MidiStandard::GS;
        return true;
    }
    if (n >= 8 && m[1] == 0x43 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C && m[4] == 0 && m[5] == 0 && m[6] == 0x7E) {
        out = MidiStandard::XG;
        return true;
    }
    return false;
}

} // namespace

MidiBridge::MidiBridge() : outputs_(kClient, "ImMidi Bridge Out", kVirtualOut) {
    std::lock_guard<std::mutex> lk(mutex_);
    source_ = settings_.source;
    configureLocked();
}

MidiBridge::~MidiBridge() {
    input_.close();  // first: its callback converts with the members below
}

bool MidiBridge::loadTables(const std::string& folder, std::string& error) {
    auto t = std::make_shared<ConversionTables>();
    bool ok = !folder.empty() && t->load(folder, error);
    if (folder.empty()) error = "folder not found (conversion)";
    std::lock_guard<std::mutex> lk(mutex_);
    tables_ = ok ? std::shared_ptr<const ConversionTables>(t) : nullptr;
    configureLocked();
    return ok;
}

bool MidiBridge::tablesLoaded() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tables_ != nullptr;
}

void MidiBridge::configure(const Settings& s, bool keepSource) {
    std::lock_guard<std::mutex> lk(mutex_);
    settings_ = s;
    if (!keepSource || !s.followResets) source_ = s.source;
    if (deviceProfile(s.source).family == MidiStandard::GS) gsModel_ = s.source;
    configureLocked();
}

void MidiBridge::configureLocked() {
    const MidiStandard src = deviceProfile(source_).family;
    emu_.configure(src, deviceProfile(settings_.target), settings_.forcedToneMap);
    setup_ = ConvSetup();
    if (settings_.useTables && tables_) setup_ = ConversionTables::setupFor(src, source_, settings_.target, emu_.forcedToneMap());
    emu_.setConversion(settings_.useTables ? tables_ : nullptr, setup_);
}

MidiBridge::Settings MidiBridge::settings() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return settings_;
}

DeviceKind MidiBridge::source() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return source_;
}

std::string MidiBridge::conversion() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!emu_.active()) return {};
    if (setup_.direction != ConvDirection::None) return setup_.describe();
    return "emulation (no table for these modules)";
}

std::vector<std::string> MidiBridge::inputDevices() {
    std::vector<std::string> v = {kNone};
    for (const std::string& d : MidiInput::availableDevices(kClient)) v.push_back(d);
    if (MidiInput::virtualPortsSupported()) v.push_back(kVirtualIn);
    return v;
}

std::vector<std::string> MidiBridge::outputDevices() {
    std::vector<std::string> v;
    // Not the bridge's own virtual input: its output would come back in.
    for (const std::string& d : outputs_.availableDevices())
        if (d.rfind(kClient, 0) != 0) v.push_back(d);
    return v;
}

bool MidiBridge::setInput(const std::string& name, std::string* error) {
    input_.close();  // without the lock: the input's callback takes it
    inName_ = kNone;
    if (name.empty() || name == kNone) return true;
    MidiInputOptions o;
    o.client = kClient;
    o.timing = true;  // clock, start / stop and active sensing pass through as they are
    auto cb = [this](const uint8_t* m, size_t n) { process(m, n); };
    bool ok = name == kVirtualIn ? input_.openVirtual("ImMidi Bridge In", cb, error, o) : input_.open(name, cb, error, o);
    if (ok) inName_ = name == kVirtualIn ? std::string(kVirtualIn) : input_.name();
    return ok;
}

bool MidiBridge::setOutput(const std::string& name, std::string* error) {
    std::string n = name.empty() ? std::string(kNone) : name;
    bool ok = outputs_.setPortDevice(0, n, error);
    outName_ = ok ? (n == kNone ? std::string(kNone) : outputs_.portDevice(0)) : std::string(kNone);
    return ok;
}

void MidiBridge::setSink(Sink sink) {
    std::lock_guard<std::mutex> lk(mutex_);
    sink_ = std::move(sink);
}

void MidiBridge::setWake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lk(mutex_);
    wake_ = std::move(wake);
}

void MidiBridge::emitLocked(const uint8_t* msg, size_t len) {
    out_++;
    lastOutMs_ = nowMs();
    if (sink_) sink_(msg, len);
    else outputs_.send(0, msg, len);
}

// As the player sends a song's messages: notes, pressure and bend are the same in every standard
// (a conversion table only moves drum notes and changes their velocity), bank selects, program
// changes, controllers and SysEx go through the emulation, and system messages pass as they are.
void MidiBridge::process(const uint8_t* msg, size_t len) {
    if (len == 0) return;
    in_++;
    const int64_t now = nowMs();
    const int64_t previous = lastInMs_.exchange(now);
    std::lock_guard<std::mutex> lk(mutex_);
    if (wake_ && (previous < 0 || now - previous >= 100)) wake_();
    const uint8_t st = msg[0];
    if (st == 0xF0) {
        MidiStandard s;
        if (settings_.followResets && resetStandard(msg, len, s)) {
            DeviceKind k = s == MidiStandard::GM    ? DeviceKind::GM
                           : s == MidiStandard::GM2 ? DeviceKind::GM2
                           : s == MidiStandard::XG  ? DeviceKind::XG
                           : deviceProfile(source_).family == MidiStandard::GS ? source_
                                                                               : gsModel_;
            if (k != source_) {
                source_ = k;
                configureLocked();
            }
        }
        scratch_.clear();
        emu_.convert(0, msg, len, scratch_);
        for (const Bytes& b : scratch_) emitLocked(b);
        return;
    }
    if (st < 0x80 || st > 0xF0) {
        emitLocked(msg, len);
        return;
    }
    const uint8_t ty = st & 0xF0;
    if (ty == 0xB0 || ty == 0xC0) {
        scratch_.clear();
        emu_.convert(0, msg, len, scratch_);
        for (const Bytes& b : scratch_) emitLocked(b);
        return;
    }
    if ((ty == 0x80 || ty == 0x90) && len >= 3 && emu_.mapsNotes()) {
        uint8_t m[3] = {msg[0], msg[1], msg[2]};
        if (emu_.mapNote(0, m)) emitLocked(m, 3);
        return;
    }
    emitLocked(msg, len);
}

void MidiBridge::sendReset() {
    std::lock_guard<std::mutex> lk(mutex_);
    emu_.reset();
    switch (deviceProfile(settings_.target).family) {
    case MidiStandard::GS:
        emitLocked(gsReset());
        for (const Bytes& b : emu_.forcedMapMessages()) emitLocked(b);
        break;
    case MidiStandard::XG: emitLocked(xgSystemOn()); break;
    case MidiStandard::GM2: emitLocked(gm2SystemOn()); break;
    default: emitLocked(gmSystemOn()); break;
    }
}

void MidiBridge::allNotesOff() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (int c = 0; c < 16; c++) {
        emitLocked(Bytes{uint8_t(0xB0 | c), 120, 0});
        emitLocked(Bytes{uint8_t(0xB0 | c), 123, 0});
    }
}

double MidiBridge::secondsSinceIn() const {
    int64_t t = lastInMs_;
    return t < 0 ? 1e9 : double(nowMs() - t) / 1000.0;
}

double MidiBridge::secondsSinceOut() const {
    int64_t t = lastOutMs_;
    return t < 0 ? 1e9 : double(nowMs() - t) / 1000.0;
}

} // namespace immidi

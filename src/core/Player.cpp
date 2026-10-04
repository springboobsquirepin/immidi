#include "Player.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace immidi {

const char* resetModeName(ResetMode m) {
    switch (m) {
    case ResetMode::None: return "None";
    case ResetMode::Auto: return "Auto (by target device)";
    case ResetMode::GmOn: return "GM System On";
    case ResetMode::Gm2On: return "GM2 System On";
    case ResetMode::GsReset: return "GS Reset";
    case ResetMode::Sc88ModeSet: return "SC-88 Mode Set + GS Reset";
    case ResetMode::XgOn: return "XG System On";
    default: return "?";
    }
}

void Player::FileChan::clear() {
    std::memset(cc, 0, sizeof cc);
    cc[7] = 100;
    cc[10] = 64;
    cc[11] = 127;
    prog = msb = lsb = 0;
    bend = 8192;
    for (int i = 0; i < kSoundParamCount; i++) sound[i] = soundParamDefault(SoundParam(i));
    nrpnMsb = nrpnLsb = 127;
    nrpnSel = false;
}

Player::Player(MidiOutputs& outputs) : out_(outputs), state_(std::make_unique<SynthState>()) {
    for (auto& p : fileCh_)
        for (auto& c : p) c.clear();
    std::memset(sentNote_, 0xFF, sizeof sentNote_);
    std::memset(sentCount_, 0, sizeof sentCount_);
    state_->resetAll(MidiStandard::None);
    configureLocked();
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
    thread_ = std::thread([this] { threadMain(); });
}

Player::~Player() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
        playing_ = false;
        silenceLocked(true);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

// ---------------------------------------------------------------- configuration

const DeviceProfile& Player::profileLocked() const { return deviceProfile(activeDevice_); }

int Player::usedPortCount() const { return file_ ? std::max(1, file_->numPorts) : 1; }

void Player::configureLocked() {
    MidiStandard source = file_ ? info_.standard : MidiStandard::GM;
    activeDevice_ = (followFile_ && file_) ? info_.suggestedDevice : deviceSetting_;
    const DeviceProfile& target = profileLocked();
    if (emulation_) emu_.configure(source, target, forcedMap_);
    else emu_.configure(target.family, target, forcedMap_, false);  // passthrough (except a forced tone map)
    // Always set: a song or device that needs no tables turns them off (an empty setup).
    ConvSetup setup;
    if (emulation_ && conversionEnabled_ && conversion_ && file_)
        setup = ConversionTables::setupFor(source, info_.suggestedDevice, activeDevice_, emu_.forcedToneMap());
    emu_.setConversion(conversion_, setup);
    state_->assumedStandard = target.family;
}

void Player::setFile(std::shared_ptr<MidiFile> file) {
    FileInfo info = file ? analyzeFile(*file) : FileInfo();
    setFile(std::move(file), info);
}

void Player::setFile(std::shared_ptr<MidiFile> file, const FileInfo& info) {
    auto lk = uiLock();
    silenceLocked(true);
    playing_ = false;
    startPending_ = false;
    commandSerial_++;
    file_ = std::move(file);
    info_ = info;
    configureLocked();
    for (auto& p : fileCh_)
        for (auto& c : p) c.clear();
    std::memset(sentNote_, 0xFF, sizeof sentNote_);
    std::memset(sentCount_, 0, sizeof sentCount_);
    for (auto& p : locks_)
        for (auto& c : p) c = ChannelLocks();
    std::memset(mute_, 0, sizeof mute_);
    std::memset(solo_, 0, sizeof solo_);
    std::memset(doubleModule_, 0, sizeof doubleModule_);
    state_->resetAll(MidiStandard::None);
    emu_.reset();
    nextEvent_ = 0;
    anchorSongUs_ = 0;
    finished_ = false;
    loopsDone_ = 0;
    skippedNotes_ = 0;
    lagUs_ = 0;
    cv_.notify_all();
}

std::shared_ptr<const MidiFile> Player::file() const {
    auto lk = uiLock();
    return file_;
}

FileInfo Player::fileInfo() const {
    auto lk = uiLock();
    return info_;
}

void Player::setFileInfo(const FileInfo& info) {
    auto lk = uiLock();
    if (!file_) return;
    info_ = info;
    configureLocked();
    if (playing_ || startPending_) {  // restart where it is: reset, chase and go on with the new conversion
        int64_t pos = songUsLocked(Clock::now());
        startUs_ = pos;
        startPending_ = true;
        playing_ = true;
        commandSerial_++;
        cv_.notify_all();
    }
}

void Player::setDevice(DeviceKind kind, bool followFile) {
    auto lk = uiLock();
    deviceSetting_ = kind;
    followFile_ = followFile;
    DeviceKind before = activeDevice_;
    configureLocked();
    if (activeDevice_ != before && (playing_ || startPending_)) {
        int64_t pos = songUsLocked(Clock::now());
        startUs_ = pos;
        startPending_ = true;
        playing_ = true;
        commandSerial_++;
        cv_.notify_all();
    }
}

DeviceKind Player::device() const {
    auto lk = uiLock();
    return activeDevice_;
}

bool Player::deviceFollowsFile() const {
    auto lk = uiLock();
    return followFile_;
}

void Player::setEmulation(bool enabled) {
    auto lk = uiLock();
    if (emulation_ == enabled) return;
    emulation_ = enabled;
    configureLocked();
    if (playing_) {
        int64_t pos = songUsLocked(Clock::now());
        startUs_ = pos;
        startPending_ = true;
        commandSerial_++;
        cv_.notify_all();
    }
}

void Player::setForcedToneMap(int map) {
    auto lk = uiLock();
    if (forcedMap_ == map) return;
    forcedMap_ = map;
    configureLocked();
    if (playing_) {
        int64_t pos = songUsLocked(Clock::now());
        startUs_ = pos;
        startPending_ = true;
        commandSerial_++;
        cv_.notify_all();
    }
}

std::shared_ptr<const ConversionTables> Player::conversionTables() const {
    auto lk = uiLock();
    return conversionEnabled_ ? conversion_ : nullptr;
}

void Player::setConversionTables(bool enabled, const std::string& folder) {
    std::shared_ptr<const ConversionTables> tables;
    std::string error;
    bool reload;
    {
        auto lk = uiLock();
        reload = folder != conversionFolder_ || (!conversion_ && conversionError_.empty());
        tables = conversion_;
        error = conversionError_;
    }
    if (reload) {  // outside the lock: parsing the tables takes a moment
        tables = nullptr;
        error.clear();
        if (folder.empty()) {
            error = "folder not found (conversion)";
        } else {
            auto t = std::make_shared<ConversionTables>();
            if (t->load(folder, error)) tables = t;
        }
    }
    auto lk = uiLock();
    conversionEnabled_ = enabled;
    conversionFolder_ = folder;
    conversion_ = std::move(tables);
    conversionError_ = error;
    configureLocked();
}

std::string Player::conversionTableInUse() const {
    auto lk = uiLock();
    return emu_.hasConversion() ? emu_.conversionSetup().describe() : std::string();
}

std::string Player::conversionTablesError() const {
    auto lk = uiLock();
    return conversionError_;
}

int Player::forcedToneMap() const {
    auto lk = uiLock();
    return emu_.forcedToneMap();
}

bool Player::emulation() const {
    auto lk = uiLock();
    return emulation_;
}

void Player::setResetMode(ResetMode m) {
    auto lk = uiLock();
    resetMode_ = m;
}

ResetMode Player::resetMode() const {
    auto lk = uiLock();
    return resetMode_;
}

void Player::setResetDelayMs(int ms) {
    auto lk = uiLock();
    resetDelayMs_ = std::clamp(ms, 0, 5000);
}

void Player::setResetOnSeek(bool on) {
    auto lk = uiLock();
    resetOnSeek_ = on;
}

// ---------------------------------------------------------------- transport

int64_t Player::songUsLocked(Clock::time_point now) const {
    if (starting_ || startPending_) return startUs_;
    if (!playing_) return anchorSongUs_;
    if (now <= anchorReal_) return anchorSongUs_;
    double el = double(std::chrono::duration_cast<std::chrono::microseconds>(now - anchorReal_).count());
    return anchorSongUs_ + int64_t(el * speed_);
}

void Player::play() {
    auto lk = uiLock();
    if (!file_ || playing_) return;
    commandSerial_++;
    finished_ = false;
    if (anchorSongUs_ >= file_->lengthUs) anchorSongUs_ = 0;
    startUs_ = anchorSongUs_;
    startPending_ = true;
    playing_ = true;
    cv_.notify_all();
}

void Player::pause() {
    auto lk = uiLock();
    if (!playing_) return;
    anchorSongUs_ = songUsLocked(Clock::now());
    playing_ = false;
    startPending_ = false;
    commandSerial_++;
    silenceLocked(true);
    cv_.notify_all();
}

void Player::togglePlayPause() {
    bool p;
    {
        auto lk = uiLock();
        p = playing_;
    }
    if (p) pause();
    else play();
}

void Player::stop() {
    auto lk = uiLock();
    playing_ = false;
    startPending_ = false;
    commandSerial_++;
    silenceLocked(true);
    anchorSongUs_ = 0;
    nextEvent_ = 0;
    cv_.notify_all();
}

void Player::seek(int64_t us) {
    auto lk = uiLock();
    if (!file_) return;
    us = std::clamp<int64_t>(us, 0, file_->lengthUs);
    commandSerial_++;
    finished_ = false;
    if (playing_) {
        startUs_ = us;
        startPending_ = true;
        cv_.notify_all();
    } else {
        anchorSongUs_ = us;
    }
}

bool Player::isPlaying() const {
    auto lk = uiLock();
    return playing_;
}

int64_t Player::positionUs() const {
    auto lk = uiLock();
    int64_t us = songUsLocked(Clock::now());
    if (file_) us = std::min(us, file_->lengthUs);
    return us;
}

bool Player::consumeFinished() { return finished_.exchange(false); }

void Player::setSpeed(double factor) {
    auto lk = uiLock();
    factor = std::clamp(factor, 0.05, 8.0);
    auto now = Clock::now();
    if (playing_ && !startPending_ && !starting_) {
        anchorSongUs_ = songUsLocked(now);
        if (now > anchorReal_) anchorReal_ = now;
    }
    speed_ = factor;
    cv_.notify_all();
}

double Player::speed() const {
    auto lk = uiLock();
    return speed_;
}

void Player::setTranspose(int semitones) {
    auto lk = uiLock();
    transpose_ = std::clamp(semitones, -48, 48);
}

int Player::transpose() const {
    auto lk = uiLock();
    return transpose_;
}

void Player::setMasterVolume(int v) {
    auto lk = uiLock();
    masterVolume_ = std::clamp(v, 0, 127);
    Bytes b = masterVolumeSysex(uint16_t(masterVolume_ == 127 ? 16383 : masterVolume_ << 7));
    for (int p = 0; p < usedPortCount(); p++) emitDirectLocked(p, b);
}

bool Player::audibleLocked(int port, int ch) const {
    if (mute_[port] & (1u << ch)) return false;
    bool anySolo = false;
    for (int p = 0; p < kMaxPorts; p++)
        if (solo_[p]) anySolo = true;
    if (anySolo && !(solo_[port] & (1u << ch))) return false;
    return true;
}

void Player::applyMuteChangesLocked(uint16_t before[kMaxPorts]) {
    for (int p = 0; p < kMaxPorts; p++)
        for (int c = 0; c < 16; c++) {
            bool was = before[p] & (1u << c);
            bool now = audibleLocked(p, c);
            if (was && !now) silenceChannelLocked(p, c);
        }
}

void Player::setMute(int port, int ch, bool on) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    uint16_t before[kMaxPorts];
    for (int p = 0; p < kMaxPorts; p++) {
        before[p] = 0;
        for (int c = 0; c < 16; c++)
            if (audibleLocked(p, c)) before[p] |= uint16_t(1u << c);
    }
    if (on) mute_[port] |= uint16_t(1u << ch);
    else mute_[port] &= uint16_t(~(1u << ch));
    applyMuteChangesLocked(before);
}

void Player::setSolo(int port, int ch, bool on) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    uint16_t before[kMaxPorts];
    for (int p = 0; p < kMaxPorts; p++) {
        before[p] = 0;
        for (int c = 0; c < 16; c++)
            if (audibleLocked(p, c)) before[p] |= uint16_t(1u << c);
    }
    if (on) solo_[port] |= uint16_t(1u << ch);
    else solo_[port] &= uint16_t(~(1u << ch));
    applyMuteChangesLocked(before);
}

// Clearing mutes or solos only makes parts audible again, so there is nothing to silence.
void Player::clearMuteSolo() {
    auto lk = uiLock();
    std::memset(mute_, 0, sizeof mute_);
    std::memset(solo_, 0, sizeof solo_);
}

void Player::clearMutes() {
    auto lk = uiLock();
    std::memset(mute_, 0, sizeof mute_);
}

void Player::clearSolos() {
    auto lk = uiLock();
    std::memset(solo_, 0, sizeof solo_);
}

// ---------------------------------------------------------------- thread

std::unique_lock<std::mutex> Player::uiLock() const {
    uiWaiting_.fetch_add(1);
    std::unique_lock<std::mutex> lk(m_);
    uiWaiting_.fetch_sub(1);
    return lk;
}

void Player::yieldToUi(std::unique_lock<std::mutex>& lk) {
    if (!uiWaiting_.load()) return;
    lk.unlock();
    while (uiWaiting_.load()) std::this_thread::yield();
    lk.lock();
}

void Player::setNoteSkipping(int maxLagMs, int minVelocity) {
    auto lk = uiLock();
    maxLagMs_ = std::max(0, maxLagMs);
    minVelocity_ = std::clamp(minVelocity, 0, 127);
}

void Player::setMirrorEffects(bool on) {
    auto lk = uiLock();
    if (mirrorEffects_ == on) return;
    mirrorEffects_ = on;
    if (playing_ && file_) {
        // Restart at the current position: the chase sends the song's settings again, routed the new way.
        startUs_ = songUsLocked(Clock::now());
        startPending_ = true;
        commandSerial_++;
        cv_.notify_all();
    }
}

bool Player::mirrorEffects() const {
    auto lk = uiLock();
    return mirrorEffects_;
}

void Player::threadMain() {
    std::unique_lock<std::mutex> lk(m_);
    while (!quit_) {
        if (startPending_) {
            startPending_ = false;
            starting_ = true;
            bool ok = startLocked(lk, startUs_);
            starting_ = false;
            if (!ok) continue;  // interrupted by another command
            continue;
        }
        if (!playing_ || !file_) {
            cv_.wait(lk);
            continue;
        }
        auto now = Clock::now();
        if (now < anchorReal_) {
            cv_.wait_until(lk, anchorReal_);
            continue;
        }
        int64_t songUs = songUsLocked(now);
        const std::vector<MidiEvent>& ev = file_->events;
        bool looping = loopActiveLocked();
        int64_t loopEndUs = looping ? file_->loopEndUs : INT64_MAX;
        int64_t maxLagUs = int64_t(maxLagMs_) * 1000;
        // Dispatch in short batches so the GUI can always get the lock, even when a dense
        // (black MIDI) passage keeps the output busy.
        auto budgetEnd = now + std::chrono::milliseconds(4);
        bool budgetHit = false;
        int n = 0;
        while (nextEvent_ < ev.size()) {
            const MidiEvent& e = ev[nextEvent_];
            if (e.timeUs > songUs || e.timeUs >= loopEndUs) break;
            nextEvent_++;
            int64_t lag = songUs - e.timeUs;
            if (e.type() == 0x90 && ((maxLagUs > 0 && lag > maxLagUs) || e.d2 < minVelocity_)) {
                skippedNotes_++;  // its note-off is dropped too (nothing was sent for it)
            } else {
                dispatchLocked(e);
                lagUs_ = lag;
            }
            if ((++n & 63) == 0 && (uiWaiting_.load(std::memory_order_relaxed) || Clock::now() >= budgetEnd)) {
                budgetHit = true;
                break;
            }
        }
        if (nextEvent_ >= ev.size() || ev[nextEvent_].timeUs > songUs) lagUs_ = 0;
        yieldToUi(lk);
        if (budgetHit || quit_ || !playing_ || startPending_) continue;
        if (looping && songUs >= file_->loopEndUs) {
            // Jump back to the loop start, keeping the timeline continuous.
            releaseNotesLocked();
            anchorReal_ += std::chrono::microseconds(int64_t(double(file_->loopEndUs - anchorSongUs_) / speed_));
            anchorSongUs_ = file_->loopStartUs;
            nextEvent_ = file_->eventIndexAtUs(file_->loopStartUs);
            loopsDone_++;
            continue;
        }
        if (nextEvent_ >= ev.size() && songUs >= file_->lengthUs) {
            silenceLocked(true);
            playing_ = false;
            anchorSongUs_ = file_->lengthUs;
            finished_ = true;
            continue;
        }
        int64_t nextUs = nextEvent_ < ev.size() ? ev[nextEvent_].timeUs : file_->lengthUs;
        if (looping) nextUs = std::min(nextUs, file_->loopEndUs);
        auto wake = anchorReal_ + std::chrono::microseconds(int64_t(double(nextUs - anchorSongUs_) / speed_));
        auto cap = now + std::chrono::milliseconds(50);
        cv_.wait_until(lk, std::min(wake, cap));
    }
}

bool Player::waitLocked(std::unique_lock<std::mutex>& lk, int ms) {
    if (ms <= 0) return !quit_;
    uint64_t serial = commandSerial_;
    cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return quit_ || commandSerial_ != serial; });
    return !quit_ && commandSerial_ == serial;
}

bool Player::startLocked(std::unique_lock<std::mutex>& lk, int64_t us) {
    if (!file_) return false;
    std::shared_ptr<MidiFile> keepAlive = file_;
    uint64_t serial = commandSerial_;
    silenceLocked(true);
    bool atStart = us <= 0;
    if (resetMode_ != ResetMode::None && (atStart || resetOnSeek_)) {
        sendResetLocked();
        if (!waitLocked(lk, resetDelayMs_)) return false;
    }
    emu_.reset();
    for (auto& p : fileCh_)
        for (auto& c : p) c.clear();
    if (atStart || !file_->hasLoop() || us < file_->loopStartUs) loopsDone_ = 0;
    if (!atStart) {
        if (!chaseLocked(lk, us)) return false;
    }
    resendLocksLocked();
    if (commandSerial_ != serial || !playing_) return false;
    nextEvent_ = file_->eventIndexAtUs(us);
    anchorSongUs_ = us;
    anchorReal_ = Clock::now();
    return true;
}

void Player::sendResetLocked() {
    const DeviceProfile& prof = profileLocked();
    ResetMode mode = resetMode_;
    if (mode == ResetMode::Auto) {
        switch (prof.family) {
        case MidiStandard::GS:
            mode = (usedPortCount() > 1 && (prof.kind == DeviceKind::SC88 || prof.kind == DeviceKind::SC88Pro))
                       ? ResetMode::Sc88ModeSet
                       : ResetMode::GsReset;
            break;
        case MidiStandard::XG: mode = ResetMode::XgOn; break;
        case MidiStandard::GM2: mode = ResetMode::Gm2On; break;
        default: mode = ResetMode::GmOn; break;
        }
    }
    int ports = usedPortCount();
    for (int p = 0; p < ports; p++) {
        for (int c = 0; c < 16; c++) {
            emitDirectLocked(p, Bytes{uint8_t(0xB0 | c), 120, 0});
            emitDirectLocked(p, Bytes{uint8_t(0xB0 | c), 121, 0});
        }
        switch (mode) {
        case ResetMode::GmOn: emitDirectLocked(p, gmSystemOn()); break;
        case ResetMode::Gm2On: emitDirectLocked(p, gm2SystemOn()); break;
        case ResetMode::GsReset: emitDirectLocked(p, gsReset()); break;
        case ResetMode::Sc88ModeSet:
            if (p % 2 == 0) emitDirectLocked(p, sc88ModeSet(false));
            emitDirectLocked(p, gsReset());
            break;
        case ResetMode::XgOn: emitDirectLocked(p, xgSystemOn()); break;
        default: break;
        }
        if (masterVolume_ != 127) emitDirectLocked(p, masterVolumeSysex(uint16_t(masterVolume_ << 7)));
        if (mode == ResetMode::GsReset || mode == ResetMode::Sc88ModeSet)
            for (const Bytes& b : emu_.forcedMapMessages()) emitDirectLocked(p, b);
    }
}

void Player::sendResetNow() {
    auto lk = uiLock();
    silenceLocked(true);
    sendResetLocked();
    resendLocksLocked();
}

void Player::panic() {
    auto lk = uiLock();
    silenceLocked(true);
    for (int p = 0; p < usedPortCount(); p++)
        for (int c = 0; c < 16; c++) {
            for (int n = 0; n < 128; n++) out_.send(p, Bytes{uint8_t(0x80 | c), uint8_t(n), 0});
            emitDirectLocked(p, Bytes{uint8_t(0xB0 | c), 121, 0});
        }
}

namespace {

bool isResetSysex(const uint8_t* m, size_t n) {
    if (n >= 6 && m[1] == 0x7E && m[3] == 0x09 && (m[4] == 0x01 || m[4] == 0x03)) return true;
    if (n >= 10 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12 &&
        ((m[5] == 0x40 && m[6] == 0x00 && m[7] == 0x7F) || (m[5] == 0x00 && m[6] == 0x00 && m[7] == 0x7F)))
        return true;
    if (n >= 8 && m[1] == 0x43 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C && m[4] == 0 && m[5] == 0 && (m[6] == 0x7E || m[6] == 0x7F))
        return true;
    return false;
}

std::string sysexKey(int port, const uint8_t* m, size_t n) {
    char buf[32];
    if (n == 11 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12) {
        snprintf(buf, sizeof buf, "G%d:%02X%02X%02X", port, m[5], m[6], m[7]);
        return buf;
    }
    if ((n == 9 || n == 10) && m[1] == 0x43 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C) {
        snprintf(buf, sizeof buf, "X%d:%02X%02X%02X", port, m[4], m[5], m[6]);
        return buf;
    }
    if (n == 8 && m[1] == 0x7F && m[3] == 0x04 && m[4] == 0x01) {
        snprintf(buf, sizeof buf, "MV%d", port);
        return buf;
    }
    return {};
}

struct ChaseChan {
    int16_t cc[128];
    int prog = -1;
    uint8_t pcMsb = 0, pcLsb = 0, curMsb = 0, curLsb = 0;
    int bend = -1, pressure = -1;
    bool nrpnSel = false;
    uint8_t rMsb = 127, rLsb = 127, nMsb = 127, nLsb = 127;
    std::map<uint16_t, std::pair<int16_t, int16_t>> rpn, nrpn;
    ChaseChan() { std::fill(std::begin(cc), std::end(cc), int16_t(-1)); }
};

} // namespace

bool Player::chaseLocked(std::unique_lock<std::mutex>& lk, int64_t us) {
    // The scan below can cover millions of events; the file is immutable, so it runs unlocked.
    std::shared_ptr<MidiFile> f = file_;
    const std::vector<MidiEvent>& ev = f->events;
    size_t end = f->eventIndexAtUs(us);
    int ports = usedPortCount();
    std::vector<ChaseChan> chans(size_t(ports) * 16);
    std::vector<size_t> sysex;
    std::vector<bool> erased;
    std::unordered_map<std::string, size_t> lastByKey;
    uint64_t scanSerial = commandSerial_;
    lk.unlock();

    for (size_t i = 0; i < end; i++) {
        const MidiEvent& e = ev[i];
        if (e.status == 0xF0) {
            const uint8_t* m = f->payload(e);
            if (isResetSysex(m, f->payloadLen(e))) {
                for (size_t k = 0; k < sysex.size(); k++)
                    if (ev[sysex[k]].port == e.port) erased[k] = true;
                for (int c = 0; c < 16 && e.port < ports; c++) chans[size_t(e.port) * 16 + c] = ChaseChan();
            }
            std::string key = sysexKey(e.port, m, f->payloadLen(e));
            if (!key.empty()) {
                auto it = lastByKey.find(key);
                if (it != lastByKey.end()) erased[it->second] = true;
                lastByKey[key] = sysex.size();
            }
            sysex.push_back(i);
            erased.push_back(false);
            continue;
        }
        if (!e.isChannel() || e.port >= ports) continue;
        ChaseChan& c = chans[size_t(e.port) * 16 + e.channel()];
        switch (e.type()) {
        case 0xB0:
            switch (e.d1) {
            case 0: c.curMsb = e.d2; break;
            case 32: c.curLsb = e.d2; break;
            case 99: c.nMsb = e.d2; c.nrpnSel = true; break;
            case 98: c.nLsb = e.d2; c.nrpnSel = true; break;
            case 101: c.rMsb = e.d2; c.nrpnSel = false; break;
            case 100: c.rLsb = e.d2; c.nrpnSel = false; break;
            case 6:
            case 38: {
                auto& target = c.nrpnSel ? c.nrpn : c.rpn;
                uint8_t hi = c.nrpnSel ? c.nMsb : c.rMsb, lo = c.nrpnSel ? c.nLsb : c.rLsb;
                if (hi == 127 && lo == 127) break;
                auto& slot = target.try_emplace(uint16_t((hi << 7) | lo), int16_t(-1), int16_t(-1)).first->second;
                if (e.d1 == 6) slot.first = e.d2;
                else slot.second = e.d2;
                break;
            }
            case 121:
                c.cc[1] = 0;
                c.cc[11] = 127;
                c.cc[64] = c.cc[65] = c.cc[66] = c.cc[67] = 0;
                c.bend = 8192;
                c.pressure = 0;
                break;
            case 120: case 123: case 124: case 125: case 126: case 127:
                break;
            default:
                c.cc[e.d1] = e.d2;
                break;
            }
            break;
        case 0xC0:
            c.prog = e.d1;
            c.pcMsb = c.curMsb;
            c.pcLsb = c.curLsb;
            break;
        case 0xD0: c.pressure = e.d1; break;
        case 0xE0: c.bend = e.d1 | (e.d2 << 7); break;
        default: break;
        }
    }

    lk.lock();
    if (commandSerial_ != scanSerial || quit_) return false;
    uint64_t serial = commandSerial_;
    for (size_t k = 0; k < sysex.size(); k++) {
        if (erased[k]) continue;
        const MidiEvent& e = ev[sysex[k]];
        const uint8_t* m = f->payload(e);
        emitFromFileLocked(e.port, m, f->payloadLen(e));
        if (isResetSysex(m, f->payloadLen(e))) {
            if (!waitLocked(lk, 60)) return false;
        }
    }
    if (commandSerial_ != serial) return false;

    for (int p = 0; p < ports; p++) {
        for (int ch = 0; ch < 16; ch++) {
            ChaseChan& c = chans[size_t(p) * 16 + ch];
            uint8_t b = uint8_t(0xB0 | ch);
            if (c.prog >= 0) {
                channelFromFileLocked(p, b, 0, c.pcMsb);
                channelFromFileLocked(p, b, 32, c.pcLsb);
                channelFromFileLocked(p, uint8_t(0xC0 | ch), uint8_t(c.prog), 0);
            }
            if (c.curMsb != c.pcMsb) channelFromFileLocked(p, b, 0, c.curMsb);
            if (c.curLsb != c.pcLsb) channelFromFileLocked(p, b, 32, c.curLsb);
            for (int n = 0; n < 128; n++) {
                if (c.cc[n] < 0) continue;
                if (n == 0 || n == 32 || n == 6 || n == 38 || (n >= 96 && n <= 101) || n >= 120) continue;
                channelFromFileLocked(p, b, uint8_t(n), uint8_t(c.cc[n]));
            }
            for (auto& [key, val] : c.rpn) {
                channelFromFileLocked(p, b, 101, uint8_t(key >> 7));
                channelFromFileLocked(p, b, 100, uint8_t(key & 0x7F));
                if (val.first >= 0) channelFromFileLocked(p, b, 6, uint8_t(val.first));
                if (val.second >= 0) channelFromFileLocked(p, b, 38, uint8_t(val.second));
            }
            for (auto& [key, val] : c.nrpn) {
                channelFromFileLocked(p, b, 99, uint8_t(key >> 7));
                channelFromFileLocked(p, b, 98, uint8_t(key & 0x7F));
                if (val.first >= 0) channelFromFileLocked(p, b, 6, uint8_t(val.first));
                if (val.second >= 0) channelFromFileLocked(p, b, 38, uint8_t(val.second));
            }
            if (!c.rpn.empty() || !c.nrpn.empty()) {
                channelFromFileLocked(p, b, 101, 127);
                channelFromFileLocked(p, b, 100, 127);
            }
            if (c.bend >= 0) channelFromFileLocked(p, uint8_t(0xE0 | ch), uint8_t(c.bend & 0x7F), uint8_t(c.bend >> 7));
            if (c.pressure >= 0) channelFromFileLocked(p, uint8_t(0xD0 | ch), uint8_t(c.pressure), 0);
        }
    }
    return true;
}

// ---------------------------------------------------------------- dispatch

void Player::dispatchLocked(const MidiEvent& e) {
    if (e.status == 0xFF) return;
    if (e.port >= kMaxPorts) return;
    if (e.status == 0xF7) {
        out_.send(e.port, file_->payload(e), file_->payloadLen(e));
        return;
    }
    if (e.status == 0xF0) {
        emitFromFileLocked(e.port, file_->payload(e), file_->payloadLen(e));
        return;
    }
    channelFromFileLocked(e.port, e.status, e.d1, e.d2);
}

void Player::channelFromFileLocked(int port, uint8_t st, uint8_t d1, uint8_t d2) {
    int ch = st & 0x0F;
    FileChan& f = fileCh_[port][ch];
    ChannelLocks& L = locks_[port][ch];
    uint8_t msg[3] = {st, d1, d2};
    switch (st & 0xF0) {
    case 0x90: {
        if (!audibleLocked(port, ch)) return;
        bool drum = state_->ports[port].ch[ch].drum;
        int n = d1 + (drum ? 0 : transpose_);
        if (n < 0 || n > 127) return;
        // Struck again after a transposition change: the old pitch ends first.
        if (sentNote_[port][ch][d1] >= 0 && sentNote_[port][ch][d1] != n) releaseSentNoteLocked(port, ch, d1);
        sentNote_[port][ch][d1] = int16_t(n);
        if (sentCount_[port][ch][d1] < 255) sentCount_[port][ch][d1]++;
        msg[1] = uint8_t(n);
        emitFromFileLocked(port, msg, 3);
        return;
    }
    case 0x80: {
        // One note-off per note-on sent; others (muted, skipped or already silenced notes) are dropped.
        int s = sentNote_[port][ch][d1];
        if (s < 0) return;
        uint8_t& count = sentCount_[port][ch][d1];
        if (count > 1) {
            count--;
        } else {
            count = 0;
            sentNote_[port][ch][d1] = -1;
        }
        msg[1] = uint8_t(s);
        emitFromFileLocked(port, msg, 3);
        return;
    }
    case 0xA0: {
        int s = sentNote_[port][ch][d1];
        if (s < 0) return;
        msg[1] = uint8_t(s);
        emitFromFileLocked(port, msg, 3);
        return;
    }
    case 0xB0: {
        f.cc[d1] = d2;
        switch (d1) {
        case 99: f.nrpnMsb = d2; f.nrpnSel = true; break;
        case 98: f.nrpnLsb = d2; f.nrpnSel = true; break;
        case 101: case 100: f.nrpnSel = false; break;
        default: break;
        }
        if ((d1 == 0 || d1 == 32) && L.program) return;
        if (d1 == 6 && f.nrpnSel && f.nrpnMsb == 1) {
            for (int i = 0; i < kSoundParamCount; i++)
                if (soundParamNrpnLsb(SoundParam(i)) == f.nrpnLsb) {
                    f.sound[i] = d2;
                    if (L.sound & (1u << i)) return;
                }
        }
        if (d1 >= 71 && d1 <= 78) {
            for (int i = 0; i < kSoundParamCount; i++)
                if (soundParamGm2Cc(SoundParam(i)) == d1) {
                    f.sound[i] = d2;
                    if (L.sound & (1u << i)) return;
                }
        }
        if (L.cc[d1]) return;
        emitFromFileLocked(port, msg, 3);
        return;
    }
    case 0xC0:
        f.prog = d1;
        f.msb = f.cc[0];
        f.lsb = f.cc[32];
        if (L.program) return;
        emitFromFileLocked(port, msg, 2);
        return;
    case 0xD0:
        emitFromFileLocked(port, msg, 2);
        return;
    case 0xE0:
        f.bend = uint16_t(d1 | (d2 << 7));
        if (L.bend) return;
        emitFromFileLocked(port, msg, 3);
        return;
    default:
        return;
    }
}

void Player::emitFromFileLocked(int port, const uint8_t* msg, size_t len) {
    uint8_t t = msg[0] & 0xF0;
    if (t == 0x80 || t == 0x90 || t == 0xA0 || t == 0xD0 || t == 0xE0) {
        // Notes, pressure and bend are identical in every standard: skip the emulator, except for
        // the drum note / velocity mapping of a conversion table.
        if ((t == 0x80 || t == 0x90) && len >= 3 && emu_.mapsNotes()) {
            uint8_t m[3] = {msg[0], msg[1], msg[2]};
            if (emu_.mapNote(port, m)) emitDirectLocked(port, m, 3);
            return;
        }
        emitDirectLocked(port, msg, len);
        return;
    }
    std::vector<Bytes> outMsgs;
    outMsgs.swap(scratch_);
    outMsgs.clear();
    emu_.convert(port, msg, len, outMsgs);
    for (const Bytes& b : outMsgs) emitRoutedLocked(port, b.data(), b.size());
    outMsgs.swap(scratch_);
}

namespace {

bool isGsDataSet(const uint8_t* m, size_t n) { return n >= 10 && m[0] == 0xF0 && m[1] == 0x41 && m[3] == 0x42 && m[4] == 0x12; }

// A GS data set with another first address byte (and the checksum to match).
Bytes withAddressMsb(const uint8_t* m, size_t n, uint8_t a1) {
    Bytes b(m, m + n);
    b[5] = a1;
    size_t sumAt = b.back() == 0xF7 ? b.size() - 2 : b.size() - 1;
    unsigned sum = 0;
    for (size_t i = 5; i < sumAt; i++) sum += b[i];
    b[sumAt] = uint8_t((128 - sum % 128) % 128);
    return b;
}

} // namespace

void Player::emitDirectLocked(int port, const uint8_t* msg, size_t len) {
    if (port < 0 || port >= kMaxPorts || len == 0) return;
    out_.send(port, msg, len);
    state_->apply(port, msg, len);
    if (msg[0] == 0xF0) {
        logLocked(port, msg, len);
        if (isGsDataSet(msg, len) && msg[5] == 0x00 && msg[6] == 0x00 && msg[7] == 0x7F)  // SC-88 System Mode Set
            doubleModule_[port >> 1] = msg[8] == 0x01;
    }
}

// An SC-88/SC-88Pro in single module mode is one sound module with 32 parts on two MIDI inputs:
// the master settings and effects are shared by all its parts, so songs set them once, usually on
// port A, and they address the parts of the other input's group with 50 xx xx instead of 40 xx xx
// (manual: "the data will be passed to the Parts of the other Group than the MIDI IN that the data
// was received at"). A single-port synth per port (e.g. one Sound Canvas VA each) would apply those
// settings only on the port they arrive at, or apply the other group's settings to its own parts.
// With mirroring on, the shared settings go to every port of the song and the other group's
// messages go to the pair's other port, rewritten to 40 xx xx. In double module mode the two groups
// have separate master settings and effects, so nothing is mirrored for that pair.
void Player::emitRoutedLocked(int port, const uint8_t* msg, size_t len) {
    if (!mirrorEffects_ || len < 6 || msg[0] != 0xF0) {
        emitDirectLocked(port, msg, len);
        return;
    }
    const bool gs = isGsDataSet(msg, len);
    const uint8_t a1 = gs ? msg[5] : 0, a2 = gs ? msg[6] : 0, a3 = gs ? msg[7] : 0;
    const bool single = !doubleModule_[port >> 1];
    if (gs && (a1 == 0x50 || a1 == 0x51) && !(single && a1 == 0x50 && a2 <= 0x03)) {
        // Parts / drum setup of the other group: the other port of the pair, as its own 40/41 xx xx.
        if ((port ^ 1) < kMaxPorts) emitDirectLocked(port ^ 1, withAddressMsb(msg, len, uint8_t(a1 - 0x10)));
        return;
    }
    bool shared = false;
    if (gs) {
        // System Mode Set, patch common (master tune/volume/key shift/pan, GS reset, reverb, chorus,
        // delay, EQ, EFX); in single module mode the other group's patch common is the same block.
        shared = (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F) || ((a1 == 0x40 || a1 == 0x50) && a2 <= 0x03);
    } else {
        // GM / GM2 System On and Off, and universal device control (master volume, balance, tuning).
        shared = (msg[1] == 0x7E && msg[3] == 0x09) || (msg[1] == 0x7F && msg[3] == 0x04);
    }
    if (!shared || !single) {
        emitDirectLocked(port, msg, len);
        return;
    }
    Bytes common;
    if (gs && a1 == 0x50) {  // the other group's patch common: the shared block in single module mode
        common = withAddressMsb(msg, len, 0x40);
        msg = common.data();
    }
    emitDirectLocked(port, msg, len);
    int ports = usedPortCount();
    for (int p = 0; p < ports; p++)
        if (p != port) emitDirectLocked(p, msg, len);
}

void Player::logLocked(int port, const uint8_t* msg, size_t len) {
    std::string s;
    s.reserve(len * 3);
    char hex[4];
    for (size_t i = 0; i < len && i < 96; i++) {
        snprintf(hex, sizeof hex, "%02X ", msg[i]);
        s += hex;
    }
    if (len > 96) s += "...";
    log_.push_back({songUsLocked(Clock::now()), port, std::move(s)});
    while (log_.size() > 500) log_.pop_front();
}

std::vector<LogEntry> Player::sysexLog() const {
    auto lk = uiLock();
    return std::vector<LogEntry>(log_.begin(), log_.end());
}

void Player::clearSysexLog() {
    auto lk = uiLock();
    log_.clear();
}

bool Player::loopActiveLocked() const {
    return loopEnabled_ && file_ && file_->hasLoop() && (loopRepeats_ < 0 || loopsDone_ < loopRepeats_);
}

void Player::setLoop(bool enabled, int repeats) {
    auto lk = uiLock();
    loopEnabled_ = enabled;
    loopRepeats_ = repeats;
    cv_.notify_all();
}

bool Player::loopEnabled() const {
    auto lk = uiLock();
    return loopEnabled_;
}

int Player::loopRepeats() const {
    auto lk = uiLock();
    return loopRepeats_;
}

void Player::releaseSentNoteLocked(int port, int ch, int note) {
    int s = sentNote_[port][ch][note];
    if (s < 0) return;
    // Through the same drum note mapping as the note-ons, so the right keys are released.
    uint8_t off[3] = {uint8_t(0x80 | ch), uint8_t(s), 0};
    for (int k = std::max(1, int(sentCount_[port][ch][note])); k > 0; k--) emitFromFileLocked(port, off, 3);
    sentNote_[port][ch][note] = -1;
    sentCount_[port][ch][note] = 0;
}

void Player::releaseNotesLocked() {
    int ports = usedPortCount();
    for (int p = 0; p < ports; p++)
        for (int c = 0; c < 16; c++)
            for (int n = 0; n < 128; n++) releaseSentNoteLocked(p, c, n);
}

void Player::silenceChannelLocked(int port, int ch) {
    for (int n = 0; n < 128; n++) releaseSentNoteLocked(port, ch, n);
    emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), 123, 0});
}

void Player::silenceLocked(bool releaseSustain) {
    int ports = usedPortCount();
    for (int p = 0; p < ports; p++) {
        for (int c = 0; c < 16; c++) {
            for (int n = 0; n < 128; n++) releaseSentNoteLocked(p, c, n);
            if (releaseSustain && state_->ports[p].ch[c].cc[64] >= 64) {
                out_.send(p, Bytes{uint8_t(0xB0 | c), 64, 0});
                state_->ports[p].ch[c].cc[64] = 0;
            }
            emitDirectLocked(p, Bytes{uint8_t(0xB0 | c), 123, 0});
            emitDirectLocked(p, Bytes{uint8_t(0xB0 | c), 120, 0});
        }
    }
}

// ---------------------------------------------------------------- user edits

void Player::userSend(int port, const Bytes& msg) {
    auto lk = uiLock();
    emitRoutedLocked(port, msg);
}

void Player::userSendAll(int port, const std::vector<Bytes>& msgs) {
    auto lk = uiLock();
    for (const Bytes& b : msgs) emitRoutedLocked(port, b);
}

void Player::setChannelCC(int port, int ch, int cc, int value, bool lock) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15 || cc < 0 || cc > 127) return;
    auto lk = uiLock();
    value = std::clamp(value, 0, 127);
    if (lock) {
        locks_[port][ch].cc.set(size_t(cc));
        user_[port][ch].cc[cc] = uint8_t(value);
    }
    emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), uint8_t(cc), uint8_t(value)});
}

void Player::setProgram(int port, int ch, int msb, int lsb, int program, bool lock) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    UserValues& u = user_[port][ch];
    u.msb = uint8_t(msb & 0x7F);
    u.lsb = uint8_t(lsb & 0x7F);
    u.prog = uint8_t(program & 0x7F);
    if (lock) locks_[port][ch].program = true;
    emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), 0, u.msb});
    emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), 32, u.lsb});
    emitDirectLocked(port, Bytes{uint8_t(0xC0 | ch), u.prog});
}

void Player::setPitchBend(int port, int ch, int value14, bool lock) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    value14 = std::clamp(value14, 0, 16383);
    if (lock) {
        locks_[port][ch].bend = true;
        user_[port][ch].bend = uint16_t(value14);
    }
    emitDirectLocked(port, Bytes{uint8_t(0xE0 | ch), uint8_t(value14 & 0x7F), uint8_t(value14 >> 7)});
}

bool Player::soundParamSupported(SoundParam p) const {
    auto lk = uiLock();
    const DeviceProfile& prof = profileLocked();
    switch (prof.family) {
    case MidiStandard::XG: return true;
    case MidiStandard::GS: return soundParamNrpnLsb(p) >= 0 && p != SoundParam::HpfCutoff && p != SoundParam::EqBassGain &&
                                  p != SoundParam::EqTrebleGain && p != SoundParam::EqBassFreq && p != SoundParam::EqTrebleFreq;
    case MidiStandard::GM2: return soundParamGm2Cc(p) >= 0;
    default: return false;
    }
}

std::vector<Bytes> Player::soundParamMessages(int ch, int port, SoundParam p, int value) const {
    (void)port;
    std::vector<Bytes> v;
    uint8_t b = uint8_t(0xB0 | ch);
    uint8_t val = uint8_t(std::clamp(value, 0, 127));
    const DeviceProfile& prof = deviceProfile(activeDevice_);
    if (prof.family == MidiStandard::GM2 || (prof.nativeSoundCtrl && soundParamGm2Cc(p) >= 0 && prof.family != MidiStandard::XG)) {
        int c = soundParamGm2Cc(p);
        if (c >= 0) v.push_back({b, uint8_t(c), val});
        return v;
    }
    if (prof.family == MidiStandard::GS || prof.family == MidiStandard::XG) {
        int lsb = soundParamNrpnLsb(p);
        if (lsb < 0) return v;
        v.push_back({b, 99, 1});
        v.push_back({b, 98, uint8_t(lsb)});
        v.push_back({b, 6, val});
        v.push_back({b, 101, 127});
        v.push_back({b, 100, 127});
    }
    return v;
}

void Player::setSoundParam(int port, int ch, SoundParam p, int value, bool lock) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    std::vector<Bytes> msgs;
    {
        auto lk = uiLock();
        msgs = soundParamMessages(ch, port, p, value);
        if (msgs.empty()) return;
        if (lock) {
            locks_[port][ch].sound |= uint16_t(1u << int(p));
            user_[port][ch].sound[int(p)] = uint8_t(value);
        }
        for (const Bytes& b : msgs) emitDirectLocked(port, b);
    }
}

void Player::setDrumPart(int port, int ch, int map) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    const DeviceProfile& prof = profileLocked();
    ChannelState& c = state_->ports[port].ch[ch];
    uint8_t prog = c.program;
    switch (prof.family) {
    case MidiStandard::GS:
        emitDirectLocked(port, gsSysex(0x401015 | (uint32_t(gsBlockOfChannel(ch)) << 8), {uint8_t(std::clamp(map, 0, 2))}));
        emitDirectLocked(port, Bytes{uint8_t(0xC0 | ch), map ? uint8_t(0) : prog});
        break;
    case MidiStandard::XG:
        emitDirectLocked(port, xgSysex(0x08, uint8_t(port * 16 + ch), 0x07, {uint8_t(map ? (map == 2 ? 3 : 2) : 0)}));
        emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), 0, uint8_t(map ? 127 : 0)});
        emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), 32, 0});
        emitDirectLocked(port, Bytes{uint8_t(0xC0 | ch), map ? uint8_t(0) : prog});
        break;
    case MidiStandard::GM2:
        emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), 0, uint8_t(map ? 120 : 121)});
        emitDirectLocked(port, Bytes{uint8_t(0xB0 | ch), 32, 0});
        emitDirectLocked(port, Bytes{uint8_t(0xC0 | ch), map ? uint8_t(0) : prog});
        break;
    default:
        break;
    }
}

void Player::setKeyShift(int port, int ch, int semis) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    semis = std::clamp(semis, -24, 24);
    uint8_t v = uint8_t(64 + semis);
    const DeviceProfile& prof = profileLocked();
    switch (prof.family) {
    case MidiStandard::GS: emitDirectLocked(port, gsSysex(0x401016 | (uint32_t(gsBlockOfChannel(ch)) << 8), {v})); break;
    case MidiStandard::XG: emitDirectLocked(port, xgSysex(0x08, uint8_t(port * 16 + ch), 0x08, {v})); break;
    default: {
        uint8_t b = uint8_t(0xB0 | ch);
        for (const Bytes& m : {Bytes{b, 101, 0}, Bytes{b, 100, 2}, Bytes{b, 6, v}, Bytes{b, 101, 127}, Bytes{b, 100, 127}})
            emitDirectLocked(port, m);
        state_->ports[port].ch[ch].keyShift = int8_t(semis);
        break;
    }
    }
}

void Player::setBendRange(int port, int ch, int semis) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    uint8_t b = uint8_t(0xB0 | ch);
    uint8_t v = uint8_t(std::clamp(semis, 0, 24));
    for (const Bytes& m : {Bytes{b, 101, 0}, Bytes{b, 100, 0}, Bytes{b, 6, v}, Bytes{b, 38, 0}, Bytes{b, 101, 127}, Bytes{b, 100, 127}})
        emitDirectLocked(port, m);
}

void Player::setEfxAssign(int port, int ch, bool on) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    emitDirectLocked(port, gsSysex(0x404022 | (uint32_t(gsBlockOfChannel(ch)) << 8), {uint8_t(on ? 1 : 0)}));
}

void Player::unlockCC(int port, int ch, int cc) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15 || cc < 0 || cc > 127) return;
    auto lk = uiLock();
    if (!locks_[port][ch].cc[size_t(cc)]) return;
    locks_[port][ch].cc.reset(size_t(cc));
    uint8_t m[3] = {uint8_t(0xB0 | ch), uint8_t(cc), fileCh_[port][ch].cc[cc]};
    emitFromFileLocked(port, m, 3);
}

void Player::unlock(int port, int ch) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15) return;
    auto lk = uiLock();
    ChannelLocks L = locks_[port][ch];
    locks_[port][ch] = ChannelLocks();
    FileChan& f = fileCh_[port][ch];
    uint8_t b = uint8_t(0xB0 | ch);
    for (int c = 0; c < 128; c++) {
        if (!L.cc[size_t(c)]) continue;
        uint8_t m[3] = {b, uint8_t(c), f.cc[c]};
        emitFromFileLocked(port, m, 3);
    }
    if (L.program) {
        uint8_t m1[3] = {b, 0, f.msb}, m2[3] = {b, 32, f.lsb}, m3[2] = {uint8_t(0xC0 | ch), f.prog};
        emitFromFileLocked(port, m1, 3);
        emitFromFileLocked(port, m2, 3);
        emitFromFileLocked(port, m3, 2);
    }
    if (L.bend) {
        uint8_t m[3] = {uint8_t(0xE0 | ch), uint8_t(f.bend & 0x7F), uint8_t(f.bend >> 7)};
        emitFromFileLocked(port, m, 3);
    }
    for (int i = 0; i < kSoundParamCount; i++)
        if (L.sound & (1u << i))
            for (const Bytes& m : soundParamMessages(ch, port, SoundParam(i), f.sound[i])) emitDirectLocked(port, m);
}

void Player::resendLocksLocked() {
    for (int p = 0; p < kMaxPorts; p++)
        for (int ch = 0; ch < 16; ch++) {
            const ChannelLocks& L = locks_[p][ch];
            if (!L.any()) continue;
            const UserValues& u = user_[p][ch];
            uint8_t b = uint8_t(0xB0 | ch);
            if (L.program) {
                emitDirectLocked(p, Bytes{b, 0, u.msb});
                emitDirectLocked(p, Bytes{b, 32, u.lsb});
                emitDirectLocked(p, Bytes{uint8_t(0xC0 | ch), u.prog});
            }
            for (int c = 0; c < 128; c++)
                if (L.cc[size_t(c)]) emitDirectLocked(p, Bytes{b, uint8_t(c), u.cc[c]});
            if (L.bend) emitDirectLocked(p, Bytes{uint8_t(0xE0 | ch), uint8_t(u.bend & 0x7F), uint8_t(u.bend >> 7)});
            for (int i = 0; i < kSoundParamCount; i++)
                if (L.sound & (1u << i))
                    for (const Bytes& m : soundParamMessages(ch, p, SoundParam(i), u.sound[i])) emitDirectLocked(p, m);
        }
}

void Player::previewNote(int port, int ch, int note, int velocity) {
    if (port < 0 || port >= kMaxPorts || ch < 0 || ch > 15 || note < 0 || note > 127) return;
    auto lk = uiLock();
    if (velocity > 0) emitDirectLocked(port, Bytes{uint8_t(0x90 | ch), uint8_t(note), uint8_t(std::min(velocity, 127))});
    else emitDirectLocked(port, Bytes{uint8_t(0x80 | ch), uint8_t(note), 0});
}

void Player::snapshot(PlayerSnapshot& s) const {
    auto lk = uiLock();
    s.numPorts = usedPortCount();
    for (int p = 0; p < s.numPorts; p++) {
        s.ports[p] = state_->ports[p];
        for (int c = 0; c < 16; c++) s.locks[p][c] = locks_[p][c];
        s.mute[p] = mute_[p];
        s.solo[p] = solo_[p];
    }
    s.positionUs = songUsLocked(Clock::now());
    if (file_) s.positionUs = std::min(s.positionUs, file_->lengthUs);
    s.playing = playing_;
    s.starting = starting_ || startPending_;
    s.speed = speed_;
    s.transpose = transpose_;
    s.device = activeDevice_;
    s.source = file_ ? info_.standard : MidiStandard::GM;
    s.emulating = emu_.active();
    s.masterVolume = masterVolume_;
    s.loopEnabled = loopEnabled_;
    s.loopRepeats = loopRepeats_;
    s.loopsDone = loopsDone_;
    s.skippedNotes = skippedNotes_;
    s.lagMs = int(lagUs_ / 1000);
    s.forcedToneMap = emu_.forcedToneMap();
}

} // namespace immidi

#include "SoftSynth.h"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>

// Ask the linker for AudioToolbox from this object file itself (a Mach-O LC_LINKER_OPTION, as
// Xcode's framework auto-linking does), so the program links even when a generated makefile
// predates this file. premake5.lua lists the framework as well.
#if defined(__MACH__)  // a Mach-O assembler directive
__asm__(".linker_option \"-framework\", \"AudioToolbox\"");
__asm__(".linker_option \"-framework\", \"CoreFoundation\"");
#endif
#endif

#include <filesystem>
#include <unordered_set>

namespace immidi {

#if defined(__APPLE__)

namespace {

std::string osStatusText(const char* what, OSStatus st) { return std::string(what) + " failed (OSStatus " + std::to_string(int(st)) + ")"; }

// Apple synth -> default output, connected directly (no AUGraph, which is deprecated).
// Without a sound bank this is the DLS synth with the built-in GS sound set. With one it is
// AUMIDISynth: Apple documents kMusicDeviceProperty_SoundBankURL as read-only for the DLS synth and
// writable for AUMIDISynth. AUMIDISynth only plays instruments that were loaded beforehand in its
// "preload" mode (a program change then loads the instrument instead of selecting it), so
// instruments are loaded as songs and program changes ask for them.
class AppleSynth final : public SoftSynth {
public:
    ~AppleSynth() override {
        if (output_) {
            AudioOutputUnitStop(output_);
            AudioUnitUninitialize(output_);
            AudioComponentInstanceDispose(output_);
        }
        if (synth_) {
            AudioUnitUninitialize(synth_);
            AudioComponentInstanceDispose(synth_);
        }
    }

    bool open(const std::string& soundBank, std::string* error) {
        midiSynth_ = !soundBank.empty();
        AudioComponentDescription synthDesc = {kAudioUnitType_MusicDevice, midiSynth_ ? kAudioUnitSubType_MIDISynth : kAudioUnitSubType_DLSSynth,
                                               kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponentDescription outDesc = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent synthComp = AudioComponentFindNext(nullptr, &synthDesc);
        AudioComponent outComp = AudioComponentFindNext(nullptr, &outDesc);
        if (!synthComp || !outComp) {
            if (error) *error = "Apple's synthesizer or the default audio output is not available";
            return false;
        }
        OSStatus st = AudioComponentInstanceNew(synthComp, &synth_);
        if (st != noErr) return fail(error, "Creating the synthesizer", st);
        if (midiSynth_) {
            // The bank is set before the synth is initialized.
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(soundBank.data()),
                                                                   CFIndex(soundBank.size()), false);
            if (!url) {
                if (error) *error = "Invalid sound bank path";
                return false;
            }
            st = AudioUnitSetProperty(synth_, kMusicDeviceProperty_SoundBankURL, kAudioUnitScope_Global, 0, &url, sizeof(url));
            CFRelease(url);
            if (st != noErr) return fail(error, "Loading the sound bank", st);
        }
        st = AudioComponentInstanceNew(outComp, &output_);
        if (st != noErr) return fail(error, "Creating the audio output", st);
        st = AudioUnitInitialize(synth_);
        if (st != noErr) return fail(error, midiSynth_ ? "Loading the sound bank" : "Initializing the synthesizer", st);

        // The output takes the synth's format (float stereo at the synth's sample rate).
        AudioStreamBasicDescription fmt = {};
        UInt32 size = sizeof fmt;
        if (AudioUnitGetProperty(synth_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fmt, &size) == noErr)
            AudioUnitSetProperty(output_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
        AudioUnitConnection conn = {synth_, 0, 0};
        st = AudioUnitSetProperty(output_, kAudioUnitProperty_MakeConnection, kAudioUnitScope_Input, 0, &conn, sizeof conn);
        if (st != noErr) return fail(error, "Connecting the synthesizer to the output", st);
        st = AudioUnitInitialize(output_);
        if (st != noErr) return fail(error, "Initializing the audio output", st);
        st = AudioOutputUnitStart(output_);
        if (st != noErr) return fail(error, "Starting the audio output", st);
        if (midiSynth_) {
            // What plays without any program change: program 0 on every channel (piano, and the
            // standard drum kit on channel 10).
            std::vector<ProgramRef> defaults;
            for (uint8_t ch = 0; ch < 16; ch++) defaults.push_back(ProgramRef{ch, 0, 0, 0});
            preload(defaults);
        }
        return true;
    }

    void send(const uint8_t* msg, size_t len) override {
        if (!synth_ || len == 0) return;
        uint8_t s = msg[0];
        if (s == 0xF0) {
            MusicDeviceSysEx(synth_, msg, UInt32(len));
            return;
        }
        if (s < 0x80 || s >= 0xF0) return;
        uint8_t d1 = len > 1 ? msg[1] : 0, d2 = len > 2 ? msg[2] : 0;
        if (midiSynth_) {
            int ch = s & 0x0F;
            if ((s & 0xF0) == 0xB0 && d1 == 0) msb_[ch] = d2;
            else if ((s & 0xF0) == 0xB0 && d1 == 32) lsb_[ch] = d2;
            else if ((s & 0xF0) == 0xC0) load(ProgramRef{uint8_t(ch), msb_[ch], lsb_[ch], d1});
        }
        MusicDeviceMIDIEvent(synth_, s, d1, d2, 0);
    }

    void preload(const std::vector<ProgramRef>& programs) override {
        if (!midiSynth_) return;
        for (const ProgramRef& p : programs) load(p);
        // Loading leaves the bank selects of the channels changed: restore what the song set.
        for (int ch = 0; ch < 16; ch++) {
            MusicDeviceMIDIEvent(synth_, UInt32(0xB0 | ch), 0, msb_[ch], 0);
            MusicDeviceMIDIEvent(synth_, UInt32(0xB0 | ch), 32, lsb_[ch], 0);
        }
    }

private:
    // Loads one instrument (once per channel: Apple does not document whether a loaded instrument
    // is available on every channel) in AUMIDISynth's preload mode.
    void load(const ProgramRef& p) {
        uint32_t key = (uint32_t(p.channel & 0x0F) << 24) | (uint32_t(p.bankMsb) << 16) | (uint32_t(p.bankLsb) << 8) | p.program;
        if (!loaded_.insert(key).second) return;
        UInt32 on = 1, off = 0;
        AudioUnitSetProperty(synth_, kAUMIDISynthProperty_EnablePreload, kAudioUnitScope_Global, 0, &on, sizeof on);
        MusicDeviceMIDIEvent(synth_, UInt32(0xB0 | p.channel), 0, p.bankMsb, 0);
        MusicDeviceMIDIEvent(synth_, UInt32(0xB0 | p.channel), 32, p.bankLsb, 0);
        MusicDeviceMIDIEvent(synth_, UInt32(0xC0 | p.channel), p.program, 0, 0);
        AudioUnitSetProperty(synth_, kAUMIDISynthProperty_EnablePreload, kAudioUnitScope_Global, 0, &off, sizeof off);
    }

    bool fail(std::string* error, const char* what, OSStatus st) {
        if (error) *error = osStatusText(what, st);
        return false;
    }
    AudioUnit synth_ = nullptr;
    AudioUnit output_ = nullptr;
    bool midiSynth_ = false;
    uint8_t msb_[16] = {}, lsb_[16] = {};
    std::unordered_set<uint32_t> loaded_;
};

} // namespace

std::unique_ptr<SoftSynth> createAppleDlsSynth(const std::string& soundBank, std::string* error, std::string* bankError) {
    if (bankError) bankError->clear();
    if (!soundBank.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(soundBank), ec)) {
            if (bankError) *bankError = "Sound bank not found: " + soundBank;
        } else {
            auto s = std::make_unique<AppleSynth>();
            std::string why;
            if (s->open(soundBank, &why)) return s;
            // A bank the synth cannot read: play with the built-in sounds instead.
            if (bankError) *bankError = why + ": " + soundBank;
        }
    }
    auto s = std::make_unique<AppleSynth>();
    if (!s->open(std::string(), error)) return nullptr;
    return s;
}

#else

std::unique_ptr<SoftSynth> createAppleDlsSynth(const std::string&, std::string* error, std::string*) {
    if (error) *error = "Apple DLS Synth is only available on macOS";
    return nullptr;
}

#endif

} // namespace immidi

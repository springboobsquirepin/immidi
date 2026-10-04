#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace immidi {

// An instrument a song selects: bank select MSB / LSB and program on a channel.
struct ProgramRef {
    uint8_t channel = 0, bankMsb = 0, bankLsb = 0, program = 0;
};

// A synthesizer inside the program, used as a MIDI output device instead of a MIDI port.
class SoftSynth {
public:
    virtual ~SoftSynth() = default;
    // One complete message: a channel message (1-3 bytes) or a SysEx (F0 ... F7).
    virtual void send(const uint8_t* msg, size_t len) = 0;
    // Loads instruments ahead of playback, for synths that load them on demand.
    virtual void preload(const std::vector<ProgramRef>& programs) { (void)programs; }
};

// macOS: Apple's built-in synthesizer playing through the default audio output: the DLS synth Audio
// Unit with the General MIDI / GS sound set that ships with every Mac, or, with `soundBank` (a
// SoundFont 2 .sf2 or DLS .dls file), Apple's AUMIDISynth playing that bank. When the bank cannot be
// loaded, the synth opens with the built-in sounds and `bankError` says why. Null elsewhere or on failure.
std::unique_ptr<SoftSynth> createAppleDlsSynth(const std::string& soundBank, std::string* error, std::string* bankError = nullptr);

} // namespace immidi

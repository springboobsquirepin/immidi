#pragma once
#include <string>
#include <vector>

class RtMidi;

namespace immidi {

// Port names as ImMidi shows and saves them. RtMidi appends a number to every name to keep them
// unique (the device index on Windows, the ALSA client:port on Linux); it changes when devices come
// and go, so it is left out, and " (2)", " (3)" ... are added only for real duplicates.
struct PortName {
    std::string display;
    std::string raw;  // as RtMidi reports it
};
std::string stripPortNumber(const std::string& raw);
std::vector<PortName> listMidiPorts(RtMidi& api);

} // namespace immidi

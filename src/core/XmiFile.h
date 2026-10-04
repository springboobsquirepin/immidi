#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace immidi {

// True for Miles Sound System eXtended MIDI data (IFF "FORM XDIR" / "FORM XMID" / "CAT XMID").
bool isXmiData(const uint8_t* d, size_t size);

// Converts one sequence of an XMIDI file into a format 0 SMF. XMIDI timing runs at a fixed 120 Hz
// and its tempo events only describe the music, so event times are mapped through the tempo map
// to musical ticks (960 PPQN): the result plays at the original speed and shows the right tempo
// and bars. Note-ons carry their own duration in XMIDI; matching note-offs are generated.
// `sequenceCount` receives the number of sequences in the file.
bool xmiToSmf(const uint8_t* d, size_t size, std::vector<uint8_t>& smf, std::string& error, int sequence = 0,
              int* sequenceCount = nullptr);

} // namespace immidi

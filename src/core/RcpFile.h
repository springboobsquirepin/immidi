#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace immidi {

// True when the data starts like a Recomposer (Come On Music, PC-9801) song file.
bool isRcpData(const uint8_t* d, size_t size);

// Converts a Recomposer 2.0 song (.RCP, "RCM-PC98V2.0") into a format 1 SMF. Loops and "same
// measure" references are expanded; an endless loop is played twice and marked with
// loopStart/loopEnd markers when every track loops over the same range.
bool rcpToSmf(const uint8_t* d, size_t size, std::vector<uint8_t>& smf, std::string& error);

} // namespace immidi

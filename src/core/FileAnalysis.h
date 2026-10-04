#pragma once
#include "MidiFile.h"
#include "Standards.h"

#include <string>

namespace immidi {

struct FileInfo {
    MidiStandard standard = MidiStandard::GM;
    DeviceKind suggestedDevice = DeviceKind::GM;
    bool hasGmOn = false, hasGm2On = false, hasGsReset = false, hasXgOn = false, hasModeSet = false;
    bool hasGsSysex = false, hasXgSysex = false;
    bool usesGsEfx = false, usesGsDelay = false, usesGsToneMap = false;
    int maxGsMap = 0;
    // Sound module named in the song's text events ("for SC-88Pro", "ＭＵ８０対応", ...), empty if none.
    std::string moduleKeyword;
    uint16_t drumChannels[kMaxPorts] = {};  // channels that act as drum parts at any time
    // Set when the user chose the module the song is made for (songs whose messages do not tell, e.g.
    // an SC-8850 song without a GS reset): standard and suggestedDevice then follow the choice, and
    // these keep what the analysis found.
    bool moduleChosen = false;
    MidiStandard detectedStandard = MidiStandard::GM;
    DeviceKind detectedDevice = DeviceKind::GM;
    std::string summary() const;
};

FileInfo analyzeFile(const MidiFile& f);
// The analysis of a song made for `module`, as the user says.
FileInfo withSongModule(const FileInfo& detected, DeviceKind module);

} // namespace immidi

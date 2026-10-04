#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace immidi {

// The Standard MIDI File behind a song file: the file itself (also with leading junk or inside a
// RIFF RMID file), or the converted SMF of MIDI stream (.mds), Recomposer (.rcp) and XMIDI files.
// Its ticks match the loop points MidiFile finds when loading the same file.
bool smfBytesForFile(const std::string& path, std::vector<uint8_t>& smf, std::string& error);

// Rewrites an SMF for export. With a loop (0 <= loopStartTick < loopEndTick) the loop is marked the
// way RPG Maker expects: CC#111 at the loop start (unless one is already there) and every track ending
// at the loop end, where notes still sounding are released and events of the next pass (note-ons,
// program and controller changes) are left out. Without a loop the song is written unchanged.
// Format 2 files (independent sequences) are never cut.
// For Apogee EMIDI files pass `emidi`: the tracks flagged in `excludedTracks` (not meant for a
// General MIDI device) are left out and the track designation controllers (CC#110/111) removed, so
// the result plays like it does in ImMidi, and the CC#111 loop start cannot be read as an EMIDI one.
bool applyRpgMakerLoop(const std::vector<uint8_t>& in, int64_t loopStartTick, int64_t loopEndTick, std::vector<uint8_t>& out,
                       std::string& error, bool emidi = false, const std::vector<bool>& excludedTracks = {});

class Emulator;

// The SMF as a module receives it from ImMidi with emulation: every event goes through `emu` (set up
// for the song's standard and the target module, with its conversion tables) on the port it plays on,
// as the player sends it. Each event's messages replace it in its track at its tick; tracks, timing,
// tempo, texts and the other meta events stay as they are.
bool applyEmulation(const std::vector<uint8_t>& in, Emulator& emu, std::vector<uint8_t>& out, std::string& error);

} // namespace immidi

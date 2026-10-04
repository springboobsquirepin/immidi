// Command line companion for ImMidi: inspects MIDI files and plays them headless.
//   immidi_cli info <file>...
//   immidi_cli ports
//   immidi_cli play <file> [--out NAME] [--device KIND] [--seconds N] [--seek S] [--speed F] [--transpose N] [--no-emu]
//   immidi_cli emulate <file> --device KIND      (prints converted SysEx/bank messages)
//   immidi_cli ins <dir> [instrument]
#include "ConversionDoc.h"
#include "ConversionTables.h"
#include "Emulation.h"
#include "FileAnalysis.h"
#include "InsDef.h"
#include "Json.h"
#include "MidiBridge.h"
#include "MidiFile.h"
#include "MidiOut.h"
#include "Player.h"
#include "SmfExport.h"
#include "SongModules.h"
#include "Playlist.h"
#include "SynthState.h"
#include "Util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

using namespace immidi;

static DeviceKind parseDevice(const std::string& s) {
    DeviceKind d;
    if (deviceFromId(s, d)) return d;
    fprintf(stderr, "unknown device %s, using SC-88Pro\n", s.c_str());
    return DeviceKind::SC88Pro;
}

static std::string hex(const Bytes& b) {
    std::string s;
    char h[4];
    for (uint8_t x : b) {
        snprintf(h, sizeof h, "%02X ", x);
        s += h;
    }
    return s;
}

static int cmdConvert(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: immidi_cli convert <song> out.mid [--raw] [--emulate DEVICE [--force-map N] [--tables DIR] [--module ID]]\n"
               "  Writes the song (.mid, .rmi, .mds, .rcp, .xmi, ...) as a Standard MIDI File. A loop is marked\n"
               "  the RPG Maker way (CC#111 at the loop start, the song ends at the loop end); --raw writes the\n"
               "  conversion unchanged. --emulate writes the song as ImMidi sends it to DEVICE (gm, gm2, sc55,\n"
               "  sc88, sc88pro, sc8850, xg) with emulation, using the conversion tables in DIR; --module sets the\n"
               "  module the song is made for.\n");
        return 1;
    }
    bool raw = false, emulate = false;
    DeviceKind target = DeviceKind::SC88Pro, module = DeviceKind::GM;
    bool haveModule = false;
    int forceMap = 0;
    std::string tablesDir;
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--raw") raw = true;
        else if (a == "--emulate" && i + 1 < argc) {
            emulate = true;
            target = parseDevice(argv[++i]);
        } else if (a == "--force-map" && i + 1 < argc) forceMap = atoi(argv[++i]);
        else if (a == "--tables" && i + 1 < argc) tablesDir = argv[++i];
        else if (a == "--module" && i + 1 < argc) {
            module = parseDevice(argv[++i]);
            haveModule = true;
        }
    }
    std::vector<uint8_t> smf, out;
    std::string err;
    if (!smfBytesForFile(argv[2], smf, err)) {
        printf("%s: %s\n", argv[2], err.c_str());
        return 1;
    }
    MidiFile f;
    if (!f.loadFromMemory(smf.data(), smf.size(), err)) {
        printf("%s: %s\n", argv[2], err.c_str());
        return 1;
    }
    std::vector<bool> excluded;
    for (const TrackStat& t : f.trackStats) excluded.push_back(t.excluded);
    if (raw) {
        out = smf;
    } else if (!applyRpgMakerLoop(smf, f.hasLoop() ? f.loopStartTick : -1, f.hasLoop() ? f.loopEndTick : -1, out, err, f.emidi, excluded)) {
        printf("%s: %s\n", argv[2], err.c_str());
        return 1;
    }
    std::string conversion;
    if (emulate) {
        FileInfo info = analyzeFile(f);
        if (haveModule) info = withSongModule(info, module);
        Emulator emu;
        emu.configure(info.standard, deviceProfile(target), forceMap);
        if (!tablesDir.empty()) {
            auto t = std::make_shared<ConversionTables>();
            if (!t->load(tablesDir, err)) {
                printf("conversion tables: %s\n", err.c_str());
                return 1;
            }
            ConvSetup setup = ConversionTables::setupFor(info.standard, info.suggestedDevice, target, emu.forcedToneMap());
            conversion = setup.describe();
            emu.setConversion(t, setup);
        }
        std::vector<uint8_t> emulated;
        if (!applyEmulation(out, emu, emulated, err)) {
            printf("%s: %s\n", argv[2], err.c_str());
            return 1;
        }
        out.swap(emulated);
        printf("emulation: %s -> %s%s%s\n", standardName(info.standard), deviceProfile(target).name, conversion.empty() ? "" : ", tables ",
               conversion.c_str());
    }
    if (!writeFileAtomic(argv[3], std::string(out.begin(), out.end()))) {
        printf("cannot write %s\n", argv[3]);
        return 1;
    }
    printf("%s -> %s (%zu bytes)", argv[2], argv[3], out.size());
    if (f.hasLoop() && !raw) printf(", loop %lld..%lld marked with CC#111", (long long)f.loopStartTick, (long long)f.loopEndTick);
    printf("\n");
    return 0;
}

static int cmdInfo(int argc, char** argv) {
    for (int i = 2; i < argc; i++) {
        MidiFile f;
        std::string err;
        if (!f.load(argv[i], err)) {
            printf("%s: ERROR %s\n", argv[i], err.c_str());
            continue;
        }
        FileInfo info = analyzeFile(f);
        printf("== %s\n", f.fileName.c_str());
        printf("  format %d, tracks %d, division %s, resolution %s\n", f.format, f.numTracks, f.divisionDescription().c_str(),
               f.resolutionDescription().c_str());
        printf("  length %s (%llu ticks), events %zu, notes %zu, sysex %zu, ports %d\n", formatTime(f.lengthUs / 1e6).c_str(),
               (unsigned long long)f.lengthTicks, f.events.size(), f.noteCount, f.sysexCount, f.numPorts);
        printf("  encoding %s, title \"%s\", copyright \"%s\"\n", textEncodingName(f.encoding), f.title.c_str(), f.copyright.c_str());
        printf("  standard %s, suggested device %s\n", info.summary().c_str(), deviceProfile(info.suggestedDevice).name);
        if (!info.moduleKeyword.empty()) printf("  module named in text: %s\n", info.moduleKeyword.c_str());
        for (int p = 0; p < f.numPorts; p++) {
            printf("  port %c: channels", 'A' + p);
            for (int c = 0; c < 16; c++)
                if (f.channelMask[p] & (1 << c)) printf(" %d", c + 1);
            printf("; drum channels");
            for (int c = 0; c < 16; c++)
                if (info.drumChannels[p] & (1 << c)) printf(" %d", c + 1);
            printf("\n");
        }
        printf("  tempo %.2f BPM at start, %zu tempo changes, lyrics lines %zu (%s)\n", f.bpmAtTick(0), f.tempos.size(), f.lyrics.size(),
               f.lyricsFromKar ? "KAR" : "lyric events");
        if (f.hasLoop())
            printf("  loop (%s): %s -> %s, count %d%s\n", f.loopType.c_str(), formatTime(f.loopStartUs / 1e6, true).c_str(),
                   formatTime(f.loopEndUs / 1e6, true).c_str(), f.loopCount, f.emidi ? " [EMIDI]" : "");
        for (size_t l = 0; l < f.lyrics.size() && l < 6; l++) {
            printf("    [%s] ", formatTime(f.lyrics[l].startUs() / 1e6).c_str());
            for (auto& s : f.lyrics[l].syllables) printf("%s", s.text.c_str());
            printf("\n");
        }
    }
    return 0;
}

// Every lyric line with its time, as the lyrics view shows them: a blank line between verses.
static int cmdLyrics(int argc, char** argv) {
    for (int i = 2; i < argc; i++) {
        MidiFile f;
        std::string err;
        if (!f.load(argv[i], err)) {
            printf("%s: ERROR %s\n", argv[i], err.c_str());
            continue;
        }
        size_t verses = 0;
        for (const LyricLine& l : f.lyrics) verses += l.paragraphStart ? 1 : 0;
        printf("== %s: %zu lines, %zu verses (%s, %s)\n", f.fileName.c_str(), f.lyrics.size(), verses,
               f.lyricsFromKar ? "KAR text events" : "lyric events", textEncodingName(f.encoding));
        for (size_t l = 0; l < f.lyrics.size(); l++) {
            if (l && f.lyrics[l].paragraphStart) printf("\n");
            printf("  [%s] ", formatTime(f.lyrics[l].startUs() / 1e6, true).c_str());
            for (auto& s : f.lyrics[l].syllables) printf("%s", s.text.c_str());
            if (f.lyrics[l].endUs) printf("  (ends %s)", formatTime(f.lyrics[l].endUs / 1e6, true).c_str());
            printf("\n");
        }
    }
    return 0;
}

static int cmdPorts() {
    MidiOutputs outs;
    for (const std::string& n : outs.availableDevices()) printf("%s\n", n.c_str());
    return 0;
}

static int cmdEmulate(int argc, char** argv) {
    if (argc < 3) return 1;
    DeviceKind dev = DeviceKind::SC88Pro;
    int forceMap = 0;
    std::string tables;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) dev = parseDevice(argv[++i]);
        else if (!strcmp(argv[i], "--force-map") && i + 1 < argc) forceMap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tables") && i + 1 < argc) tables = argv[++i];
    }
    MidiFile f;
    std::string err;
    if (!f.load(argv[2], err)) {
        printf("ERROR %s\n", err.c_str());
        return 1;
    }
    FileInfo info = analyzeFile(f);
    Emulator emu;
    emu.configure(info.standard, deviceProfile(dev), forceMap);
    if (!tables.empty()) {
        auto t = std::make_shared<ConversionTables>();
        std::string terr;
        if (!t->load(tables, terr)) {
            printf("ERROR conversion tables: %s\n", terr.c_str());
            return 1;
        }
        ConvSetup setup = ConversionTables::setupFor(info.standard, info.suggestedDevice, dev, emu.forcedToneMap());
        printf("conversion tables: %s\n", setup.direction == ConvDirection::None ? "(none for this song and device)" : setup.describe().c_str());
        emu.setConversion(t, setup);
    }
    printf("source %s -> %s, active %d\n", standardName(info.standard), deviceProfile(dev).name, emu.active());
    std::vector<Bytes> out;
    int shown = 0;
    for (const MidiEvent& e : f.events) {
        out.clear();
        if (e.status == 0xF0) {
            emu.convert(e.port, f.payload(e), f.payloadLen(e), out);
            Bytes in(f.payload(e), f.payload(e) + f.payloadLen(e));
            if (shown < 400) {
                printf("[%c %s] SX %s\n", 'A' + e.port, formatTime(e.timeUs / 1e6, true).c_str(), hex(in).c_str());
                for (auto& b : out) printf("        -> %s\n", hex(b).c_str());
                shown++;
            }
        } else if (e.isChannel() && (e.type() == 0xC0 || (e.type() == 0xB0 && (e.d1 == 0 || e.d1 == 32)))) {
            uint8_t m[3] = {e.status, e.d1, e.d2};
            emu.convert(e.port, m, e.type() == 0xC0 ? 2 : 3, out);
            if (shown < 400 && !out.empty() && e.type() == 0xC0) {
                printf("[%c %s] ch%d PC %d ->", 'A' + e.port, formatTime(e.timeUs / 1e6, true).c_str(), e.channel() + 1, e.d1);
                for (auto& b : out) printf(" [%s]", hex(b).c_str());
                printf("\n");
                shown++;
            }
        }
    }
    return 0;
}

static int cmdPlay(int argc, char** argv) {
    if (argc < 3) return 1;
    std::string outName, file = argv[2];
    DeviceKind dev = DeviceKind::SC88Pro;
    bool follow = true, emu = false;
    double seconds = 10, seek = 0, speed = 1.0;
    int transpose = 0;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outName = argv[++i];
        else if (a == "--device" && i + 1 < argc) {
            dev = parseDevice(argv[++i]);
            follow = false;
        } else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--seek" && i + 1 < argc) seek = atof(argv[++i]);
        else if (a == "--speed" && i + 1 < argc) speed = atof(argv[++i]);
        else if (a == "--transpose" && i + 1 < argc) transpose = atoi(argv[++i]);
        else if (a == "--emulate") emu = true;
        else if (a == "--no-emu") emu = false;
    }
    MidiOutputs outs;
    if (outName.empty()) {
        auto devs = outs.availableDevices();
        for (auto& d : devs)
            if (d != MidiOutputs::kNone && d != MidiOutputs::kVirtual) {
                outName = d;
                break;
            }
    }
    std::string err;
    auto mf = std::make_shared<MidiFile>();
    if (!mf->load(file, err)) {
        printf("ERROR %s\n", err.c_str());
        return 1;
    }
    for (int p = 0; p < mf->numPorts; p++)
        if (!outs.setPortDevice(p, outName, &err)) printf("port %d: %s\n", p, err.c_str());
    printf("output: %s\n", outName.c_str());
    Player player(outs);
    player.setDevice(dev, follow);
    player.setEmulation(emu);
    player.setFile(mf);
    player.setSpeed(speed);
    player.setTranspose(transpose);
    if (seek > 0) player.seek(int64_t(seek * 1e6));
    player.play();
    auto t0 = std::chrono::steady_clock::now();
    auto snap = std::make_unique<PlayerSnapshot>();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        auto s0 = std::chrono::steady_clock::now();
        player.snapshot(*snap);
        double snapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
        int notes = 0;
        std::string drums;
        for (int p = 0; p < snap->numPorts; p++)
            for (int c = 0; c < 16; c++) {
                notes += snap->ports[p].ch[c].activeNotes;
                if (snap->ports[p].ch[c].drum) drums += std::string(1, char('A' + p)) + std::to_string(c + 1) + " ";
            }
        printf("t=%5.1fs pos=%s playing=%d notes=%d skipped=%llu lag=%dms snapshot=%.2fms device=%s drums=%s\n", el,
               formatTime(snap->positionUs / 1e6, true).c_str(), snap->playing, notes, (unsigned long long)snap->skippedNotes, snap->lagMs,
               snapMs, deviceProfile(snap->device).name, drums.c_str());
        fflush(stdout);
        if (el >= seconds || player.consumeFinished()) break;
    }
    player.stop();
    printf("bytes sent: %llu, messages dropped by the output: %llu\n", (unsigned long long)outs.bytesSent(),
           (unsigned long long)outs.messagesDropped());
    return 0;
}

static int cmdIns(int argc, char** argv) {
    if (argc < 3) return 1;
    InsLibrary lib;
    int n = lib.loadDirectory(argv[2]);
    printf("files %d, instruments %zu\n", n, lib.instruments().size());
    if (argc > 3) {
        const InsInstrument* ins = lib.find(argv[3]);
        if (!ins) {
            printf("not found\n");
            return 1;
        }
        printf("%s: %zu banks, drum=%d\n", ins->name.c_str(), ins->patchByBank.size(), ins->isDrumDefinition());
        for (auto& [bank, list] : ins->patchByBank) {
            printf("  bank %d (MSB %d LSB %d): %zu patches, first: %s\n", bank, bank >> 7, bank & 127, list->size(),
                   list->empty() ? "" : list->begin()->second.c_str());
        }
        if (argc > 5) {
            int bank = atoi(argv[4]), prog = atoi(argv[5]);
            printf("patch %d/%d = %s\n", bank, prog, InsLibrary::patchName(ins, bank, prog).c_str());
            for (int note = 35; note < 40; note++)
                printf("  note %d = %s\n", note, InsLibrary::noteName(ins, bank, prog, note).c_str());
        }
    }
    return 0;
}

static int failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) printf("  ok   ");                      \
        else {                                            \
            printf("  FAIL ");                            \
            failures++;                                   \
        }                                                 \
        printf(__VA_ARGS__);                              \
        printf("\n");                                    \
    } while (0)

// Drives the Player like the GUI does and checks the resulting device state.
static int cmdSelfTest(int argc, char** argv) {
    std::string dir = argc > 2 ? argv[2] : "example-midis";
    MidiOutputs outs;  // no device: messages only update the tracked state
    auto snap = std::make_unique<PlayerSnapshot>();
    auto wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };

    printf("[locks, mute, transpose] multidrum_gs.mid on SC-88Pro\n");
    {
        Player pl(outs);
        pl.setDevice(DeviceKind::SC88Pro, false);
        auto f = std::make_shared<MidiFile>();
        std::string err;
        if (!f->load(dir + "/multidrum_gs.mid", err)) { printf("load failed: %s\n", err.c_str()); return 1; }
        pl.setResetDelayMs(0);
        pl.setFile(f);
        pl.setSpeed(4.0);
        pl.play();
        wait(1500);
        pl.snapshot(*snap);
        CHECK(snap->ports[0].ch[8].drum && snap->ports[0].ch[9].drum && snap->ports[0].ch[10].drum && snap->ports[0].ch[15].drum,
              "GS drum parts 9/10/11/16 detected during playback");
        pl.setChannelCC(0, 0, 7, 30, true);
        wait(1500);
        pl.snapshot(*snap);
        CHECK(snap->ports[0].ch[0].cc[7] == 30 && snap->locks[0][0].cc[7], "locked volume stays at 30 (got %d)", snap->ports[0].ch[0].cc[7]);
        pl.unlock(0, 0);
        wait(200);
        pl.snapshot(*snap);
        CHECK(snap->ports[0].ch[0].cc[7] != 30 && !snap->locks[0][0].any(), "unlock restores the file value (got %d)", snap->ports[0].ch[0].cc[7]);
        pl.setProgram(0, 1, 8, 0, 40, true);
        wait(1500);
        pl.snapshot(*snap);
        CHECK(snap->ports[0].ch[1].program == 40 && snap->ports[0].ch[1].bankMsb == 8, "program lock holds (prg %d msb %d)",
              snap->ports[0].ch[1].program, snap->ports[0].ch[1].bankMsb);
        pl.setMute(0, 0, true);
        wait(800);
        pl.snapshot(*snap);
        CHECK(snap->ports[0].ch[0].activeNotes == 0, "muted channel 1 has no sounding notes");
        pl.setMute(0, 0, false);
        pl.setTranspose(5);
        int melodicHigh = 0, drumOk = 1;
        for (int i = 0; i < 20; i++) {
            wait(100);
            pl.snapshot(*snap);
            for (int n = 0; n < 128; n++) {
                if (snap->ports[0].ch[9].noteVel[n] && (n < 27 || n > 87)) drumOk = 0;
                if (snap->ports[0].ch[0].noteVel[n]) melodicHigh = 1;
            }
        }
        CHECK(drumOk, "drum notes are not transposed (stay in the drum map range)");
        pl.stop();
    }

    printf("[seek chase] multiport.mid, seek to 2:00 on SC-88Pro\n");
    {
        Player pl(outs);
        pl.setDevice(DeviceKind::SC88Pro, false);
        auto f = std::make_shared<MidiFile>();
        std::string err;
        f->load(dir + "/multiport.mid", err);
        pl.setResetDelayMs(0);
        pl.setFile(f);
        pl.seek(120000000);
        pl.play();
        wait(800);
        pl.snapshot(*snap);
        CHECK(snap->positionUs >= 120000000 && snap->positionUs < 122000000, "position after seek %.2f s", snap->positionUs / 1e6);
        CHECK(snap->ports[1].ch[8].drum && snap->ports[1].ch[9].drum, "port B drum parts restored by chase (50 xx xx SysEx)");
        CHECK(snap->ports[0].gsEfxMsb == 0x01, "SC-88Pro EFX type chased (%02X %02X)", snap->ports[0].gsEfxMsb, snap->ports[0].gsEfxLsb);
        int assigned = 0;
        for (int c = 0; c < 16; c++) assigned += snap->ports[0].ch[c].efxAssign;
        CHECK(assigned > 0, "EFX part assignment chased (%d parts)", assigned);
        pl.stop();
    }

    printf("[loops] looptype1.mid (markers) at 8x speed\n");
    {
        Player pl(outs);
        auto f = std::make_shared<MidiFile>();
        std::string err;
        f->load(dir + "/looptype1.mid", err);
        pl.setResetDelayMs(0);
        pl.setFile(f);
        pl.setLoop(true, 1);
        pl.seek(f->loopEndUs - 1500000);
        pl.setSpeed(1.0);
        pl.play();
        wait(2200);
        pl.snapshot(*snap);
        CHECK(snap->loopsDone == 1 && snap->positionUs < f->loopStartUs + 2000000, "jumped back to loop start (pos %.2f s, loops %d)",
              snap->positionUs / 1e6, snap->loopsDone);
        pl.stop();
    }

    printf("[emulation] XG file on SC-8850: drum parts via GS SysEx\n");
    {
        Player pl(outs);
        pl.setDevice(DeviceKind::SC8850, false);
        pl.setEmulation(true);
        auto f = std::make_shared<MidiFile>();
        std::string err;
        f->load(dir + "/multidrum_xg.mid", err);
        pl.setResetDelayMs(0);
        pl.setFile(f);
        pl.play();
        wait(1500);
        pl.snapshot(*snap);
        CHECK(snap->emulating && snap->ports[0].ch[9].drum && snap->ports[0].ch[10].drum && snap->ports[0].ch[11].drum && !snap->ports[0].ch[0].drum,
              "XG drums on channels 10/11/12 became GS drum parts");
        CHECK(snap->ports[0].gsEfxMsb == 0x01 && snap->ports[0].gsEfxLsb == 0x22, "XG insertion Rotary Speaker -> EFX Rotary (%02X %02X)",
              snap->ports[0].gsEfxMsb, snap->ports[0].gsEfxLsb);
        pl.stop();
    }
    printf("[emulation] off by default: GS song on an SC-55 passes unchanged\n");
    {
        Emulator emu;
        emu.configure(MidiStandard::GS, deviceProfile(DeviceKind::SC55), 0, false);
        std::vector<Bytes> out;
        const uint8_t bank[3] = {0xB0, 32, 3}, msb[3] = {0xB0, 0, 8};
        emu.convert(0, msb, 3, out);
        emu.convert(0, bank, 3, out);
        CHECK(!emu.active() && out.size() == 2 && out[0] == Bytes(msb, msb + 3) && out[1] == Bytes(bank, bank + 3),
              "bank select MSB 8 / map 3 sent as is (%zu messages)", out.size());
        Emulator forced;
        forced.configure(MidiStandard::GS, deviceProfile(DeviceKind::SC8850), 1, false);
        CHECK(forced.active() && !forced.forcedMapMessages().empty(), "a forced tone map still works with emulation off");
    }

    printf("[playlist] files dropped between entries\n");
    {
        Playlist pl;
        for (const char* n : {"a.mid", "b.mid", "c.mid"}) pl.add(n);
        pl.setCurrent(1);
        pl.add("x.mid");
        pl.add("y.mid");
        pl.placeAddedAt(3, 1);
        std::string order;
        for (auto& e : pl.entries) order += e.path.substr(0, 1);
        CHECK(order == "axybc" && pl.current == 3, "inserted before entry 2: %s, current entry still b (%d)", order.c_str(), pl.current);
    }

    printf("[playlist] multi-selection move and remove\n");
    {
        Playlist pl;
        for (const char* n : {"a", "b", "c", "d", "e", "f"}) pl.add(std::string(n) + ".mid");
        pl.setCurrent(2);  // c
        pl.moveEntries({1, 3}, 5);  // b, d before f
        std::string order;
        for (auto& e : pl.entries) order += e.path.substr(0, 1);
        CHECK(order == "acebdf" && pl.entries[size_t(pl.current)].path == "c.mid", "move b,d before f: %s (current %s)", order.c_str(),
              pl.entries[size_t(pl.current)].path.c_str());
        pl.removeEntries({0, 3, 4});  // a, b, d
        order.clear();
        for (auto& e : pl.entries) order += e.path.substr(0, 1);
        CHECK(order == "cef" && pl.current == 0, "remove a,b,d: %s (current %d)", order.c_str(), pl.current);
    }

    printf("[master tune] GS / XG / universal\n");
    {
        auto st = std::make_unique<SynthState>();
        st->resetAll(MidiStandard::GS);
        Bytes gs = gsSysex(0x400000, {0x00, 0x05, 0x00, 0x00});  // 0x0500 = +25.6 cents
        st->apply(0, gs.data(), gs.size());
        int a = st->ports[0].masterTune;
        Bytes xg = xgSysex(0x00, 0x00, 0x00, {0x00, 0x03, 0x00, 0x00});  // 0x0300 = -25.6 cents
        st->apply(0, xg.data(), xg.size());
        int b = st->ports[0].masterTune;
        Bytes uni = masterFineTuningSysex(-500);
        st->apply(0, uni.data(), uni.size());
        int c = st->ports[0].masterTune;
        CHECK(a == 256 && b == -256 && c == -500, "GS +25.6, XG -25.6, universal -50.0 cents (got %d, %d, %d tenths)", a, b, c);
    }

    printf("[display] SC-55 dot pictures (Star Games.MID)\n");
    {
        MidiFile f;
        std::string err;
        if (f.load(dir + "/Star Games.MID", err)) {
            auto st = std::make_unique<SynthState>();
            st->resetAll(MidiStandard::GS);
            for (const MidiEvent& e : f.events) {
                if (e.status != 0xF0) continue;
                const uint8_t* m = f.payload(e);
                size_t n = f.payloadLen(e);
                st->apply(0, m, n);
                if (st->ports[0].lcdDotsSerial == 1) break;  // first picture
            }
            const PortState& ps = st->ports[0];
            // First frame, row 0: #.......##.#####  row 15: all lit
            CHECK(ps.lcdShownPage == 1 && ps.lcdPage[0][0] == 0x80DF && ps.lcdPage[0][15] == 0xFFFF, "first picture decoded (row 0 %04X, row 15 %04X)",
                  ps.lcdPage[0][0], ps.lcdPage[0][15]);
        } else {
            printf("  skip (%s)\n", err.c_str());
        }
        auto st = std::make_unique<SynthState>();
        st->resetAll(MidiStandard::GS);
        const char* msg = "Hello SC-55";
        Bytes text = {0xF0, 0x41, 0x10, 0x45, 0x12, 0x10, 0x00, 0x00};
        text.insert(text.end(), msg, msg + strlen(msg));
        text.push_back(0);
        text.push_back(0xF7);
        st->apply(0, text.data(), text.size());
        CHECK(std::string(st->ports[0].lcdText) == msg && st->ports[0].lcdTextSerial == 1, "display letters: \"%s\"", st->ports[0].lcdText);
    }

    printf("[formats] MIDI stream (.mds) and Recomposer (.rcp)\n");
    {
        MidiFile mds, src;
        std::string err;
        if (mds.load(dir + "/Ethno_pa.mds", err) && src.load(dir + "/ETHNO_PA.MID", err)) {
            // The MDS was made from the SMF: every note must keep the same distance to its original.
            std::vector<int64_t> a, b;
            for (const MidiEvent& e : mds.events)
                if (e.isChannel() && e.type() == 0x90 && e.d2) a.push_back(e.timeUs);
            for (const MidiEvent& e : src.events)
                if (e.isChannel() && e.type() == 0x90 && e.d2) b.push_back(e.timeUs);
            int64_t worst = 0;
            if (a.size() == b.size() && !a.empty())
                for (size_t k = 0; k < a.size(); k++) worst = std::max<int64_t>(worst, std::llabs((b[k] - a[k]) - (b[0] - a[0])));
            CHECK(a.size() == b.size() && worst < 2000, "Ethno_pa.mds notes follow ETHNO_PA.MID without gaps (%zu/%zu notes, drift %.1f ms)",
                  a.size(), b.size(), worst / 1000.0);
        } else {
            printf("  skip (%s)\n", err.c_str());
        }
        MidiFile rcp;
        if (rcp.load(dir + "/SGT2108P.RCP", err)) {
            CHECK(rcp.noteCount == 5097 && rcp.sysexCount == 79 && rcp.ppqn == 48, "SGT2108P.RCP: %zu notes, %zu SysEx, %d PPQN",
                  size_t(rcp.noteCount), size_t(rcp.sysexCount), rcp.ppqn);
            CHECK(rcp.loopStartTick > 0 && rcp.loopEndTick > rcp.loopStartTick, "endless RCP loop became loop markers (%lld -> %lld)",
                  (long long)rcp.loopStartTick, (long long)rcp.loopEndTick);
        } else {
            printf("  skip (%s)\n", err.c_str());
        }
    }

    printf("[formats] XMIDI (10.xmi) and SMF export with an RPG Maker loop\n");
    {
        MidiFile x;
        std::string err;
        if (x.load(dir + "/10.xmi", err)) {
            int64_t lastNote = 0;
            for (const MidiEvent& e : x.events)
                if (e.isChannel() && (e.type() == 0x80 || e.type() == 0x90)) lastNote = std::max<int64_t>(lastNote, e.timeUs);
            // 8228 XMIDI ticks at 120 Hz = 68.567 s
            CHECK(x.noteCount == 1887 && std::llabs(lastNote - 68566667) < 2000 && x.hasLoop(),
                  "10.xmi: %zu notes, last note-off at %.3f s, loop %s", size_t(x.noteCount), lastNote / 1e6, x.hasLoop() ? "yes" : "no");
        } else {
            printf("  skip (%s)\n", err.c_str());
        }
        std::vector<uint8_t> smf, out;
        MidiFile r;
        if (smfBytesForFile(dir + "/SGT2108P.RCP", smf, err) && r.loadFromMemory(smf.data(), smf.size(), err) && r.hasLoop() &&
            applyRpgMakerLoop(smf, r.loopStartTick, r.loopEndTick, out, err)) {
            MidiFile e;
            e.loadFromMemory(out.data(), out.size(), err);
            int64_t cc111 = -1;
            int open = 0;
            for (const MidiEvent& ev : e.events) {
                if (!ev.isChannel()) continue;
                if (ev.type() == 0xB0 && ev.d1 == 111) cc111 = ev.timeUs;
                if (ev.type() == 0x90 && ev.d2) open++;
                if (ev.type() == 0x80 || (ev.type() == 0x90 && !ev.d2)) open--;
            }
            CHECK(cc111 >= 0 && std::llabs(cc111 - r.loopStartUs) < 1000 && std::llabs(e.lengthUs - r.loopEndUs) < 1000 && open == 0,
                  "RCP export: CC#111 at the loop start (%.3f s), ends at the loop end (%.3f s), no hanging notes (%d)", cc111 / 1e6,
                  e.lengthUs / 1e6, open);
        } else {
            printf("  skip (%s)\n", err.c_str());
        }
    }

    printf("[loops] RPG Maker CC#111 at the start of a track\n");
    {
        // Format 0: CC#111 = 0 (loop start) at tick 0, then a note. Not EMIDI (no CC#110): the
        // track must keep playing.
        const uint8_t trk[] = {0x00, 0xB0, 111, 0, 0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00};
        std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, uint8_t(sizeof trk)};
        b.insert(b.end(), trk, trk + sizeof trk);
        MidiFile f;
        std::string err;
        f.loadFromMemory(b.data(), b.size(), err);
        size_t notes = 0;
        for (const MidiEvent& e : f.events)
            if (e.isChannel() && e.type() == 0x90 && e.d2) notes++;
        CHECK(notes == 1 && f.hasLoop() && f.loopType == std::string("RPG Maker CC#111"), "track kept (%zu note), loop type %s", notes,
              f.loopType.c_str());
    }

    printf("[lyrics] lines and verses\n");
    {
        // Format 0, 480 PPQN, 120 BPM (a beat is 0.5 s): text meta events {delta ticks, type, text}, then a note.
        struct Ev {
            uint32_t delta;
            uint8_t type;
            std::string text;
        };
        auto lyricsOf = [](const std::vector<Ev>& evs, std::vector<int64_t>* ends = nullptr) {
            std::vector<uint8_t> trk;
            auto vlq = [&trk](uint32_t v) {
                uint8_t b[5];
                int n = 0;
                b[n++] = uint8_t(v & 0x7F);
                while (v >>= 7) b[n++] = uint8_t(0x80 | (v & 0x7F));
                while (n) trk.push_back(b[--n]);
            };
            for (const Ev& e : evs) {
                vlq(e.delta);
                trk.push_back(0xFF);
                trk.push_back(e.type);
                vlq(uint32_t(e.text.size()));
                trk.insert(trk.end(), e.text.begin(), e.text.end());
            }
            const uint8_t tail[] = {0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00};
            trk.insert(trk.end(), tail, tail + sizeof tail);
            std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k',
                                      uint8_t(trk.size() >> 24), uint8_t(trk.size() >> 16), uint8_t(trk.size() >> 8), uint8_t(trk.size())};
            b.insert(b.end(), trk.begin(), trk.end());
            MidiFile f;
            std::string err;
            f.loadFromMemory(b.data(), b.size(), err);
            // The lines, separated by "|", and verses by "||".
            std::string out;
            for (size_t i = 0; i < f.lyrics.size(); i++) {
                std::string line;
                for (const LyricSyllable& s : f.lyrics[i].syllables) line += s.text;
                while (!line.empty() && line.back() == ' ') line.pop_back();
                out += (i ? (f.lyrics[i].paragraphStart ? "||" : "|") : "") + line;
                if (ends) ends->push_back(f.lyrics[i].endUs);
            }
            return out;
        };
        std::string got = lyricsOf({{0, 5, "Hel"}, {480, 5, "lo "}, {480, 5, "world\r"}, {480, 5, "How "}, {480, 5, "are "}, {480, 5, "you\r"}});
        CHECK(got == "Hello world|How are you", "syllables, lines ended by CR: %s", got.c_str());
        got = lyricsOf({{0, 1, "@KMIDI KARAOKE FILE"}, {0, 1, "@TSong"}, {480, 1, "\\Twin"}, {240, 1, "kle "}, {240, 1, "twin"},
                        {240, 1, "kle"}, {480, 1, "/Lit"}, {240, 1, "tle "}, {240, 1, "star"}, {960, 1, "\\How "}, {480, 1, "I "},
                        {480, 1, "won"}, {240, 1, "der"}});
        CHECK(got == "Twinkle twinkle|Little star||How I wonder", "KAR text events, '/' lines and '\\' verses: %s", got.c_str());
        got = lyricsOf({{0, 5, "I "}, {480, 5, "will "}, {480, 5, "al"}, {1920, 5, "ways "}, {480, 5, "love "}, {480, 5, "you "},
                        {1920, 5, "Ev"}, {480, 5, "ery "}, {480, 5, "day "}});
        CHECK(got == "I will always love you|Every day", "syllables without marks: lines at the pauses between words: %s", got.c_str());
        got = lyricsOf({{0, 5, "\xE3\x81\x84"}, {240, 5, "\xE3\x81\xA4"}, {240, 5, "\xE3\x81\xBE"}, {240, 5, "\xE3\x81\xA7"},
                        {1920, 5, "\xE3\x81\xBE"}, {240, 5, "\xE3\x81\xA3"}, {240, 5, "\xE3\x81\x9F"}});
        CHECK(got == "\xE3\x81\x84\xE3\x81\xA4\xE3\x81\xBE\xE3\x81\xA7|\xE3\x81\xBE\xE3\x81\xA3\xE3\x81\x9F",
              "Japanese syllables without marks: lines at the pauses (%s)", got.c_str());
        std::vector<int64_t> ends;
        got = lyricsOf({{0, 5, "Hello there my friend "}, {1920, 5, "how are you today "}, {1920, 5, "\xE3\x80\x80"},
                        {15360, 5, "see you again "}},
                       &ends);
        CHECK(got == "Hello there my friend|how are you today||see you again" && ends.size() == 3 && ends[1] == 4000000,
              "a phrase in each lyric event: a line each, a verse after a long pause, a blank event ends a line (%s, ends at %.1f s)",
              got.c_str(), ends.size() == 3 ? ends[1] / 1e6 : -1.0);
        got = lyricsOf({{0, 6, "A"}, {0, 5, "line one goes "}, {3840, 5, "line two goes "}, {3600, 5, "line three goes "}, {240, 6, "B"},
                        {3840, 5, "line four goes "}});
        CHECK(got == "line one goes|line two goes||line three goes|line four goes",
              "phrases with section markers: a verse at each section, its pickup line too: %s", got.c_str());
        MidiFile tr;
        std::string terr;
        if (tr.load(dir + "/traces-tracing you mix-(88pro).mid", terr)) {
            std::string verses;
            for (size_t i = 0; i < tr.lyrics.size(); i++)
                if (tr.lyrics[i].paragraphStart) verses += (verses.empty() ? "" : " ") + std::to_string(i + 1);
            bool ended = tr.lyrics.size() == 19 && tr.lyrics[17].endUs > 0;
            CHECK(tr.lyrics.size() == 19 && verses == "1 6 10 14 18 19" && ended,
                  "traces -tracing you mix- (a phrase in each lyric event, the sections as markers): %zu lines, verses at lines %s, "
                  "the blank lyric ends line 18 (%s)",
                  tr.lyrics.size(), verses.c_str(), ended ? "yes" : "no");
        } else {
            printf("  skip (%s)\n", terr.c_str());
        }
    }

    printf("[analysis] module named in the song text\n");
    {
        // Format 0 SMF: GS Reset, a text event naming the module in full-width characters, one note.
        auto smf = [](const std::string& text) {
            std::vector<uint8_t> trk = {0x00, 0xF0, 0x0A, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7,
                                        0x00, 0xFF, 0x01, uint8_t(text.size())};
            trk.insert(trk.end(), text.begin(), text.end());
            const uint8_t tail[] = {0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00};
            trk.insert(trk.end(), tail, tail + sizeof(tail));
            std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k',
                                      0, 0, uint8_t(trk.size() >> 8), uint8_t(trk.size())};
            b.insert(b.end(), trk.begin(), trk.end());
            return b;
        };
        struct Case { const char* text; DeviceKind want; } cases[] = {
            {"\xEF\xBC\xB3\xEF\xBC\xA3\xEF\xBC\x8D\xEF\xBC\x98\xEF\xBC\x98\xEF\xBD\x90\xEF\xBD\x92\xEF\xBD\x8F\xE5\xAF\xBE\xE5\xBF\x9C",
             DeviceKind::SC88Pro},  // ＳＣ－８８ｐｒｏ対応
            {"for SC-55mkII", DeviceKind::SC55},
            {"(C) 1996 / SC-88", DeviceKind::SC88},
            {"Song title", DeviceKind::SC55},
        };
        for (const Case& c : cases) {
            auto b = smf(c.text);
            MidiFile f;
            std::string err;
            f.loadFromMemory(b.data(), b.size(), err);
            FileInfo fi = analyzeFile(f);
            CHECK(fi.suggestedDevice == c.want, "\"%s\" -> %s (keyword '%s')", c.text, deviceProfile(fi.suggestedDevice).name,
                  fi.moduleKeyword.c_str());
        }
    }

    printf("[emulation] XG drum notes 13-34 on a GS module (TMIDI PatchDrumXGtoGS)\n");
    {
        Emulator emu;
        emu.configure(MidiStandard::XG, deviceProfile(DeviceKind::SC55));
        uint8_t surdo[3] = {0x99, 13, 100}, brush[3] = {0x99, 25, 100}, piano[3] = {0x90, 13, 100};
        bool sent = emu.mapNote(0, surdo);
        CHECK(emu.mapsNotes() && sent && surdo[1] == 86, "Surdo Mute 13 -> 86 (got %d)", surdo[1]);
        CHECK(!emu.mapNote(0, brush), "Brush Tap 25 has no GS counterpart and is dropped");
        CHECK(emu.mapNote(0, piano) && piano[1] == 13, "melodic channel notes are untouched");
    }

    printf("[mirroring] SC-88 shared settings for single-port synths (DEPARTUR.MID, 2 ports)\n");
    {
        auto f = std::make_shared<MidiFile>();
        std::string err;
        if (f->load(dir + "/DEPARTUR.MID", err)) {
            auto count = [](const std::vector<LogEntry>& log, int port, const char* prefix) {
                int n = 0;
                for (const LogEntry& e : log) n += e.port == port && e.text.rfind(prefix, 0) == 0;
                return n;
            };
            auto checksumOk = [](const std::string& text) {  // "F0 41 10 42 12 a1 a2 a3 dd .. ss F7"
                std::vector<unsigned> b;
                for (size_t i = 0; i + 1 < text.size(); i += 3) b.push_back(unsigned(strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
                unsigned sum = 0;
                for (size_t i = 5; i + 2 < b.size(); i++) sum += b[i];
                return b.size() >= 10 && (sum + b[b.size() - 2]) % 128 == 0;
            };
            for (int mirror = 0; mirror < 2; mirror++) {
                Player pl(outs);
                pl.setDevice(DeviceKind::SC88Pro, false);
                pl.setResetDelayMs(0);
                pl.setMirrorEffects(mirror != 0);
                pl.setFile(f);
                pl.seek(f->lengthUs / 2);  // the chase sends the settings made so far
                pl.play();
                wait(800);
                pl.pause();
                pl.snapshot(*snap);
                std::vector<LogEntry> log = pl.sysexLog();
                int efxOnB = count(log, 1, "F0 41 10 42 12 40 03 00 ");      // EFX type (the song sends it on port A)
                int reverbOnB = count(log, 1, "F0 41 10 42 12 40 01 30 ");   // reverb macro
                int otherOnA = count(log, 0, "F0 41 10 42 12 50 ");          // other part group addresses
                int assignOnB = count(log, 1, "F0 41 10 42 12 40 41 22 ");   // 50 41 22 (B01 EFX assign) as port B's own
                bool sums = true;
                for (const LogEntry& e : log)
                    if (e.port == 1 && e.text.rfind("F0 41 10 42 12 40 4", 0) == 0) sums = sums && checksumOk(e.text);
                const PortState &a = snap->ports[0], &b = snap->ports[1];
                bool same = a.gsEfxMsb == b.gsEfxMsb && a.gsEfxLsb == b.gsEfxLsb && !memcmp(a.gsEfxParam, b.gsEfxParam, sizeof a.gsEfxParam) &&
                            !memcmp(a.gsReverb, b.gsReverb, sizeof a.gsReverb) && !memcmp(a.gsChorus, b.gsChorus, sizeof a.gsChorus) &&
                            a.masterVolume == b.masterVolume;
                if (!mirror)
                    CHECK(efxOnB == 0 && reverbOnB == 0 && otherOnA > 0 && !same, "off: effects only on port A, 50 xx xx on port A (%d)", otherOnA);
                else
                    CHECK(efxOnB > 0 && reverbOnB > 0 && otherOnA == 0 && assignOnB > 0 && sums && same,
                          "on: EFX (%d) and reverb (%d) also on port B, port B parts' 50 xx xx as 40 xx xx on port B (%d, checksums %s), "
                          "port B effects equal port A's",
                          efxOnB, reverbOnB, assignOnB, sums ? "ok" : "bad");
            }
            Player pl(outs);
            pl.setDevice(DeviceKind::SC88Pro, false);
            pl.setMirrorEffects(true);
            pl.setFile(f);
            pl.userSend(0, sc88ModeSet(true));  // double module mode: separate effects per part group
            pl.userSend(0, gsSysex(0x400130, {5}));
            pl.userSend(0, gsSysex(0x500130, {6}));
            std::vector<LogEntry> log = pl.sysexLog();
            CHECK(count(log, 1, "F0 41 10 42 12 00 00 7F 01 ") == 1 && count(log, 1, "F0 41 10 42 12 40 01 30 05 ") == 0 &&
                      count(log, 1, "F0 41 10 42 12 40 01 30 06 ") == 1 && count(log, 0, "F0 41 10 42 12 40 01 30 05 ") == 1,
                  "double module mode: the mode set reaches both ports, port A's reverb stays on A, group B's goes to B");
        } else {
            printf("  skip (%s)\n", err.c_str());
        }
    }

    printf("[songinfo] playlist details from a light parse match a full load\n");
    {
        int files = 0, same = 0;
        std::string diff;
        std::error_code ec;
        for (auto& de : std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) {
            std::string p = pathToUtf8(de.path());
            if (!isMidiFileName(p) || endsWithNoCase(p, "bigmidi.mid")) continue;
            MidiFile full;
            std::string err;
            if (!full.load(p, err)) continue;
            SongInfo a = songInfoOf(full), b;
            if (!readSongInfo(p, TextEncoding::Auto, b, err)) continue;
            files++;
            if (a.durationUs == b.durationUs && a.ports == b.ports && a.tracks == b.tracks && a.title == b.title && a.format == b.format &&
                a.fileSize == b.fileSize)
                same++;
            else
                diff += " " + fileNameOf(p);
        }
        CHECK(files > 5 && same == files, "%d of %d files: same length, ports, tracks, title and format%s", same, files, diff.c_str());
        SongInfo big;
        std::string err;
        auto t0 = std::chrono::steady_clock::now();
        if (readSongInfo(dir + "/bigmidi.mid", TextEncoding::Auto, big, err)) {
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            CHECK(big.durationUs > 0, "bigmidi.mid (%.0f MB) read in %.2f s: %s, %d tracks", big.fileSize / 1e6, s, formatTime(big.durationUs / 1e6).c_str(),
                  big.tracks);
        }
    }

    printf("[playlist] sorting by column and details saved with the playlist\n");
    {
        Playlist pl;
        auto entry = [](const char* path, const char* title, int64_t us) {
            PlaylistEntry e;
            e.path = path;
            e.info.title = title;
            e.info.durationUs = us;
            e.infoState = us >= 0 ? InfoState::Known : InfoState::Unknown;
            return e;
        };
        pl.add(entry("/m/Track 10.mid", "", 30000000));
        pl.add(entry("/m/track 2.mid", "", -1));
        pl.add(entry("/m/b.mid", "Zeta", 10000000));
        pl.add(entry("/m/a.mid", "alpha", 20000000));
        pl.setCurrent(1);
        auto names = [&] {
            std::string s;
            for (const PlaylistEntry& e : pl.entries) s += (s.empty() ? "" : ",") + e.shownTitle();
            return s;
        };
        std::vector<int> moved = pl.sortBy(PlaylistSortKey::Title, true);
        std::string byTitle = names();
        bool follow = pl.current == moved[1] && pl.entries[size_t(pl.current)].path == "/m/track 2.mid";
        pl.sortBy(PlaylistSortKey::Duration, false);
        std::string byTimeDesc = names();
        pl.sortBy(PlaylistSortKey::Path, true);
        std::string byPath = names();
        CHECK(byTitle == "alpha,track 2.mid,Track 10.mid,Zeta" && follow, "title, natural order, ignoring case: %s (current follows %d)",
              byTitle.c_str(), follow);
        CHECK(byTimeDesc == "Track 10.mid,alpha,Zeta,track 2.mid", "time, longest first, unknown last: %s", byTimeDesc.c_str());
        CHECK(byPath == "alpha,Zeta,track 2.mid,Track 10.mid", "path: %s", byPath.c_str());
        pl.entries[0].info.copyright = "(C) tab\there";
        std::string saved = pl.serialize();
        std::string file = configDirectory() + "/selftest_playlist.m3u8";
        Playlist back;
        bool ok = writeWholeFile(file, saved) && back.load(file);
        std::error_code rmErr;
        std::filesystem::remove(pathFromUtf8(file), rmErr);
        bool same = ok && back.entries.size() == 4 && back.entries[0].infoState == InfoState::Known && back.entries[0].info.title == "alpha" &&
                    back.entries[0].info.durationUs == 20000000 && back.entries[0].info.copyright == "(C) tab here" &&
                    back.entries[2].infoState == InfoState::Unknown && back.entries[2].name == "track 2.mid";
        CHECK(same, "saved and loaded again: details kept, the unread entry is read again (%zu entries)", back.entries.size());
    }

    printf("[pan] CC#10 0 is the leftmost position; only the part pan SysEx selects random\n");
    {
        auto st = std::make_unique<SynthState>();
        st->resetAll(MidiStandard::GS);
        auto send = [&](const Bytes& b) { st->apply(0, b.data(), b.size()); };
        const ChannelState* ch = st->ports[0].ch;
        send({0xB0, 10, 0});
        send(gsSysex(0x40121C, {0}));  // part 2: random
        send(gsSysex(0x40131C, {0}));  // part 3: random, then CC#10 ends it
        send({0xB2, 10, 20});
        bool gs = !ch[0].randomPan && ch[0].cc[10] == 0 && ch[1].randomPan && ch[1].cc[10] == 64 && !ch[2].randomPan && ch[2].cc[10] == 20;
        st->resetAll(MidiStandard::XG);
        send(xgSysex(0x08, 0x03, 0x0E, {0}));  // XG part 4: random
        send(xgSysex(0x08, 0x04, 0x0E, {1}));  // XG part 5: L63
        bool xg = ch[3].randomPan && !ch[4].randomPan && ch[4].cc[10] == 1;
        send(gsSysex(0x40007F, {0}));  // a reset ends it
        CHECK(gs && xg && !ch[3].randomPan, "CC#10 0 stays a position, GS/XG part pan 0 is random until CC#10 or a reset (gs %d, xg %d)", gs, xg);
    }

    printf("[notes] two tracks on one channel overlapping the same note (SEQ_DU_YURI1.mid)\n");
    {
        // Track 1 holds B4 on channel 2 for 2 s; track 2 plays it again from 0.125 to 0.25 s. Both
        // note-offs must reach the synth: the key sounds until the second one. (A controller at 5 s
        // keeps the song playing, so its end does not silence a stuck note.)
        auto track = [](std::initializer_list<uint8_t> ev) {
            std::vector<uint8_t> t(ev);
            t.insert(t.end(), {0x00, 0xFF, 0x2F, 0x00});
            std::vector<uint8_t> c = {'M', 'T', 'r', 'k', 0, 0, 0, uint8_t(t.size())};
            c.insert(c.end(), t.begin(), t.end());
            return c;
        };
        std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0, 96};
        for (auto& t : {track({0x00, 0x91, 71, 70, 0x83, 0x00, 0x81, 71, 0}), track({0x18, 0x91, 71, 80, 0x18, 0x81, 71, 0, 0x87, 0x10, 0xBF, 7, 100})})
            b.insert(b.end(), t.begin(), t.end());
        auto f = std::make_shared<MidiFile>();
        std::string err;
        if (f->loadFromMemory(b.data(), b.size(), err)) {
            Player pl(outs);
            pl.setDevice(DeviceKind::SC88Pro, false);
            pl.setResetDelayMs(0);
            pl.setFile(f);
            pl.play();
            wait(1000);
            pl.snapshot(*snap);
            const ChannelState& c = snap->ports[0].ch[1];
            bool held = c.noteVel[71] != 0 && c.noteDepth[71] == 1;
            wait(1600);
            pl.snapshot(*snap);
            bool released = snap->ports[0].ch[1].noteVel[71] == 0;
            pl.stop();
            CHECK(held && released, "held after the inner note-off (%d), released by the outer one (%d)", held, released);
        } else {
            printf("  skip (%s)\n", err.c_str());
        }
    }

    printf("[notes] notes held by the sustain and sostenuto pedals\n");
    {
        auto st = std::make_unique<SynthState>();
        st->resetAll(MidiStandard::GS);
        auto send = [&](std::initializer_list<uint8_t> m) {
            Bytes b(m);
            st->apply(0, b.data(), b.size());
        };
        const ChannelState& c = st->ports[0].ch[0];
        send({0x90, 60, 100});
        send({0xB0, 64, 127});  // sustain down
        send({0x80, 60, 0});    // released: the pedal holds it
        send({0x90, 64, 90});
        send({0x80, 64, 0});
        bool held = !c.noteVel[60] && c.noteSus[60] == 100 && c.noteSus[64] == 90 && c.sustainedNotes == 2 && c.activeNotes == 0;
        send({0x90, 60, 80});  // struck again: a key held down, no longer a sustained note
        bool restruck = c.noteVel[60] == 80 && !c.noteSus[60] && c.sustainedNotes == 1;
        send({0xB0, 123, 0});  // all notes off: the pedal still holds them
        bool allOff = !c.noteVel[60] && c.noteSus[60] == 80 && c.sustainedNotes == 2;
        send({0xB0, 64, 0});
        CHECK(held && restruck && allOff && c.sustainedNotes == 0 && !c.noteSus[60] && !c.noteSus[64],
              "sustain: held after note off %d, struck again %d, kept by all notes off %d, released with the pedal", held, restruck, allOff);
        send({0x90, 48, 100});
        send({0xB0, 66, 127});  // sostenuto catches the key down now, not later ones
        send({0x90, 50, 100});
        send({0x80, 48, 0});
        send({0x80, 50, 0});
        bool sos = c.noteSus[48] == 100 && !c.noteSus[50] && c.sustainedNotes == 1;
        send({0xB0, 66, 0});
        CHECK(sos && c.sustainedNotes == 0, "sostenuto holds only the key that was down when pressed (%d), until released", sos);
        send({0xB0, 64, 127});
        send({0x90, 55, 100});
        send({0x80, 55, 0});
        send({0xB0, 121, 0});
        bool reset = c.sustainedNotes == 0 && !c.noteSus[55];
        send({0xB0, 64, 127});
        send({0x90, 57, 100});
        send({0x80, 57, 0});
        send({0xB0, 120, 0});
        CHECK(reset && c.sustainedNotes == 0 && !c.noteSus[57], "reset all controllers (%d) and all sounds off end pedal-held notes", reset);
    }

    printf("[song module] the module a song is made for, set by the user\n");
    {
        // A song without any reset: General MIDI unless its text names a module or the user says.
        auto smf = [](const std::string& text) {
            std::vector<uint8_t> trk = {0x00, 0xFF, 0x01, uint8_t(text.size())};
            trk.insert(trk.end(), text.begin(), text.end());
            const uint8_t tail[] = {0x00, 0xC0, 81, 0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00};
            trk.insert(trk.end(), tail, tail + sizeof(tail));
            std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k',
                                      0, 0, uint8_t(trk.size() >> 8), uint8_t(trk.size())};
            b.insert(b.end(), trk.begin(), trk.end());
            return b;
        };
        auto plain = smf("Song title"), named = smf("for SC-8850");
        auto f = std::make_shared<MidiFile>();
        MidiFile g;
        std::string err;
        f->loadFromMemory(plain.data(), plain.size(), err);
        g.loadFromMemory(named.data(), named.size(), err);
        FileInfo gm = analyzeFile(*f), byText = analyzeFile(g);
        CHECK(gm.standard == MidiStandard::GM && byText.standard == MidiStandard::GS && byText.suggestedDevice == DeviceKind::SC8850,
              "no reset: General MIDI (%s); naming the SC-8850 in its text: %s", standardName(gm.standard), byText.summary().c_str());
        FileInfo chosen = withSongModule(gm, DeviceKind::SC8850);
        CHECK(chosen.standard == MidiStandard::GS && chosen.suggestedDevice == DeviceKind::SC8850 && chosen.detectedStandard == MidiStandard::GM &&
                  withSongModule(chosen, DeviceKind::XG).detectedDevice == DeviceKind::GM,
              "chosen: %s", chosen.summary().c_str());

        SongModules sm, back;
        sm.set("/songs/a.mid", DeviceKind::SC8850);
        sm.set("/songs/b b.mid", DeviceKind::XG);
        sm.set("/songs/c.mid", DeviceKind::SC55);
        sm.clear("/songs/c.mid");
        back.parse(sm.serialize() + "nonsense\tx.mid\nsc88\n");
        DeviceKind a = DeviceKind::GM, b = DeviceKind::GM, c;
        CHECK(back.size() == 2 && back.get("/songs/a.mid", a) && a == DeviceKind::SC8850 && back.get("/songs/b b.mid", b) &&
                  b == DeviceKind::XG && !back.get("/songs/c.mid", c),
              "saved and read again: %zu songs, a %s, b %s", back.size(), deviceShortName(a), deviceShortName(b));

        // The player converts as the chosen module at once.
        auto tables = std::make_shared<ConversionTables>();
        std::string terr;
        if (tables->load("conversion", terr)) {
            Player pl(outs);
            pl.setDevice(DeviceKind::XG, false);
            pl.setEmulation(true);
            pl.setConversionTables(true, "conversion");
            pl.setFile(f, gm);
            std::string before = pl.conversionTableInUse();
            pl.setFileInfo(withSongModule(gm, DeviceKind::SC8850));
            std::string after = pl.conversionTableInUse();
            pl.setDevice(DeviceKind::SC88Pro, false);
            pl.setFileInfo(withSongModule(gm, DeviceKind::XG));
            std::string xg = pl.conversionTableInUse();
            CHECK(before.empty() && after == "gs-to-xg.json" && xg.rfind("xg-to-gs.json", 0) == 0,
                  "General MIDI on XG: none; set to SC-8850: %s; set to XG on an SC-88Pro: %s", after.c_str(), xg.c_str());
        } else {
            printf("  skip (no conversion tables: %s)\n", terr.c_str());
        }
    }

    printf("[json] the conversion tables' file format\n");
    {
        JsonValue v;
        std::string err;
        bool ok = parseJson("\xEF\xBB\xBF{\"a\": [1, -2.5e2, true, null], \"s\": \"x\\\"\\u00e9\\ud83c\\udfb9\\n\", \"o\": {}}", v, err);
        CHECK(ok && v["a"].size() == 4 && v["a"][1].number() == -250 && v["a"][2].boolean() && v["a"][3].isNull() &&
                  v["s"].string() == "x\"\xC3\xA9\xF0\x9F\x8E\xB9\n" && v["o"].isObject() && v["missing"]["deeper"].integer(7) == 7,
              "values, escapes and UTF-16 surrogates (%s)", err.c_str());
        JsonValue bad;
        bool rejected = !parseJson("{\n  \"a\": 1,\n  \"b\": [1 2]\n}", bad, err);
        CHECK(rejected && err.rfind("line 3:", 0) == 0, "errors name the line: %s", err.c_str());
    }

    printf("[names] XG drum note names (Yamaha MU2000.ins uses the note lists of Yamaha XG.ins)\n");
    {
        InsLibrary ins;
        for (const std::string& d : {std::string("insdef"), resourceDirectory() + "/insdef"})
            if (ins.loadDirectory(d) > 0) break;
        const InsInstrument* mu = ins.find("Yamaha MU2000 Drums");
        const InsInstrument* xg = ins.find("Yamaha XG Drums");
        std::string bell = InsLibrary::noteName(mu, 127 * 128, 0, 22), rim = InsLibrary::noteName(mu, 127 * 128, 0, 34);
        std::string sfx1 = InsLibrary::noteName(mu, 126 * 128, 0, 36), sfx2 = InsLibrary::noteName(xg, 126 * 128, 1, 36);
        CHECK(mu && bell == "Metronome Bell" && rim == "Open Rim Shot", "MU2000 Standard Kit: 22 = %s, 34 = %s", bell.c_str(), rim.c_str());
        CHECK(sfx1 == "Cutting Noise" && sfx2 == "Phone Call", "SFX Kit 1 note 36 = %s, SFX Kit 2 note 36 = %s", sfx1.c_str(), sfx2.c_str());
        const NameList* kits = InsLibrary::patchList(mu, 126 * 128);
        std::string list;
        for (auto& [prog, name] : kits ? *kits : NameList()) list += std::to_string(prog + 1) + " " + name + ", ";
        CHECK(kits && kits->size() == 8 && kits->begin()->second == "SFX Kit 1", "MU2000 SFX kits: %s", list.c_str());
        // Each kit has its own sounds: the Symphony Kit's hand cymbals and jazz toms, not the Standard Kit's.
        std::string h49 = InsLibrary::noteName(mu, 127 * 128, 48, 49), h51 = InsLibrary::noteName(mu, 127 * 128, 48, 51);
        std::string t41 = InsLibrary::noteName(xg, 127 * 128, 48, 41), m22 = InsLibrary::noteName(xg, 127 * 128, 48, 22);
        CHECK(h49 == "Hand Cymbal" && h51 == "Hand Cymbal Short" && t41 == "Tom Jazz 1" && m22 == "Metronome Bell",
              "Symphony Kit: 49 = %s, 51 = %s, 41 = %s, 22 = %s", h49.c_str(), h51.c_str(), t41.c_str(), m22.c_str());
        std::string rr5 = InsLibrary::noteName(mu, 127 * 128, 8, 48), sak = InsLibrary::noteName(mu, 126 * 128, 32, 36);
        CHECK(rr5 == "Tom Room 5" && sak == "Dora", "Room Kit 48 = %s (typo fixed); MU2000 Sakura Kit 36 = %s", rr5.c_str(), sak.c_str());
    }

    printf("[names] SC-88Pro map drum sets (as in the SC-88Pro owner's manual)\n");
    {
        InsLibrary ins;
        for (const std::string& d : {std::string("insdef"), resourceDirectory() + "/insdef"})
            if (ins.loadDirectory(d) > 0) break;
        const InsInstrument* gs = ins.find("Roland SC-88 Pro Drumsets");
        auto nn = [&](int map, int kit, int note) { return InsLibrary::noteName(gs, map, kit, note); };
        CHECK(nn(3, 0, 99) == "[88] Standard 2 Snare 1" && nn(3, 0, 39) == "TR-909 Hand Clap" && nn(3, 1, 39) == "Hand Clap",
              "Standard 1: 99 = %s, 39 = %s; Standard 2: 39 = %s", nn(3, 0, 99).c_str(), nn(3, 0, 39).c_str(), nn(3, 1, 39).c_str());
        CHECK(nn(3, 25, 51) == "TR-606 Ride Cymbal" && nn(3, 25, 60) == "CR-78 High Bongo" && nn(3, 25, 99) == "Impact Hit" &&
                  nn(3, 25, 0) == "[88] Electric Kick 2",
              "TR-808: 51 = %s, 60 = %s, 99 = %s, 0 = %s", nn(3, 25, 51).c_str(), nn(3, 25, 60).c_str(), nn(3, 25, 99).c_str(),
              nn(3, 25, 0).c_str());
        CHECK(nn(3, 32, 99).empty() && nn(3, 32, 112) == "[88] Standard 2 Snare 1" && nn(3, 48, 120).empty() && nn(3, 48, 99) == "[55] Timpani D#",
              "Jazz: no sound on 99 (%s), 112 = %s; Orchestra: none on 120 (%s), 99 = %s", nn(3, 32, 99).c_str(), nn(3, 32, 112).c_str(),
              nn(3, 48, 120).c_str(), nn(3, 48, 99).c_str());
    }

    printf("[conversion] instrument conversion tables (conversion/*.json)\n");
    {
        std::string tdir;
        for (const std::string& d : {std::string("conversion"), resourceDirectory() + "/conversion"}) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(pathFromUtf8(d) / ConversionTables::kFiles[0], ec)) {
                tdir = d;
                break;
            }
        }
        auto tables = std::make_shared<ConversionTables>();
        std::string terr;
        bool loaded = !tdir.empty() && tables->load(tdir, terr);
        CHECK(loaded, "tables load from %s %s", tdir.empty() ? "(folder not found)" : tdir.c_str(), terr.c_str());
        InsLibrary ins;
        for (const std::string& d : {std::string("insdef"), resourceDirectory() + "/insdef"})
            if (ins.loadDirectory(d) > 0) break;
        const InsInstrument* gsIns = ins.find("Roland SC-88 Pro");
        const InsInstrument* gsKits = ins.find("Roland SC-88 Pro Drumsets");
        const InsInstrument* sc8850Ins = ins.find("Roland SC-8850");  // the SC-8850 map (map 4)
        const InsInstrument* sc8850Kits = ins.find("Roland SC-8850 (Drum Set)");
        const InsInstrument* xgIns[2] = {ins.find("Yamaha XG"), ins.find("Yamaha MU2000")};
        const InsInstrument* xgKits[2] = {ins.find("Yamaha XG Drums"), ins.find("Yamaha MU2000 Drums")};
        if (loaded && gsIns && gsKits && sc8850Ins && sc8850Kits && xgIns[0] && xgKits[0]) {
            // Every sound the tables name sits where the instrument definitions put it, so a program
            // number off by one (or a wrong bank) cannot slip in unnoticed.
            int checked = 0, wrong = 0;
            std::string firstWrong;
            auto expect = [&](const std::string& what, const std::string& name, std::string found) {
                checked++;
                if (found == name) return;
                if (!wrong++) firstWrong = what + ": \"" + name + "\" but the definitions have \"" + found + "\"";
            };
            auto gsName = [&](const JsonValue& v, bool kit) {
                int pc = v["pc"].integer() - 1, map = v["map"].integer();
                if (map == 4)
                    return kit ? InsLibrary::patchName(sc8850Kits, map, pc) : InsLibrary::patchName(sc8850Ins, v["bank"].integer() * 128 + map, pc);
                return kit ? InsLibrary::patchName(gsKits, map, pc) : InsLibrary::patchName(gsIns, v["bank"].integer() * 128 + map, pc);
            };
            auto xgName = [&](const JsonValue& v, bool kit) {
                int pc = v["pc"].integer() - 1;
                for (int i = 0; i < 2; i++) {
                    std::string n = kit ? InsLibrary::patchName(xgKits[i], v["msb"].integer() * 128, pc)
                                        : InsLibrary::patchName(xgIns[i], v["msb"].integer() * 128 + v["lsb"].integer(), pc);
                    if (n == v["name"].string()) return n;
                }
                return kit ? InsLibrary::patchName(xgKits[0], v["msb"].integer() * 128, pc)
                           : InsLibrary::patchName(xgIns[0], v["msb"].integer() * 128 + v["lsb"].integer(), pc);
            };
            for (const char* file : ConversionTables::kFiles) {
                std::vector<uint8_t> bytes;
                JsonValue doc;
                std::string jerr;
                readWholeFile(pathToUtf8(pathFromUtf8(tdir) / file), bytes);
                if (!parseJson(std::string(bytes.begin(), bytes.end()), doc, jerr)) continue;
                bool xgFrom = std::string(file) == "xg-to-gs.json";
                bool xgTo = std::string(file) == "gs-to-xg.json";
                for (const char* list : {"voices", "drumKits"}) {
                    bool kit = std::string(list) == "drumKits";
                    for (auto& [_, e] : doc[list].items()) {
                        std::string where = std::string(file) + " " + list + " " + e["from"]["name"].string();
                        const JsonValue& from = e["from"];
                        expect(where, from["name"].string(), xgFrom ? xgName(from, kit) : gsName(from, kit));
                        if (xgTo) expect(where + " -> XG", e["to"]["name"].string(), xgName(e["to"], kit));
                        for (const char* tag : {"sc8850", "sc88pro", "sc88", "sc55"})
                            if (e.has(tag)) expect(where + " -> " + tag, e[tag]["name"].string(), gsName(e[tag], kit));
                    }
                }
            }
            CHECK(checked > 6000 && wrong == 0, "%d names match the instrument definitions (%d do not%s%s)", checked, wrong,
                  wrong ? ", first: " : "", firstWrong.c_str());
        } else {
            CHECK(false, "instrument definitions for the name check (insdef folder)");
        }
        if (loaded) {
            // The table editor's document: every table reads and writes back byte for byte, an edit changes its
            // own line only, and the tables load from text as from the folder.
            std::string texts[3];
            int sameText = 0;
            for (int f = 0; f < 3; f++) {
                std::vector<uint8_t> bytes;
                readWholeFile(pathToUtf8(pathFromUtf8(tdir) / ConversionTables::kFiles[f]), bytes);
                texts[f].assign(bytes.begin(), bytes.end());
                ConversionDoc d;
                std::string derr;
                if (d.parse(texts[f], derr) && d.serialize() == texts[f]) sameText++;
            }
            ConversionDoc ed;
            std::string docErr;
            int changedLines = -1;
            if (ed.parse(texts[1], docErr)) {
                JsonValue* voices = ed.root().find("voices");
                if (voices && voices->size() > 0) {
                    JsonValue& t = voices->at(0);
                    if (JsonValue* sc88 = t.find("sc88")) sc88->set("volume", JsonValue::makeNumber(5), {"map", "bank", "pc", "name", "volume", "attack", "note"});
                    std::string a = texts[1], b = ed.serialize();
                    size_t la = std::count(a.begin(), a.end(), '\n'), lb = std::count(b.begin(), b.end(), '\n');
                    changedLines = 0;
                    size_t pa = 0, pb = 0;
                    while (pa < a.size() && pb < b.size()) {
                        size_t ea = a.find('\n', pa), eb = b.find('\n', pb);
                        if (a.compare(pa, ea - pa, b, pb, eb - pb) != 0) changedLines++;
                        pa = ea + 1;
                        pb = eb + 1;
                    }
                    if (la != lb) changedLines = 99;
                }
            }
            ConversionTables fromText;
            std::string ferr;
            const bool textLoads = fromText.loadTexts(texts, ferr);
            CHECK(sameText == 3 && changedLines == 1 && textLoads,
                  "table editor documents: %d of 3 tables write back unchanged, an edit changes %d line, loading from text: %s", sameText,
                  changedLines, textLoads ? "ok" : ferr.c_str());
            auto vs = [](const ConvTarget* t) {
                char b[48];
                snprintf(b, sizeof b, t ? "%d/%d/%d" : "none", t ? t->msb : 0, t ? t->lsb : 0, t ? t->program + 1 : 0);
                return std::string(b);
            };
            {
                // A module plays the sounds of itself and the older modules only: an "sc88pro" sound put on the
                // SC-88 map (as the table editor allows) is not the SC-88's.
                ConversionDoc pd;
                std::string perr;
                std::string ptexts[3] = {texts[0], texts[1], texts[2]};
                if (pd.parse(texts[1], perr) && pd.root().find("voices") && pd.root().find("voices")->size() > 0) {
                    JsonValue t = JsonValue::makeObject();
                    t.set("map", JsonValue::makeNumber(2));
                    t.set("bank", JsonValue::makeNumber(8));
                    t.set("pc", JsonValue::makeNumber(1));
                    t.set("name", JsonValue::makeString("Piano 1w"));
                    pd.root().find("voices")->at(0).set("sc88pro", t);
                    ptexts[1] = pd.serialize();
                }
                ConversionTables pt;
                bool ptLoads = pt.loadTexts(ptexts, perr);
                ConvSetup p88 = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC88);
                ConvSetup pPro = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC88Pro);
                const ConvTarget* on88 = ptLoads ? pt.voice(p88, 0, 0, 0) : nullptr;
                const ConvTarget* onPro = ptLoads ? pt.voice(pPro, 0, 0, 0) : nullptr;
                CHECK(on88 && on88->msb == 0 && on88->lsb == 2 && onPro && onPro->msb == 8 && onPro->lsb == 2,
                      "an \"sc88pro\" sound on the SC-88 map plays on the SC-88Pro (%s), the SC-88 plays its own (%s)", vs(onPro).c_str(),
                      vs(on88).c_str());
                // Drum sets of the SC-8850 map (added with the table editor) convert as their entries say, as voices do.
                auto obj = [](std::initializer_list<std::pair<const char*, JsonValue>> kv) {
                    JsonValue o = JsonValue::makeObject();
                    for (auto& [k, v] : kv) o.set(k, v);
                    return o;
                };
                auto n = [](int v) { return JsonValue::makeNumber(v); };
                ConversionDoc kx, km;
                if (kx.parse(texts[0], perr) && km.parse(texts[2], perr) && kx.root().find("drumKits") && km.root().find("drumKits")) {
                    kx.root().find("drumKits")->append(obj({{"from", obj({{"map", n(4)}, {"pc", n(1)}})}, {"to", obj({{"msb", n(127)}, {"pc", n(26)}})}}));
                    km.root().find("drumKits")->append(obj({{"from", obj({{"map", n(4)}, {"pc", n(1)}})}, {"sc88", obj({{"map", n(2)}, {"pc", n(26)}})}}));
                    ptexts[0] = kx.serialize();
                    ptexts[2] = km.serialize();
                }
                ConversionTables kt;
                bool ktLoads = kt.loadTexts(ptexts, perr);
                ConvSetup k8850xg = ConversionTables::setupFor(MidiStandard::GS, DeviceKind::SC8850, DeviceKind::XG);
                ConvSetup kProxg = ConversionTables::setupFor(MidiStandard::GS, DeviceKind::SC88Pro, DeviceKind::XG);
                ConvSetup k8850to88 = ConversionTables::setupFor(MidiStandard::GS, DeviceKind::SC8850, DeviceKind::SC88);
                const ConvTarget* kx4 = ktLoads ? kt.drumKit(k8850xg, 0, 0, 0) : nullptr;
                const ConvTarget* kx3 = ktLoads ? kt.drumKit(kProxg, 0, 0, 0) : nullptr;
                const ConvTarget* km4 = ktLoads ? kt.drumKit(k8850to88, 0, 0, 0) : nullptr;
                CHECK(kx4 && kx4->msb == 127 && kx4->program == 25 && kx3 && kx3->program == 0 && km4 && km4->lsb == 2 && km4->program == 25,
                      "SC-8850 map drum set entries: on XG %s (the SC-88Pro map's %s), on the SC-88 %s", vs(kx4).c_str(), vs(kx3).c_str(),
                      vs(km4).c_str());
            }
            ConvSetup gx = ConversionTables::setupFor(MidiStandard::GS, DeviceKind::SC55, DeviceKind::XG);
            const ConvTarget* solo = tables->voice(gx, 8, 0, 81);
            const ConvTarget* solo1 = tables->voice(gx, 8, 1, 81);
            CHECK(solo && solo->msb == 0 && solo->lsb == 41 && solo->program == 81 && solo1 == solo,
                  "SC-55 Doctor Solo (CC#0 8, PC 82) on XG: Dr. Lead 0/41/82 (got %s)", vs(solo).c_str());
            const ConvTarget* pulse = tables->voice(gx, 2, 0, 81);
            const ConvTarget* pulse1 = tables->voice(gx, 2, 1, 81);
            CHECK(pulse && pulse->lsb == 40 && pulse1 && pulse1->lsb == 0,
                  "an SC-88 sound in an SC-55 song converts as on the SC-88 map (%s), not with an explicit SC-55 map (%s)",
                  vs(pulse).c_str(), vs(pulse1).c_str());
            const ConvTarget* kit = tables->drumKit(gx, 0, 0, 0);
            CHECK(kit && kit->msb == 127 && kit->program == 0 && kit->notes && kit->notes->note[34] == 22 && kit->notes->note[27] == 15,
                  "GS Standard set on XG: Standard Kit, Metronome Bell 34 -> 22, High Q 27 -> 15");
            ConvSetup x55 = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC55);
            ConvSetup x88 = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC88);
            ConvSetup xPro = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC88Pro);
            const ConvTarget* lead55 = tables->voice(x55, 0, 41, 81);
            const ConvTarget* lead88 = tables->voice(x88, 0, 41, 81);
            const ConvTarget* leadPro = tables->voice(xPro, 0, 41, 81);
            const ConvTarget* str = tables->voice(x55, 0, 0, 50);
            CHECK(lead55 && lead55->msb == 8 && lead55->lsb == 1 && lead88 && lead88->msb == 8 && lead88->lsb == 2 && leadPro &&
                      leadPro->msb == 8 && leadPro->lsb == 3 && str && str->program == 50,
                  "XG Dr. Lead: Doctor Solo on the SC-55 (%s), the SC-88 map (%s) and the SC-88Pro map (%s); Syn Str1 stays SynStrings1 (%s)",
                  vs(lead55).c_str(), vs(lead88).c_str(), vs(leadPro).c_str(), vs(str).c_str());

            // XG voices on the SC-8850: its own map's sounds, and the SC-88Pro map's with that map forced.
            ConvSetup x8850 = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC8850);
            ConvSetup x8850on3 = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC8850, 3);
            const ConvTarget* hammer = tables->voice(x8850, 0, 96, 38);
            const ConvTarget* hammerPro = tables->voice(xPro, 0, 96, 38);
            const ConvTarget* hammer3 = tables->voice(x8850on3, 0, 96, 38);
            const ConvTarget* sqr = tables->voice(x8850, 0, 65, 38);
            CHECK(hammer && hammer->msb == 20 && hammer->lsb == 4 && hammer->program == 38 && hammerPro && hammerPro->msb == 16 &&
                      hammerPro->lsb == 3 && hammer3 && hammer3->msb == 16 && hammer3->lsb == 3 && sqr && sqr->msb == 17 && sqr->lsb == 4,
                  "XG Hammer: the SC-8850's Hammer (%s), Reso SH Bass on the SC-88Pro (%s) and with the SC-88Pro map forced (%s); "
                  "Sqr.Bass: TB303 Sqr Bs (%s)",
                  vs(hammer).c_str(), vs(hammerPro).c_str(), vs(hammer3).c_str(), vs(sqr).c_str());
            const ConvTarget* grand = tables->voice(x8850, 0, 0, 0);
            const ConvTarget* bright = tables->voice(x8850, 0, 0, 1);
            const ConvTarget* wire = tables->voice(x8850, 0, 64, 39);
            const ConvTarget* violin = tables->voice(x8850, 0, 0, 40);
            CHECK(grand && grand->msb == 0 && grand->lsb == 4 && bright && bright->msb == 8 && bright->lsb == 3 && wire && wire->msb == 10 &&
                      wire->lsb == 4 && violin && violin->msb == 0 && violin->lsb == 4 &&
                      x8850.describe() == "xg-to-gs.json (SC-8850 map)" && x8850on3.describe() == "xg-to-gs.json (SC-88Pro map)",
                  "SC-8850 map: Piano 1 (%s); Piano 2w stays on the SC-88Pro map (%s), X Wire Bass moves to variation 10 (%s), "
                  "Violin (%s); %s, forced: %s",
                  vs(grand).c_str(), vs(bright).c_str(), vs(wire).c_str(), vs(violin).c_str(), x8850.describe().c_str(),
                  x8850on3.describe().c_str());
            // SMF Knife's SC-55 map choices (mostly the GM sounds) play the module's own version on the SC-88 and later;
            // the SC-55 keeps them. The SC-88Pro plays the SC-88Pro map sounds the SC-8850 shares.
            const ConvTarget* vio55 = tables->voice(x55, 0, 0, 40), *vio88 = tables->voice(x88, 0, 0, 40), *vioPro = tables->voice(xPro, 0, 0, 40);
            const ConvTarget* sqrPro = tables->voice(xPro, 0, 65, 38), *pulsePro = tables->voice(xPro, 0, 67, 80);
            const ConvTarget* wirePro = tables->voice(xPro, 0, 64, 39);
            CHECK(vio55 && vio55->lsb == 1 && vio88 && vio88->lsb == 2 && vioPro && vioPro->lsb == 3 && sqrPro && sqrPro->msb == 17 &&
                      sqrPro->lsb == 3 && pulsePro && pulsePro->msb == 24 && pulsePro->lsb == 3 && wirePro && wirePro->msb == 10 && wirePro->lsb == 3,
                  "XG Violin on each module's own map (SC-55 %s, SC-88 %s, SC-88Pro %s); SC-88Pro: Sqr.Bass -> 303 Sqr Bs (%s), "
                  "Pulse Ld -> Pulse Lead (%s), X Wire Bass on variation 10 (%s)",
                  vs(vio55).c_str(), vs(vio88).c_str(), vs(vioPro).c_str(), vs(sqrPro).c_str(), vs(pulsePro).c_str(), vs(wirePro).c_str());
            // XG's Sine Lead is GS's Sine Wave (XG's leads are GS's waves), which the SC-88Pro and SC-8850 maps call 2600 Sine.
            const ConvTarget* sine88 = tables->voice(x88, 0, 66, 80), *sinePro = tables->voice(xPro, 0, 66, 80);
            const ConvTarget* sine8850 = tables->voice(x8850, 0, 66, 80);
            CHECK(sine88 && sine88->msb == 8 && sine88->lsb == 2 && sinePro && sinePro->msb == 8 && sinePro->lsb == 3 && sine8850 &&
                      sine8850->msb == 8 && sine8850->lsb == 4,
                  "XG Sine Lead: Sine Wave on the SC-88 (%s), 2600 Sine on the SC-88Pro (%s) and SC-8850 (%s)", vs(sine88).c_str(),
                  vs(sinePro).c_str(), vs(sine8850).c_str());
            ConvSetup p88 = ConversionTables::setupFor(MidiStandard::GS, DeviceKind::SC88Pro, DeviceKind::SC88);
            const ConvTarget* piano88 = tables->voice(p88, 0, 3, 0);
            CHECK(piano88 && piano88->msb == 0 && piano88->lsb == 2 && piano88->program == 0,
                  "an SC-88Pro song's Piano 1 on the SC-88: the SC-88's own Piano 1 (%s)", vs(piano88).c_str());

            // XG drum kits on GS modules: the SC-88Pro (and SC-8850) use the SC-88Pro map sets.
            auto kitOn = [&](const ConvSetup& st, int msb, int prog) { return tables->drumKit(st, msb, 0, prog); };
            auto to = [](const ConvTarget* t, int note) { return t && t->notes ? int(t->notes->note[note]) : -9; };
            const ConvTarget* std55 = kitOn(x55, 127, 0), *std88 = kitOn(x88, 127, 0), *stdPro = kitOn(xPro, 127, 0);
            CHECK(std55 && std55->lsb == 1 && std88 && std88->lsb == 2 && stdPro && stdPro->lsb == 3 && stdPro->program == 0,
                  "Standard Kit: SC-55 map (%s), SC-88 map (%s), SC-88Pro map (%s)", vs(std55).c_str(), vs(std88).c_str(), vs(stdPro).c_str());
            CHECK(to(std55, 34) == 40 && to(std88, 34) == 40 && to(stdPro, 34) == 99,
                  "Open Rim Shot (34): the tight snare E2 on the SC-55 (%d) and SC-88 maps (%d), D#7 on the SC-88Pro map (%d)",
                  to(std55, 34), to(std88, 34), to(stdPro, 34));
            CHECK(to(stdPro, 100) == -2 && to(stdPro, 5) == -2 && to(stdPro, 84) == -1 && to(stdPro, 86) == -2,
                  "notes the XG kit has no sound for stay silent on the SC-88Pro map (100: %d, 5: %d; 84 Bell Tree: %d, 86: %d)",
                  to(stdPro, 100), to(stdPro, 5), to(stdPro, 84), to(stdPro, 86));
            const ConvTarget* jazzPro = kitOn(xPro, 127, 32);
            CHECK(jazzPro && jazzPro->lsb == 3 && to(jazzPro, 34) == 112,
                  "Jazz Kit on the SC-88Pro map: Open Rim Shot -> 112 (%d; the Jazz set has no sound on D#7)", to(jazzPro, 34));
            for (const ConvSetup* st : {&x55, &x88, &xPro}) {
                const ConvTarget* sym = kitOn(*st, 127, 48);
                CHECK(sym && sym->program == 48 && to(sym, 49) == 59 && to(sym, 51) == 59 && to(sym, 57) == 57 && to(sym, 59) == 57 &&
                          to(sym, 42) == 27 && to(sym, 44) == 28 && to(sym, 46) == 29,
                      "Symphony Kit -> Orchestra (map %d): hand cymbals 49/51 -> 59 (%d %d), 57/59 -> 57 (%d %d); hi-hats -> 27 28 29 (%d %d %d)",
                      sym ? sym->lsb : 0, to(sym, 49), to(sym, 51), to(sym, 57), to(sym, 59), to(sym, 42), to(sym, 44), to(sym, 46));
                // Matched by ear (S-YXG50 against Sound Canvas VA): the A1 kicks of the Symphony and Electro kits play B1.
                const ConvTarget* elec = kitOn(*st, 127, 24);
                CHECK(to(sym, 33) == 35 && to(sym, 36) == 36 && elec && elec->program == 24 && to(elec, 33) == 35,
                      "by ear (map %d): Symphony Kit A1 -> %d (C2 stays: %d), Electro Kit A1 -> %d", sym ? sym->lsb : 0, to(sym, 33),
                      to(sym, 36), to(elec, 33));
            }
            const ConvTarget* sub55 = tables->voice(x55, 64, 0, 90), *subPro = tables->voice(xPro, 64, 0, 90);
            CHECK(sub55 && sub55->msb == 4 && sub55->lsb == 1 && sub55->program == 122 && subPro && subPro->msb == 4 && subPro->lsb == 3 &&
                      subPro->program == 122,
                  "XG SFX voice Submarine (64/0/91): Stream by ear, on the SC-55 (%s) and the SC-88Pro map (%s)", vs(sub55).c_str(),
                  vs(subPro).c_str());
            const ConvTarget* sfx2 = kitOn(xPro, 126, 1);
            CHECK(sfx2 && sfx2->program == 56 && to(sfx2, 56) == 67 && to(sfx2, 36) == 89 && to(sfx2, 60) == -2,
                  "SFX Kit 2 -> SFX set: Siren 56 -> 67 (%d), Phone Call 36 -> 89 (%d), Burst 60 not played (%d)", to(sfx2, 56),
                  to(sfx2, 36), to(sfx2, 60));
            ConvSetup forced = ConversionTables::setupFor(MidiStandard::XG, DeviceKind::XG, DeviceKind::SC88Pro, 2);
            const ConvTarget* stdForced = kitOn(forced, 127, 0);
            CHECK(stdForced && stdForced->lsb == 2 && to(stdForced, 34) == 40,
                  "SC-88Pro with the SC-88 map forced: SC-88 map sets (%s, 34 -> %d)", vs(stdForced).c_str(), to(stdForced, 34));
            // The Symphony Kit's Gran Cassa (B1) plays the Orchestra set's Concert BD (C2), on every map; MU Basic (PC 128)
            // converts as the Standard Kit; the SC-88 plays its own sets where SMF Knife chose SC-55 map ones.
            const ConvTarget* sym55 = kitOn(x55, 127, 48), *symPro = kitOn(xPro, 127, 48), *muBasic = kitOn(xPro, 127, 127);
            const ConvTarget* jazz88 = kitOn(x88, 127, 32);
            CHECK(to(sym55, 35) == 36 && to(symPro, 35) == 36 && muBasic && muBasic->lsb == 3 && muBasic->program == 0 &&
                      to(muBasic, 34) == 99 && jazz88 && jazz88->lsb == 2 && jazz88->program == 32,
                  "Symphony Kit B1 -> C2 (SC-55 map %d, SC-88Pro map %d); MU Basic -> Standard 1 (%s, 34 -> %d); Jazz Kit on the SC-88: %s",
                  to(sym55, 35), to(symPro, 35), vs(muBasic).c_str(), to(muBasic, 34), vs(jazz88).c_str());
            // The SC-8850: its own map's sets (the SC-88Pro's set numbers) with note maps from its drum set list.
            const ConvTarget* std8850 = kitOn(x8850, 127, 0), *jazz8850 = kitOn(x8850, 127, 32), *sym8850 = kitOn(x8850, 127, 48);
            const ConvTarget* techno8850 = kitOn(x8850, 127, 26), *std8850on3 = kitOn(x8850on3, 127, 0);
            CHECK(std8850 && std8850->lsb == 4 && std8850->program == 0 && to(std8850, 34) == 99 && jazz8850 && jazz8850->lsb == 4 &&
                      to(jazz8850, 34) == 112 && sym8850 && sym8850->lsb == 4 && sym8850->program == 48 && to(sym8850, 49) == 59 &&
                      to(sym8850, 33) == 35 && techno8850 && techno8850->lsb == 4 && techno8850->program == 11 &&
                      to(techno8850, 37) == -1 && std8850on3 && std8850on3->lsb == 3,
                  "SC-8850 map sets: Standard 1 (%s, Open Rim Shot -> %d), Jazz (34 -> %d), Orchestra (49 -> %d, A1 -> %d), Techno "
                  "(%s, its snare rim plays the side stick: %d); with the SC-88Pro map forced: %s",
                  vs(std8850).c_str(), to(std8850, 34), to(jazz8850, 34), to(sym8850, 49), to(sym8850, 33), vs(techno8850).c_str(),
                  to(techno8850, 37), vs(std8850on3).c_str());
            // GS Orchestra on XG: the hi-hats, concert cymbals and timpani of the Symphony Kit.
            const ConvTarget* orch = tables->drumKit(gx, 0, 2, 48);
            CHECK(orch && orch->msb == 127 && orch->program == 48 && to(orch, 27) == 42 && to(orch, 28) == 44 && to(orch, 29) == 46 &&
                      to(orch, 59) == 49 && to(orch, 57) == 57 && to(orch, 42) == 41 && to(orch, 53) == 50,
                  "Orchestra -> Symphony Kit: hi-hats 27 28 29 -> %d %d %d, cymbals 59 -> %d, 57 -> %d, timpani F# 42 -> %d, f 53 -> %d",
                  to(orch, 27), to(orch, 28), to(orch, 29), to(orch, 59), to(orch, 57), to(orch, 42), to(orch, 53));
            ConvSetup p55 = ConversionTables::setupFor(MidiStandard::GS, DeviceKind::SC88Pro, DeviceKind::SC55);
            const ConvTarget* glock = tables->voice(p55, 4, 0, 98);
            CHECK(glock && glock->msb == 0 && glock->program == 9, "SC-88Pro Loud Glock on an SC-55: Glockenspiel (%s)", vs(glock).c_str());

            Emulator emu;
            emu.configure(MidiStandard::GS, deviceProfile(DeviceKind::XG));
            emu.setConversion(tables, gx);
            std::vector<Bytes> out;
            const uint8_t bank[3] = {0xB0, 0, 8}, prog[2] = {0xC0, 81}, drumPc[2] = {0xC9, 0};
            emu.convert(0, bank, 3, out);
            emu.convert(0, prog, 2, out);
            emu.convert(0, drumPc, 2, out);
            bool lsb41 = false;
            for (const Bytes& b : out)
                if (b == Bytes{0xB0, 32, 41}) lsb41 = true;
            uint8_t bell[3] = {0x99, 34, 100};
            bool sent = emu.mapNote(0, bell);
            CHECK(lsb41 && sent && bell[1] == 22, "emulation: Doctor Solo sends CC#32 41 (%d), drum note 34 plays 22 (%d)", lsb41, bell[1]);

            // An XG song on an SC-88Pro: the drum part selects the SC-88Pro map set.
            Emulator xe;
            xe.configure(MidiStandard::XG, deviceProfile(DeviceKind::SC88Pro));
            xe.setConversion(tables, xPro);
            std::vector<Bytes> xo;
            const uint8_t xmsb[3] = {0xB9, 0, 127}, xpc[2] = {0xC9, 0};
            xe.convert(0, xmsb, 3, xo);
            xe.convert(0, xpc, 2, xo);
            bool map3 = false, pc0 = false;
            for (const Bytes& b : xo) {
                if (b == Bytes{0xB9, 32, 3}) map3 = true;
                if (b == Bytes{0xC9, 0}) pc0 = true;
            }
            uint8_t rim[3] = {0x99, 34, 100};
            bool rimSent = xe.mapNote(0, rim);
            CHECK(map3 && pc0 && rimSent && rim[1] == 99,
                  "emulation: an XG Standard Kit on the SC-88Pro sends CC#32 3 (%d), PC 1 (%d); Open Rim Shot plays 99 (%d)", map3, pc0, rim[1]);

            // Several drum parts on a GS module (two drum maps): channel 10 keeps map 1, another set takes
            // map 2 even when the song's own GS message names map 1 (RS3_4mabat2XG.mid), the same set
            // shares map 1, and a part sharing it moves to map 2 when channel 10 selects another set.
            {
                Emulator de;
                de.configure(MidiStandard::XG, deviceProfile(DeviceKind::SC88Pro));
                de.setConversion(tables, xPro);
                auto feed = [&](std::initializer_list<uint8_t> m) {
                    std::vector<Bytes> o;
                    std::vector<uint8_t> v(m);
                    de.convert(0, v.data(), v.size(), o);
                    return o;
                };
                auto has = [](const std::vector<Bytes>& v, const Bytes& b) { return std::find(v.begin(), v.end(), b) != v.end(); };
                const Bytes map2 = gsSysex(0x401A15, {2}), map1 = gsSysex(0x401A15, {1});
                std::vector<Bytes> songMap1 = feed({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1A, 0x15, 0x01, 0x10, 0xF7});
                feed({0xB9, 0, 127});
                feed({0xC9, 24});  // Electro Kit
                feed({0xBA, 0, 127});
                std::vector<Bytes> other = feed({0xCA, 0});  // Standard Kit
                const bool secondOnMap2 = has(songMap1, map2) && !has(songMap1, map1) && !has(other, map1);
                std::vector<Bytes> same = feed({0xCA, 24});  // channel 10's set
                const bool shares = has(same, map1);
                std::vector<Bytes> moved = feed({0xC9, 0});  // channel 10 selects another set
                const bool movesBack = has(moved, map2) && has(moved, Bytes({0xCA, 24}));
                CHECK(secondOnMap2 && shares && movesBack,
                      "GS drum maps: a second set on map 2 (%d), channel 10's set shares map 1 (%d), and moves to map 2 when channel 10 "
                      "changes (%d)",
                      secondOnMap2, shares, movesBack);
            }
            // A table's attack correction: XG's door squeak plays the Sound Canvas's Door Creaking with the attack
            // 10 shorter (it fades in there); the song's own attack keeps the correction, the next sound gets it back.
            {
                Emulator ae;
                ae.configure(MidiStandard::XG, deviceProfile(DeviceKind::SC88Pro));
                ae.setConversion(tables, xPro);
                auto feed = [&](std::initializer_list<uint8_t> m) {
                    std::vector<Bytes> o;
                    std::vector<uint8_t> v(m);
                    ae.convert(0, v.data(), v.size(), o);
                    return o;
                };
                auto has = [](const std::vector<Bytes>& v, const Bytes& b) { return std::find(v.begin(), v.end(), b) != v.end(); };
                feed({0xB0, 0, 64});
                std::vector<Bytes> door = feed({0xC0, 65});  // SFX DoorSqek (64/0/66)
                feed({0xB0, 99, 1});
                feed({0xB0, 98, 0x63});
                std::vector<Bytes> songAttack = feed({0xB0, 6, 80});
                feed({0xB0, 0, 0});
                std::vector<Bytes> piano = feed({0xC0, 0});
                // (The XG to GS rules scale attack times around 64 by half: the song's 80 is 72 on the Sound Canvas.)
                const bool doorAttack = has(door, Bytes({0xC0, 124})) && has(door, Bytes({0xB0, 98, 0x63})) && has(door, Bytes({0xB0, 6, 54}));
                const bool songKeeps = has(songAttack, Bytes({0xB0, 6, 62}));
                const bool restored = has(piano, Bytes({0xB0, 6, 72}));
                CHECK(doorAttack && songKeeps && restored,
                      "attack correction: Door Creaking with attack 54 (%d), the song's 80 (72 scaled) sent as 62 (%d), 72 again for the "
                      "next sound (%d)",
                      doorAttack, songKeeps, restored);
            }

            // Export with emulation: an XG song written as the SC-88Pro receives it, tracks and timing kept.
            auto track = [](std::initializer_list<std::pair<uint32_t, Bytes>> evs) {
                Bytes body;
                for (const auto& [delta, b] : evs) {
                    for (int sh = 21; sh > 0; sh -= 7)
                        if (delta >> sh) body.push_back(uint8_t(0x80 | ((delta >> sh) & 0x7F)));
                    body.push_back(uint8_t(delta & 0x7F));
                    body.insert(body.end(), b.begin(), b.end());
                }
                body.insert(body.end(), {0, 0xFF, 0x2F, 0});
                uint32_t n = uint32_t(body.size());
                Bytes t = {'M', 'T', 'r', 'k', uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
                t.insert(t.end(), body.begin(), body.end());
                return t;
            };
            Bytes song = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x01, 0xE0};
            Bytes t0 = track({{0, {0xFF, 0x51, 3, 0x07, 0xA1, 0x20}}, {0, {0xFF, 0x01, 4, 't', 'e', 's', 't'}}});
            Bytes t1 = track({{0, {0xF0, 8, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7}},
                              {10, {0xB9, 0, 127}}, {0, {0xC9, 0}}, {10, {0x99, 34, 100}}, {20, {0x89, 34, 64}}, {10, {0xB9, 7, 100}}});
            song.insert(song.end(), t0.begin(), t0.end());
            song.insert(song.end(), t1.begin(), t1.end());
            Emulator ee;
            ee.configure(MidiStandard::XG, deviceProfile(DeviceKind::SC88Pro));
            ee.setConversion(tables, xPro);
            Bytes exported;
            std::string eerr;
            bool exportedOk = applyEmulation(song, ee, exported, eerr);
            int fmt = 0, ntracks = 0;
            uint16_t div = 0;
            std::vector<uint64_t> ends;
            bool tempo = false, text = false, reset = false, xgOn = false, kitMap3 = false, rimOn = false, rimOff = false, vol = false;
            if (exportedOk)
                MidiFile::forEachSmfEvent(
                    exported.data(), exported.size(),
                    [&](const SmfEventRef& e) {
                        ntracks = std::max(ntracks, e.track + 1);
                        if (e.kind == SmfEventRef::Meta) {
                            tempo |= e.track == 0 && e.d1 == 0x51;
                            text |= e.track == 0 && e.d1 == 0x01;
                            return;
                        }
                        if (e.track != 1) return;
                        if (e.kind == SmfEventRef::Sysex) {
                            Bytes b(e.data, e.data + e.len);
                            reset |= e.tick == 0 && b == gsReset();
                            xgOn |= b == xgSystemOn();
                            return;
                        }
                        kitMap3 |= e.tick == 10 && e.status == 0xB9 && e.d1 == 32 && e.d2 == 3;
                        rimOn |= e.tick == 20 && e.status == 0x99 && e.d1 == 99;
                        rimOff |= e.tick == 40 && e.status == 0x89 && e.d1 == 99;
                        vol |= e.tick == 50 && e.status == 0xB9 && e.d1 == 7;
                    },
                    fmt, div, ends, eerr);
            CHECK(exportedOk && fmt == 1 && ntracks == 2 && div == 480 && tempo && text && ends.size() == 2 && ends[1] == 50,
                  "export with emulation keeps the format (%d), tracks (%d), resolution (%d), tempo and text (%d %d) and the "
                  "track ends (%llu) %s",
                  fmt, ntracks, div, tempo, text, ends.size() == 2 ? (unsigned long long)ends[1] : 0ULL, eerr.c_str());
            CHECK(reset && !xgOn && kitMap3 && rimOn && rimOff && vol,
                  "export with emulation for the SC-88Pro: XG System On -> GS reset (%d, XG left %d), drum kit on map 3 (%d), "
                  "Open Rim Shot on and off at 99 (%d %d), volume kept in place (%d)",
                  reset, xgOn, kitMap3, rimOn, rimOff, vol);

            // ImMidi Bridge: the same conversion in real time, message by message.
            MidiBridge bridge;
            std::string berr;
            bool bTables = bridge.loadTables(tdir, berr);
            std::vector<Bytes> got;
            bridge.setSink([&](const uint8_t* m, size_t n) { got.emplace_back(m, m + n); });
            MidiBridge::Settings bs;
            bs.source = DeviceKind::XG;
            bs.target = DeviceKind::SC88Pro;
            bridge.configure(bs);
            auto feed = [&](Bytes m) {
                got.clear();
                bridge.process(m.data(), m.size());
                return got;
            };
            auto has = [](const std::vector<Bytes>& v, const Bytes& b) { return std::find(v.begin(), v.end(), b) != v.end(); };
            bool bReset = has(feed(xgSystemOn()), gsReset());
            feed({0xB9, 0, 127});
            std::vector<Bytes> kitMsgs = feed({0xC9, 0});
            bool bKit = has(kitMsgs, Bytes{0xB9, 32, 3}) && has(kitMsgs, Bytes{0xC9, 0});
            std::vector<Bytes> rimMsgs = feed({0x99, 34, 100});
            bool bRim = rimMsgs.size() == 1 && rimMsgs[0][1] == 99;
            CHECK(bTables && bReset && bKit && bRim && bridge.conversion() == "xg-to-gs.json (SC-88Pro map)",
                  "bridge XG -> SC-88Pro: XG System On -> GS reset (%d), drum kit on map 3 (%d), Open Rim Shot -> 99 (%d), %s %s", bReset,
                  bKit, bRim, bridge.conversion().c_str(), berr.c_str());
            // A reset in the stream switches the source: XG, then back to the GS module last chosen.
            bs.source = DeviceKind::SC8850;
            bridge.configure(bs);
            bs.source = DeviceKind::GM;
            bridge.configure(bs);
            feed(xgSystemOn());
            DeviceKind afterXg = bridge.source();
            feed(gsReset());
            DeviceKind afterGs = bridge.source();
            CHECK(afterXg == DeviceKind::XG && afterGs == DeviceKind::SC8850, "bridge follows resets: XG On -> %s, GS reset -> %s",
                  deviceShortName(afterXg), deviceShortName(afterGs));
            // Another module keeps the source a reset switched to; choosing the source replaces it.
            feed(xgSystemOn());
            bs.target = DeviceKind::SC8850;
            bridge.configure(bs, true);
            DeviceKind kept = bridge.source();
            bridge.configure(bs);
            DeviceKind chosen = bridge.source();
            CHECK(kept == DeviceKind::XG && chosen == DeviceKind::GM, "bridge settings: the source a reset chose stays (%s), the chosen one replaces it (%s)",
                  deviceShortName(kept), deviceShortName(chosen));
            bs.source = DeviceKind::GM;
            bs.target = DeviceKind::GM;
            bridge.configure(bs);
            std::vector<Bytes> same = feed({0x90, 60, 100});
            std::vector<Bytes> clock = feed({0xF8});
            const bool sameOk = same.size() == 1 && same[0] == Bytes({0x90, 60, 100});
            const bool clockOk = clock.size() == 1 && clock[0] == Bytes({0xF8});
            CHECK(sameOk && clockOk && bridge.conversion().empty(),
                  "bridge GM -> GM: notes and clock pass unchanged (%zu, %zu)", same.size(), clock.size());
        }
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: immidi_cli info|lyrics|ports|play|emulate|ins|convert|selftest ...\n");
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "info") return cmdInfo(argc, argv);
    if (cmd == "lyrics") return cmdLyrics(argc, argv);
    if (cmd == "convert") return cmdConvert(argc, argv);
    if (cmd == "ports") return cmdPorts();
    if (cmd == "play") return cmdPlay(argc, argv);
    if (cmd == "emulate") return cmdEmulate(argc, argv);
    if (cmd == "ins") return cmdIns(argc, argv);
    if (cmd == "selftest") return cmdSelfTest(argc, argv);
    printf("unknown command\n");
    return 1;
}

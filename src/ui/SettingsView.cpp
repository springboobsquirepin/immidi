#include "App.h"
#include "Renderer.h"
#include "Fonts.h"
#include "Util.h"
#include "Widgets.h"

#include <algorithm>
#include <cstdio>

namespace immidi {

void App::drawSettings() {
    ImGui::BeginChild("##settings");
    if (ImGui::CollapsingHeader("MIDI outputs", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Refresh device list")) refreshDevices();
        ImGui::SameLine();
        ui::HelpMarker("Each MIDI port of the song (A, B, ...) can go to a different output. Multi-port songs (e.g. SC-88/SC-88Pro "
                       "32-part songs) need one output per port; ports that share one output collide.");
        int shown = std::max(4, file_ ? file_->numPorts : 1);
        for (int p = 0; p < shown; p++) {
            ImGui::PushID(p);
            ImGui::AlignTextToFramePadding();
            bool used = file_ && p < file_->numPorts;
            if (used) ImGui::Text("Port %c", 'A' + p);
            else ImGui::TextDisabled("Port %c", 'A' + p);
            ImGui::SameLine(80);
            ImGui::SetNextItemWidth(420);
            std::string cur = portDevice_[p].empty() ? (p == 0 ? std::string(MidiOutputs::kNone) : "(same as port A)") : portDevice_[p];
            if (ImGui::BeginCombo("##dev", cur.c_str())) {
                if (p > 0 && ImGui::Selectable("(same as port A)", portDevice_[p].empty())) {
                    portDevice_[p].clear();
                    outputs_->setPortDevice(p, portDevice_[0]);
                }
                for (const std::string& d : deviceList_) {
                    if (ImGui::Selectable(d.c_str(), d == portDevice_[p])) {
                        std::string err;
                        portDevice_[p] = d;
                        if (!outputs_->setPortDevice(p, d, &err)) loadError_ = "Port " + std::string(1, char('A' + p)) + ": " + err;
                        if (p == 0)
                            for (int q = 1; q < kMaxPorts; q++)
                                if (portDevice_[q].empty()) outputs_->setPortDevice(q, d);
                    }
                }
                ImGui::EndCombo();
            }
            if (outputs_->portSharesDevice(p) && used) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "shares an output with another port");
            }
            ImGui::PopID();
        }
        if (ImGui::Checkbox("Mirror effects to every port (one single-port synth per port)", &mirrorEffects_))
            player_->setMirrorEffects(mirrorEffects_);
        ImGui::SameLine();
        ui::HelpMarker("For SC-88 / SC-88Pro multi-port songs played on several single-port GS synths, e.g. one Roland Sound "
                       "Canvas VA per port. The SC-88 shares its effects (reverb, chorus, delay, EQ, insertion EFX) and master "
                       "settings between all 32 parts, so such songs set them once, usually on port A, and reach the parts of "
                       "the other port with \"other part group\" messages (50 xx xx). With this on, ImMidi sends those shared "
                       "settings to every port and the other group's messages to that port. Leave it off for a real SC-88, "
                       "SC-88Pro or SC-8850, and for songs that set each port's effects themselves (SC-88 double module mode "
                       "songs are recognized and left as they are).");
#if defined(__APPLE__)
        // The built-in synthesizer can play a SoundFont 2 or DLS bank instead of its own sounds.
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Apple DLS Synth sound bank:");
        ImGui::SameLine();
        std::string shownBank = soundBank_.empty() ? std::string("built-in (Apple's GS sound set)")
                                                   : pathToUtf8(pathFromUtf8(soundBank_).filename());
        ImGui::TextUnformatted(shownBank.c_str());
        if (!soundBank_.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", soundBank_.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Browse...##bank")) showFileDialog(DialogPurpose::SoundBank);
        ImGui::SameLine();
        ImGui::BeginDisabled(soundBank_.empty());
        if (ImGui::Button("Built-in##bank")) {
            soundBank_.clear();
            applySoundBank();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ui::HelpMarker("A SoundFont 2 (.sf2) or DLS (.dls) file for \"Apple DLS Synth (built-in)\" to play with instead of its own "
                       "sounds. Changing it reloads the synth; a playing song continues with the new sounds.");
        if (!soundBankError_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s (playing with the built-in sounds)", soundBankError_.c_str());
#endif
    }
    if (ImGui::CollapsingHeader("Graphics")) {
        // Takes effect at the next start (the window is created for one API).
        RendererKind current = activeRenderer() ? activeRenderer()->kind() : defaultRenderer();
        RendererKind chosen = defaultRenderer();
        if (!rendererSetting_.empty() && !(parseRendererName(rendererSetting_, &chosen) && rendererAvailable(chosen))) chosen = defaultRenderer();
        auto label = [](RendererKind k) {
            return std::string(rendererName(k)) + (k == RendererKind::OpenGL ? " (deprecated)" : k == defaultRenderer() ? " (default)" : "");
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Graphics API");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##renderer", label(chosen).c_str())) {
            for (RendererKind k : {defaultRenderer(), RendererKind::OpenGL}) {
                if (k == RendererKind::OpenGL && defaultRenderer() == RendererKind::OpenGL) break;  // listed once
                if (ImGui::Selectable(label(k).c_str(), k == chosen)) {
                    rendererSetting_ = rendererKey(k);
                    saveRequested_ = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ui::HelpMarker("ImMidi draws with the system's native graphics API (Direct3D 11 on Windows, Metal on macOS, Vulkan on "
                       "Linux). OpenGL is deprecated: it is only used when the native API cannot start, or when chosen here. "
                       "The change applies when ImMidi is started again.");
        if (chosen != current) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Restart ImMidi to use %s (now: %s).", rendererName(chosen), rendererName(current));
    }
    if (ImGui::CollapsingHeader("Sound module", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Target device");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        std::string cur = deviceSetting_ < 0 ? "Auto (follow the file's standard)" : deviceProfile(DeviceKind(deviceSetting_)).name;
        if (ImGui::BeginCombo("##target", cur.c_str())) {
            if (ImGui::Selectable("Auto (follow the file's standard)", deviceSetting_ < 0)) setDevice(-1);
            for (int d = 0; d < deviceCount(); d++)
                if (ImGui::Selectable(deviceProfile(DeviceKind(d)).name, deviceSetting_ == d)) setDevice(d);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ui::HelpMarker("The sound module connected to the outputs. It decides which reset is sent, how bank selects and "
                       "effects are interpreted, which instrument names are shown, and which messages are converted. "
                       "'Auto' assumes a module that natively understands whatever standard the file uses.");
        if (ImGui::Checkbox("Emulate other standards on the target device", &emulation_)) player_->setEmulation(emulation_);
        ImGui::BeginDisabled(!emulation_);
        if (ImGui::Checkbox("Use instrument conversion tables", &useConversionTables_))
            player_->setConversionTables(useConversionTables_, conversionDir_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ui::HelpMarker("The tables in the conversion folder map every voice, drum kit and drum note of the SC-55, SC-88 and "
                       "SC-88Pro maps to the closest XG ones and back, with volume corrections, and the SC-88Pro and SC-88 "
                       "sounds to the nearest ones of the SC-88 and SC-55. With them, e.g. an XG song's variation voices play "
                       "as matching SC-88 map variations, and an SC-88Pro song on an SC-55 uses the SC-55's nearest sounds "
                       "instead of falling back to capital tones; XG sounds also get the SC-8850's own. SC-55 songs on later "
                       "Sound Canvases use the SC-55 map. ImMidi Conversion Editor edits the tables (ImMidi reads them when it "
                       "starts).");
        {
            std::string inUse = player_->conversionTableInUse(), error = player_->conversionTablesError();
            if (!error.empty()) ImGui::TextDisabled("  Tables: %s", error.c_str());
            else ImGui::TextDisabled("  Tables: %s", conversionDir_.c_str());
            ImGui::TextDisabled("  In use: %s", inUse.empty() ? "none for this song and device" : inUse.c_str());
        }
        ImGui::SameLine();
        ui::HelpMarker("Converts messages written for another standard into equivalents for the target: e.g. XG bank selects, "
                       "drum kits, part parameters, reverb/chorus/variation types on a GS module (XG on SC-88Pro/SC-8850), "
                       "GS drum parts and EFX on XG, GM2 sound controllers as NRPNs, etc.");
        {
            const DeviceProfile& prof = profile();
            static const char* maps[] = {"Off (use the song's maps)", "SC-55 map", "SC-88 map", "SC-88Pro map", "SC-8850 map"};
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Force tone map");
            ImGui::SameLine(200);
            ImGui::SetNextItemWidth(320);
            ImGui::BeginDisabled(prof.gsMaxMap == 0);
            if (ImGui::BeginCombo("##forcemap", maps[std::clamp(forcedToneMap_, 0, 4)])) {
                for (int m = 0; m <= 4; m++) {
                    ImGui::BeginDisabled(m > prof.gsMaxMap);
                    if (ImGui::Selectable(maps[m], forcedToneMap_ == m)) {
                        forcedToneMap_ = m;
                        player_->setForcedToneMap(m);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ui::HelpMarker("Sound Canvas units from the SC-88 on contain the instrument sets of their predecessors. "
                           "Forcing a map plays every song with that set, e.g. the SC-55 map on an SC-8850 for songs written "
                           "for the SC-55. ImMidi sends it as bank select LSB (CC#32) with every program change, rewrites the "
                           "song's own map selections, and sets the parts' tone map-0 number after each GS reset.");
            if (forcedToneMap_ > prof.gsMaxMap && prof.gsMaxMap > 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "not available on %s", prof.name);
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Reset before playback");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        if (ImGui::Combo("##reset", &resetMode_, [](void*, int i) { return resetModeName(ResetMode(i)); }, nullptr, int(ResetMode::Count)))
            player_->setResetMode(ResetMode(resetMode_));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Wait after reset");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        if (ImGui::SliderInt("##resetdelay", &resetDelayMs_, 0, 1000, "%d ms")) player_->setResetDelayMs(resetDelayMs_);
        if (ImGui::Checkbox("Also reset when seeking", &resetOnSeek_)) player_->setResetOnSeek(resetOnSeek_);
        ImGui::SameLine();
        ui::HelpMarker("When seeking, controllers, programs and SysEx up to the new position are always re-sent (chased). "
                       "Enable this to additionally send the reset message first.");
        if (ImGui::Button("Send reset now")) player_->sendResetNow();
    }
    if (ImGui::CollapsingHeader("Loop points", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Honor loop points in files", &loopEnabled_)) player_->setLoop(loopEnabled_, loopRepeats_);
        ImGui::SameLine();
        ui::HelpMarker("Supported: 'loopStart'/'loopEnd' markers, Apogee EMIDI CC#116/117 (and 118/119), RPG Maker CC#111.");
        bool forever = loopRepeats_ < 0;
        if (ImGui::Checkbox("Loop forever", &forever)) {
            loopRepeats_ = forever ? -1 : 1;
            player_->setLoop(loopEnabled_, loopRepeats_);
        }
        if (!forever) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("Times to repeat the loop", &loopRepeats_)) {
                loopRepeats_ = std::clamp(loopRepeats_, 0, 99);
                player_->setLoop(loopEnabled_, loopRepeats_);
            }
        }
    }
    if (ImGui::CollapsingHeader("Performance (black MIDI)", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool changed = ImGui::Checkbox("Drop late notes when the output cannot keep up", &dropLateNotes_);
        ImGui::SameLine();
        ui::HelpMarker("Files with millions of notes can send more messages than a MIDI device or driver accepts. "
                       "Note-ons that are later than the limit below are dropped (with their note-offs) so playback "
                       "stays in time and the window stays responsive. Controllers, programs and SysEx are never dropped.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Maximum lag");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        ImGui::BeginDisabled(!dropLateNotes_);
        changed |= ImGui::SliderInt("##maxlag", &maxLagMs_, 5, 1000, "%d ms");
        ImGui::EndDisabled();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Ignore notes softer than");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        changed |= ImGui::SliderInt("##minvel", &minVelocity_, 0, 127, minVelocity_ ? "velocity %d" : "off");
        if (changed) player_->setNoteSkipping(dropLateNotes_ ? maxLagMs_ : 0, minVelocity_);
    }
    if (ImGui::CollapsingHeader("Instrument definitions", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%zu instrument definitions loaded (from the 'insdef' folders next to the program and in %s).", ins_.instruments().size(),
                    (configDirectory() + "/insdef").c_str());
        const DeviceProfile& prof = profile();
        ImGui::Text("For %s:", prof.name);
        auto insCombo = [&](const char* label, std::string& value, const char* def) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(200);
            ImGui::SetNextItemWidth(360);
            std::string preview = value.empty() ? std::string("Default: ") + def : value;
            ImGui::PushID(label);
            if (ImGui::BeginCombo("##ins", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable((std::string("Default: ") + def).c_str(), value.empty())) value.clear();
                for (auto& i : ins_.instruments())
                    if (ImGui::Selectable(i->name.c_str(), i->name == value)) value = i->name;
                ImGui::EndCombo();
            }
            ImGui::PopID();
        };
        insCombo("Normal parts", insOverrideMelodic_[int(prof.kind)], prof.insMelodic);
        insCombo("Drum parts", insOverrideDrum_[int(prof.kind)], prof.insDrums);
    }
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        int vm = int(viewMode_);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Channel view");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        if (ImGui::Combo("##viewmode", &vm, [](void*, int i) { return viewModeName(ViewMode(i)); }, nullptr, int(ViewMode::Count)))
            viewMode_ = ViewMode(vm);
        ImGui::Checkbox("Use compact rows automatically for multi-port songs", &autoCompactMultiport_);
        ImGui::Checkbox("Show playlist pane next to the tabs (wide windows)", &showPlaylistPane_);
        ImGui::Checkbox("Show channel detail panel", &showChannelDetail_);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Keyboard range");
        ImGui::SameLine(200);
        struct Range {
            const char* n;
            int lo, hi;
        } ranges[] = {{"All 128 notes", 0, 127}, {"C0 - B8 (12-119)", 12, 119}, {"88 keys (A0 - C8)", 21, 108}, {"C1 - C7 (24-96)", 24, 96}};
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(ranges[i].n, kbLo_ == ranges[i].lo && kbHi_ == ranges[i].hi)) {
                kbLo_ = ranges[i].lo;
                kbHi_ = ranges[i].hi;
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("UI scale");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        if (ImGui::SliderFloat("##uiscale", &uiScale_, 0.75f, 2.0f, "%.2f")) ImGui::GetStyle().FontScaleMain = uiScale_;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Meter decay");
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(320);
        ImGui::SliderFloat("##decay", &meterDecay_, 0.3f, 5.0f, "%.1f / s");
        if (ImGui::Checkbox("Dark theme", &darkTheme_)) applyTheme();
        ImGui::Text("CJK font: %s", ui::g_fonts.cjkPath.empty() ? "none found (Japanese text shows as '?')" : ui::g_fonts.cjkPath.c_str());
        ImGui::SameLine();
        ui::HelpMarker("Put a .ttf/.otf/.ttc font into a 'fonts' folder next to the program to use it for CJK text, "
                       "or install Noto Sans CJK / a Japanese font on the system.");
        drawLcdColorSettings();
    }
    if (ImGui::CollapsingHeader("Playlist and files")) {
        ImGui::Checkbox("Remember the playlist between sessions", &rememberPlaylist_);
        ImGui::Checkbox("Play files dropped outside the playlist", &playDroppedFiles_);
        ImGui::SameLine();
        ui::HelpMarker("Files dropped onto the playlist (the side pane, or the list in the Playlist tab) are inserted where "
                       "they are dropped, and playback goes on. Dropped anywhere else in the window, they are added at the "
                       "end of the playlist and the first one plays at once; with this off, they are only added.");
        ImGui::Checkbox("Use the system's file dialogs", &useNativeDialogs_);
        ImGui::SameLine();
        ui::HelpMarker("Windows and macOS use their standard dialogs; Linux uses zenity or kdialog when installed. "
                       "Otherwise ImMidi's own file browser is used.");
        if (!NativeDialogs::available()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not available here)");
        }
        ImGui::Checkbox("Include subfolders when adding a folder", &addSubfolders_);
        ImGui::Checkbox("Export playlists with relative paths", &exportRelative_);
        ImGui::Text("Settings folder: %s", configDirectory().c_str());
    }
    ImGui::EndChild();
}

} // namespace immidi

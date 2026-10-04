#include "App.h"
#include "Fonts.h"
#include "Util.h"
#include "Widgets.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace immidi {

namespace {

const char* metaTypeName(int t) {
    switch (t) {
    case 1: return "Text";
    case 2: return "Copyright";
    case 3: return "Track name";
    case 4: return "Instrument";
    case 5: return "Lyric";
    case 6: return "Marker";
    case 7: return "Cue point";
    case 8: return "Program name";
    case 9: return "Device name";
    default: return "Meta";
    }
}

} // namespace

void App::drawInfo() {
    if (!file_) {
        ImGui::TextDisabled("No file loaded.");
        return;
    }
    const std::vector<TrackStat>& stats = file_->trackStats;

    ImGui::BeginChild("##info");
    if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##fileinfo", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", k);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.c_str());
            };
            char b[256];
            row("Path", file_->path);
            row("Container", file_->containerFormat);
            snprintf(b, sizeof b, "%zu bytes", file_->fileSize);
            row("Size", b);
            row("Title", file_->title);
            row("Copyright", file_->copyright);
            snprintf(b, sizeof b, "SMF format %d, %d track(s)", file_->format, file_->numTracks);
            row("Format", b);
            row("Resolution", file_->resolutionDescription());
            snprintf(b, sizeof b, "%s  (raw header value 0x%04X)", file_->divisionDescription().c_str(), file_->rawDivision);
            row("Time division", b);
            snprintf(b, sizeof b, "%s  (%llu ticks)", formatTime(file_->lengthUs / 1e6, true).c_str(), (unsigned long long)file_->lengthTicks);
            row("Length", b);
            snprintf(b, sizeof b, "%zu events, %zu notes, %zu SysEx", file_->events.size(), file_->noteCount, file_->sysexCount);
            row("Contents", b);
            snprintf(b, sizeof b, "%d", file_->numPorts);
            row("MIDI ports", b);
            row("Standard", info_.summary());
            std::string details;
            if (info_.hasGmOn) details += "GM On, ";
            if (info_.hasGm2On) details += "GM2 On, ";
            if (info_.hasGsReset) details += "GS Reset, ";
            if (info_.hasModeSet) details += "SC-88 Mode Set, ";
            if (info_.hasXgOn) details += "XG System On, ";
            if (info_.usesGsEfx) details += "GS insertion EFX, ";
            if (info_.usesGsDelay) details += "GS delay, ";
            if (info_.maxGsMap) details += "GS tone map " + std::to_string(info_.maxGsMap) + ", ";
            if (!details.empty()) details.resize(details.size() - 2);
            row("Detected", details.empty() ? "-" : details);
            row("Suggested device", deviceProfile(detectedInfo_.suggestedDevice).name);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Song made for");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(260 * uiScale_);
            if (ImGui::BeginCombo("##songmodule", info_.moduleChosen ? deviceProfile(info_.suggestedDevice).name : "As detected")) {
                songModuleMenu({file_->path});
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ui::HelpMarker("The module the song is made for, when its messages do not tell (e.g. a song for the SC-8850 "
                           "without a GS reset). The emulation converts from it and the Auto device follows it. "
                           "ImMidi remembers it for the file.");
            if (!info_.moduleKeyword.empty()) row("Module named in text", info_.moduleKeyword);
            std::string drums;
            for (int p = 0; p < file_->numPorts; p++)
                for (int c = 0; c < 16; c++)
                    if (info_.drumChannels[p] & (1u << c)) {
                        if (file_->numPorts > 1) drums += char('A' + p);
                        drums += std::to_string(c + 1) + " ";
                    }
            row("Drum channels", drums.empty() ? "-" : drums);
            if (file_->hasLoop()) {
                snprintf(b, sizeof b, "%s: %s -> %s (ticks %lld -> %lld)%s", file_->loopType.c_str(), formatTime(file_->loopStartUs / 1e6, true).c_str(),
                         formatTime(file_->loopEndUs / 1e6, true).c_str(), (long long)file_->loopStartTick, (long long)file_->loopEndTick,
                         file_->loopCount ? (", count " + std::to_string(file_->loopCount)).c_str() : "");
                row("Loop", b);
            }
            if (file_->emidi) {
                snprintf(b, sizeof b, "yes, %d track(s) excluded for other sound cards", file_->emidiExcludedTracks);
                row("EMIDI", b);
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Text encoding");
            ImGui::TableNextColumn();
            ImGui::Text("%s (detected %s)", textEncodingName(file_->encoding), textEncodingName(file_->detectedEncoding));
            ImGui::SameLine();
            const char* encs[] = {"Auto", "UTF-8", "Shift-JIS", "Latin-1"};
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("##enc", &encodingOverride_, encs, 4) && fileMut_) {
                fileMut_->setEncoding(TextEncoding(encodingOverride_));
                infoReader_->setEncoding(TextEncoding(encodingOverride_));
                // The open song's playlist title follows; other entries change when they are opened.
                int cur = playlist_.current;
                if (cur >= 0 && cur < int(playlist_.entries.size()) && playlist_.entries[size_t(cur)].path == fileMut_->path) {
                    playlist_.entries[size_t(cur)].info = songInfoOf(*fileMut_);
                    playlist_.revision++;
                }
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Tempo, time and key signatures")) {
        ImGui::Text("%zu tempo change(s)", file_->tempos.size());
        if (ImGui::BeginTable("##tempo", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchSame, ImVec2(0, 160))) {
            ImGui::TableSetupColumn("Tick");
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("BPM");
            ImGui::TableSetupColumn("us / quarter");
            ImGui::TableHeadersRow();
            for (const TempoChange& t : file_->tempos) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)t.tick);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatTime(t.timeUs / 1e6, true).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", 60000000.0 / t.usPerQn);
                ImGui::TableNextColumn();
                ImGui::Text("%u", t.usPerQn);
            }
            ImGui::EndTable();
        }
        for (const TimeSignature& ts : file_->timeSigs)
            ImGui::BulletText("Tick %llu: %d/%d (%d clocks/click, %d 32nds/quarter)", (unsigned long long)ts.tick, ts.num, 1 << ts.denPow, ts.clocks, ts.n32);
        for (const KeySignature& ks : file_->keySigs) ImGui::BulletText("Tick %llu: %s", (unsigned long long)ks.tick, ks.name().c_str());
    }
    if (ImGui::CollapsingHeader("Tracks", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##tracks", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Port");
            ImGui::TableSetupColumn("Channels");
            ImGui::TableSetupColumn("Events");
            ImGui::TableSetupColumn("Notes");
            ImGui::TableHeadersRow();
            for (int t = 0; t < file_->numTracks; t++) {
                const TrackStat& s = stats[size_t(t)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", t);
                ImGui::TableNextColumn();
                std::string name = t < int(file_->trackNames.size()) ? decodeText(file_->trackNames[size_t(t)], file_->encoding) : std::string();
                if (s.excluded) ImGui::TextDisabled("%s  (EMIDI: other sound card, skipped)", name.c_str());
                else ImGui::TextUnformatted(name.c_str());
                ImGui::TableNextColumn();
                if (s.port >= 0) ImGui::Text("%c", 'A' + s.port);
                ImGui::TableNextColumn();
                std::string chs;
                for (int c = 0; c < 16; c++)
                    if (s.channels & (1u << c)) chs += std::to_string(c + 1) + " ";
                ImGui::TextUnformatted(chs.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", s.events);
                ImGui::TableNextColumn();
                ImGui::Text("%u", s.notes);
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Text events")) {
        if (ImGui::BeginTable("##texts", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 260))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Track");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(file_->texts.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                    const TextItem& t = file_->texts[size_t(i)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(formatTime(t.timeUs / 1e6, true).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", t.track);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(metaTypeName(t.type));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(t.text.c_str());
                }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("SysEx sent to the device(s)")) {
        if (ImGui::Button("Clear log")) player_->clearSysexLog();
        ImGui::SameLine();
        ImGui::TextDisabled("Last 500 messages after emulation, including resets and your edits.");
        std::vector<LogEntry> log = player_->sysexLog();
        ImGui::PushFont(ui::g_fonts.mono, 0.0f);
        ImGui::BeginChild("##sxlog", ImVec2(0, 260), ImGuiChildFlags_Borders);
        ImGuiListClipper clip;
        clip.Begin(int(log.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const LogEntry& e = log[size_t(i)];
                ImGui::Text("%s  %c  %s", formatTime(e.songUs / 1e6, true).c_str(), 'A' + e.port, e.text.c_str());
            }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

} // namespace immidi

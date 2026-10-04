#include "App.h"
#include "Util.h"
#include "Widgets.h"

#include <algorithm>
#include <cstdio>

namespace immidi {

void App::openInstrumentPicker(int port, int ch) {
    pickerOpen_ = true;
    pickerPort_ = port;
    pickerCh_ = ch;
    const ChannelState& c = snap_->ports[port].ch[ch];
    pickerBank_ = insBank(c, c.drum);
    pickerFilter_[0] = 0;
    pickerRequest_ = true;
}

static std::string bankLabel(const DeviceProfile& prof, int bank, bool drum) {
    int msb = bank >> 7, lsb = bank & 127;
    char buf[96];
    if (prof.family == MidiStandard::GS) {
        static const char* maps[] = {"default map", "SC-55 map", "SC-88 map", "SC-88Pro map", "SC-8850 map"};
        const char* m = lsb <= 4 ? maps[lsb] : "map ?";
        if (drum) snprintf(buf, sizeof buf, "Drum sets (%s)", m);
        else if (msb == 0) snprintf(buf, sizeof buf, "Capital tones (%s)", m);
        else snprintf(buf, sizeof buf, "Variation %d (%s)", msb, m);
    } else if (prof.family == MidiStandard::XG) {
        if (msb == 127) snprintf(buf, sizeof buf, "Drum kits (MSB 127)");
        else if (msb == 126) snprintf(buf, sizeof buf, "SFX kits (MSB 126)");
        else if (msb == 64) snprintf(buf, sizeof buf, "SFX voices (MSB 64)");
        else snprintf(buf, sizeof buf, "Bank MSB %d / LSB %d", msb, lsb);
    } else {
        snprintf(buf, sizeof buf, "Bank MSB %d / LSB %d", msb, lsb);
    }
    return buf;
}

void App::drawInstrumentPicker() {
    if (!pickerOpen_) return;
    if (pickerRequest_) {
        ImGui::OpenPopup("Choose instrument");
        pickerRequest_ = false;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(820.0f, vp->WorkSize.x * 0.9f), std::min(560.0f, vp->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Choose instrument", &pickerOpen_)) {
        if (!ImGui::IsPopupOpen("Choose instrument")) pickerOpen_ = false;
        return;
    }
    int port = pickerPort_, ch = pickerCh_;
    const ChannelState& c = snap_->ports[port].ch[ch];
    const DeviceProfile& prof = profile();
    bool drum = c.drum;
    const InsInstrument* ins = drum ? drumIns() : melodicIns();
    ImGui::Text("Channel %s%d", file_ && file_->numPorts > 1 ? (std::string(1, char('A' + port)) + ":").c_str() : "", ch + 1);
    ImGui::SameLine(0, 12);
    ImGui::TextDisabled("%s  |  definition: %s", drum ? "drum part" : "normal part", ins ? ins->name.c_str() : "(none found)");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##filter", "Search all banks...", pickerFilter_, sizeof pickerFilter_);
    ImGui::SameLine();
    // Manual entry
    static int man[3] = {0, 0, 1};
    ImGui::SetNextItemWidth(220);
    ImGui::InputInt3("MSB / LSB / Prg", man);
    ImGui::SameLine();
    if (ImGui::Button("Send")) {
        player_->setProgram(port, ch, std::clamp(man[0], 0, 127), std::clamp(man[1], 0, 127), std::clamp(man[2] - 1, 0, 127));
    }

    auto choose = [&](int bank, int prog) {
        int msb = bank >> 7, lsb = bank & 127;
        if (prof.family == MidiStandard::GS && drum) msb = 0;
        player_->setProgram(port, ch, msb, lsb, prog);
    };

    float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.3f;
    std::string filter = lowerAscii(pickerFilter_);
    if (!ins) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "No instrument definition found for %s. Put .ins files into the insdef folder.", prof.name);
    } else if (!filter.empty()) {
        ImGui::BeginChild("##results", ImVec2(0, listH), ImGuiChildFlags_Borders);
        for (auto& [bank, list] : ins->patchByBank) {
            for (auto& [prog, name] : *list) {
                if (lowerAscii(name).find(filter) == std::string::npos) continue;
                char label[160];
                snprintf(label, sizeof label, "%03d  %s   —  %s##%d_%d", prog + 1, name.c_str(), bankLabel(prof, bank, drum).c_str(), bank, prog);
                if (ImGui::Selectable(label, bank == insBank(c, drum) && prog == c.program)) choose(bank, prog);
            }
        }
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##banks", ImVec2(300, listH), ImGuiChildFlags_Borders);
        for (auto& [bank, list] : ins->patchByBank) {
            std::string l = bankLabel(prof, bank, drum) + "  (" + std::to_string(list->size()) + ")";
            ImGui::PushID(bank);
            if (ImGui::Selectable(l.c_str(), pickerBank_ == bank)) pickerBank_ = bank;
            if (ImGui::IsWindowAppearing() && pickerBank_ == bank) ImGui::SetScrollHereY();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##progs", ImVec2(0, listH), ImGuiChildFlags_Borders);
        const NameList* list = InsLibrary::patchList(ins, pickerBank_);
        if (list) {
            for (auto& [prog, name] : *list) {
                char label[128];
                snprintf(label, sizeof label, "%03d  %s##%d", prog + 1, name.c_str(), prog);
                bool cur = pickerBank_ == insBank(c, drum) && prog == c.program;
                if (ImGui::Selectable(label, cur)) choose(pickerBank_, prog);
                if (cur && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            }
        } else {
            ImGui::TextDisabled("Select a bank.");
        }
        ImGui::EndChild();
    }
    ImGui::TextDisabled("Choosing an instrument locks the program on this channel (right-click the instrument > Unlock to follow the file again).");
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pickerOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace immidi

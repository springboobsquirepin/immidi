#include "App.h"
#include "Util.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace immidi {

namespace {

const ImU32 kVolColor = IM_COL32(70, 150, 230, 255);
const ImU32 kExpColor = IM_COL32(90, 190, 200, 255);
const ImU32 kPanColor = IM_COL32(200, 160, 80, 255);
const ImU32 kBendColor = IM_COL32(200, 110, 200, 255);
const ImU32 kModColor = IM_COL32(160, 200, 90, 255);
const ImU32 kRevColor = IM_COL32(110, 130, 230, 255);
const ImU32 kChoColor = IM_COL32(90, 200, 150, 255);
const ImU32 kDlyColor = IM_COL32(220, 120, 110, 255);
const ImU32 kSndColor = IM_COL32(150, 150, 220, 255);

// Pan as Sound Canvas modules show it: L63..C..R63 (CC#10 0 and 1 are both the leftmost
// position), "Rnd" only for a part pan set to random by SysEx.
std::string panText(int v, bool random) {
    if (random) return "Rnd";
    if (v == 64) return "C";
    char b[8];
    snprintf(b, sizeof b, "%c%d", v < 64 ? 'L' : 'R', v < 64 ? 64 - std::max(v, 1) : v - 64);
    return b;
}

} // namespace

void App::drawChannels() {
    int ports = file_ ? file_->numPorts : 1;
    ViewMode mode = viewMode_;
    if (ports <= 1 && (mode == ViewMode::PortTabs || mode == ViewMode::SideBySide)) mode = ViewMode::Normal;
    if (ports > 1 && mode == ViewMode::Normal && autoCompactMultiport_) mode = ViewMode::Compact;
    float detailH = showChannelDetail_ ? ImGui::GetFrameHeightWithSpacing() * 5.6f : 0.0f;
    float avail = ImGui::GetContentRegionAvail().y - detailH;

    ImGui::BeginChild("##channels", ImVec2(0, avail), ImGuiChildFlags_None);
    switch (mode) {
    case ViewMode::PortTabs: {
        if (ImGui::BeginTabBar("##porttabs")) {
            for (int p = 0; p < ports; p++) {
                char label[32];
                snprintf(label, sizeof label, "Port %c", 'A' + p);
                if (ImGui::BeginTabItem(label)) {
                    portTab_ = p;
                    drawChannelTable(p, ImGui::GetContentRegionAvail().y, false, false);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        break;
    }
    case ViewMode::SideBySide: {
        float w = (ImGui::GetContentRegionAvail().x - 6.0f * (ports - 1)) / float(ports);
        for (int p = 0; p < ports; p++) {
            if (p) ImGui::SameLine(0, 6);
            char id[24];
            snprintf(id, sizeof id, "##side%d", p);
            ImGui::BeginChild(id, ImVec2(w, 0), ImGuiChildFlags_None);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Port %c", 'A' + p);
            drawChannelTable(p, ImGui::GetContentRegionAvail().y, true, true);
            ImGui::EndChild();
        }
        break;
    }
    default: {
        bool compact = mode == ViewMode::Compact;
        for (int p = 0; p < ports; p++) {
            if (ports > 1) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Port %c%s", 'A' + p,
                                              outputs_->portSharesDevice(p) ? "   (shares the output device of another port)" : "");
            float h = ports > 1 ? 0.0f : ImGui::GetContentRegionAvail().y;
            drawChannelTable(p, h, compact, false);
        }
        break;
    }
    }
    ImGui::EndChild();
    if (showChannelDetail_) drawChannelDetail();
}

void App::drawChannelTable(int port, float height, bool compact, bool narrow) {
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    float headerH = ImGui::GetTextLineHeightWithSpacing();
    float rowH;
    if (height > 0) {
        rowH = (height - headerH - 4) / 16.0f - ImGui::GetStyle().CellPadding.y * 2;
        rowH = std::clamp(rowH, compact ? 16.0f : 20.0f, compact ? 26.0f : 44.0f);
    } else {
        rowH = compact ? 18.0f : 30.0f;
    }
    char id[16];
    snprintf(id, sizeof id, "##chtab%d", port);
    int cols = narrow ? 9 : 17;
    if (!ImGui::BeginTable(id, cols, tf)) return;
    float u = uiScale_;
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 34 * u);
    ImGui::TableSetupColumn("M S", ImGuiTableColumnFlags_WidthFixed, 44 * u);
    ImGui::TableSetupColumn("Instrument", ImGuiTableColumnFlags_WidthFixed, (narrow ? 130 : 170) * u);
    if (!narrow) ImGui::TableSetupColumn("Bank/Prg", ImGuiTableColumnFlags_WidthFixed, 72 * u);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 46 * u);
    ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 44 * u);
    if (!narrow) {
        ImGui::TableSetupColumn("Exp", ImGuiTableColumnFlags_WidthFixed, 44 * u);
    }
    ImGui::TableSetupColumn("Pan", ImGuiTableColumnFlags_WidthFixed, 44 * u);
    if (!narrow) {
        ImGui::TableSetupColumn("Bend", ImGuiTableColumnFlags_WidthFixed, 52 * u);
        ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthFixed, 40 * u);
    }
    ImGui::TableSetupColumn("Rev", ImGuiTableColumnFlags_WidthFixed, 40 * u);
    ImGui::TableSetupColumn("Cho", ImGuiTableColumnFlags_WidthFixed, 40 * u);
    if (!narrow) {
        ImGui::TableSetupColumn(profile().family == MidiStandard::XG ? "Var" : "Dly", ImGuiTableColumnFlags_WidthFixed, 40 * u);
        ImGui::TableSetupColumn("Pedal", ImGuiTableColumnFlags_WidthFixed, 44 * u);
        ImGui::TableSetupColumn("Poly", ImGuiTableColumnFlags_WidthFixed, 26 * u);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 20 * u);
    }
    ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int ch = 0; ch < 16; ch++) drawChannelRow(port, ch, rowH, compact, narrow);
    ImGui::EndTable();
}

void App::drawChannelRow(int port, int ch, float rowH, bool compact, bool narrow) {
    const ChannelState& c = snap_->ports[port].ch[ch];
    const ChannelLocks& L = snap_->locks[port][ch];
    bool used = file_ && (file_->channelMask[port] & (1u << ch));
    bool muted = snap_->mute[port] & (1u << ch);
    bool solo = snap_->solo[port] & (1u << ch);
    bool selected = selPort_ == port && selCh_ == ch;
    ImGui::PushID(port * 16 + ch);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
    if (selected) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(80, 110, 170, 60));

    float barH = std::min(rowH, compact ? 16.0f : 20.0f);
    auto vcenter = [&](float h) { ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (rowH - h) * 0.5f)); };
    float cellW;

    // Channel number (click to select)
    ImGui::TableNextColumn();
    vcenter(ImGui::GetTextLineHeight());
    {
        char label[24];
        if (file_ && file_->numPorts > 1) snprintf(label, sizeof label, "%c%02d", 'A' + port, ch + 1);
        else snprintf(label, sizeof label, "%02d", ch + 1);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p0.x + 3, p0.y + ImGui::GetTextLineHeight() + 2),
                                                  ui::ChannelColor(ch, used ? 1.0f : 0.3f));
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y));
        ImGui::PushStyleColor(ImGuiCol_Text, used ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2(0, 0))) {
            selPort_ = port;
            selCh_ = ch;
        }
        ImGui::PopStyleColor();
    }

    // Mute / Solo
    ImGui::TableNextColumn();
    vcenter(barH);
    {
        ImVec2 bs(19 * uiScale_, barH);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
        if (ui::ToggleButton("M", muted, bs, IM_COL32(230, 80, 70, 255))) player_->setMute(port, ch, !muted);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Mute");
        ImGui::SameLine();
        if (ui::ToggleButton("S", solo, bs, IM_COL32(240, 200, 60, 255))) player_->setSolo(port, ch, !solo);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Solo");
        ImGui::PopStyleVar(2);
    }

    // Instrument
    ImGui::TableNextColumn();
    vcenter(barH);
    {
        bool fb = false;
        std::string name = instrumentName(port, ch, &fb);
        if (c.drum) name = "[D] " + name;
        cellW = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 0));
        if (c.drum) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(120, 70, 40, 200));
        if (L.program) ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 170, 40, 255));
        if (L.program) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
        if (ImGui::Button(name.c_str(), ImVec2(cellW, barH))) openInstrumentPicker(port, ch);
        if (L.program) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        if (c.drum) ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::Text("%s%s", name.c_str(), fb ? "  (variation not found, capital tone shown)" : "");
            ImGui::TextDisabled("Bank MSB %d, LSB %d, Program %d%s", c.bankMsb, c.bankLsb, c.program + 1, c.drum ? "  (drum part)" : "");
            ImGui::TextDisabled("Click to change the instrument. Right-click for drum part / unlock.");
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem("##instctx")) {
            if (ImGui::MenuItem("Choose instrument...")) openInstrumentPicker(port, ch);
            if (profile().family != MidiStandard::GM) {
                if (ImGui::MenuItem("Drum part: off", nullptr, !c.drum)) player_->setDrumPart(port, ch, 0);
                if (ImGui::MenuItem("Drum part: map 1", nullptr, c.drum && c.drumMap != 2 && c.drumMap != 3)) player_->setDrumPart(port, ch, 1);
                if (profile().family != MidiStandard::GM2 &&
                    ImGui::MenuItem("Drum part: map 2", nullptr, c.drum && (c.drumMap == 2 || c.drumMap == 3)))
                    player_->setDrumPart(port, ch, 2);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Unlock channel (follow the file again)", nullptr, false, L.any())) player_->unlock(port, ch);
            ImGui::EndPopup();
        }
    }

    // Bank / program
    if (!narrow) {
        ImGui::TableNextColumn();
        vcenter(ImGui::GetTextLineHeight());
        ImGui::TextDisabled("%03d:%03d", c.bankMsb, c.bankLsb);
        ImGui::SameLine(0, 3);
        ImGui::Text("%03d", c.program + 1);
    }

    // Level meter
    ImGui::TableNextColumn();
    vcenter(barH * 0.7f);
    ui::Meter(meter_[port][ch], ImVec2(ImGui::GetContentRegionAvail().x, barH * 0.7f));

    auto bar = [&](const char* id, int cc, ImU32 color, bool centered, int def, const char* fmtText = nullptr) {
        ImGui::TableNextColumn();
        vcenter(barH);
        int v = c.cc[cc];
        char num[4];
        const char* text = fmtText;
        if (!text) {  // 0..127 without snprintf (this runs for ~140 bars every frame)
            int n = 0;
            if (v >= 100) num[n++] = char('0' + v / 100);
            if (v >= 10) num[n++] = char('0' + (v / 10) % 10);
            num[n++] = char('0' + v % 10);
            num[n] = 0;
            text = num;
        }
        ui::BarResult r = ui::ValueBar(id, &v, 0, 127, def, ImVec2(ImGui::GetContentRegionAvail().x, barH), text, color, L.cc[size_t(cc)], centered);
        if (r.changed) player_->setChannelCC(port, ch, cc, v, true);
        if (r.rightClicked) player_->unlockCC(port, ch, cc);
        if (r.hovered) {
            const char* n = ccName(cc);
            if (cc == 10 && c.randomPan)
                ImGui::SetItemTooltip("Pan: random (part pan SysEx; CC#10 cannot select it)\nDrag / wheel to change, double-click resets.");
            else
                ImGui::SetItemTooltip("%s (CC#%d): %d%s\nDrag / wheel to change, double-click resets, right-click unlocks.", n ? n : "CC", cc,
                                      c.cc[cc], L.cc[size_t(cc)] ? "  [locked]" : "");
        }
    };
    bar("vol", 7, kVolColor, false, 100);
    if (!narrow) bar("exp", 11, kExpColor, false, 127);
    bar("pan", 10, kPanColor, true, 64, panText(c.cc[10], c.randomPan).c_str());

    if (!narrow) {
        // Pitch bend
        ImGui::TableNextColumn();
        vcenter(barH);
        {
            int v = c.pitchBend;
            char text[24];
            double semis = (v - 8192) / 8192.0 * c.bendRange;
            snprintf(text, sizeof text, "%+.2f", semis);
            ui::BarResult r = ui::ValueBar("bend", &v, 0, 16383, 8192, ImVec2(ImGui::GetContentRegionAvail().x, barH), text, kBendColor, L.bend, true);
            if (r.changed) player_->setPitchBend(port, ch, v, true);
            if (r.rightClicked) player_->unlock(port, ch);
            if (r.hovered) ImGui::SetItemTooltip("Pitch bend %d (%+.2f semitones, range %d)\nDouble-click centres it.", c.pitchBend - 8192, semis, c.bendRange);
        }
        bar("mod", 1, kModColor, false, 0);
    }
    bar("rev", 91, kRevColor, false, 40);
    bar("cho", 93, kChoColor, false, 0);
    if (!narrow) {
        bar("dly", 94, kDlyColor, false, 0);

        // Pedals
        ImGui::TableNextColumn();
        vcenter(barH);
        {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            struct Ped {
                int cc;
                const char* label;
                ImU32 col;
            } peds[3] = {{64, "S", IM_COL32(90, 220, 110, 255)}, {66, "T", IM_COL32(90, 170, 240, 255)}, {67, "F", IM_COL32(230, 170, 80, 255)}};
            float pw = (w - 4) / 3.0f;
            for (int i = 0; i < 3; i++) {
                bool on = c.cc[peds[i].cc] >= 64;
                ImVec2 a(p0.x + i * (pw + 2), p0.y), b(a.x + pw, p0.y + barH);
                ImGui::SetCursorScreenPos(a);
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("##ped", ImVec2(pw, barH))) player_->setChannelCC(port, ch, peds[i].cc, on ? 0 : 127, false);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s: %s (click to toggle)", i == 0 ? "Sustain (CC#64)" : i == 1 ? "Sostenuto (CC#66)" : "Soft (CC#67)", on ? "on" : "off");
                ImGui::PopID();
                dl->AddRectFilled(a, b, on ? peds[i].col : ImGui::GetColorU32(ImGuiCol_FrameBg));
                ImVec2 ts = ImGui::CalcTextSize(peds[i].label);
                dl->AddText(ImVec2(a.x + (pw - ts.x) * 0.5f, a.y + (barH - ts.y) * 0.5f),
                            on ? IM_COL32(10, 10, 10, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled), peds[i].label);
            }
        }

        // Polyphony: sounding notes, including the ones a pedal holds after their key was released
        ImGui::TableNextColumn();
        vcenter(ImGui::GetTextLineHeight());
        if (int sounding = c.activeNotes + c.sustainedNotes) {
            ImGui::Text("%d", sounding);
            if (c.sustainedNotes) ImGui::SetItemTooltip("%d key%s held, %d note%s held by the pedal", c.activeNotes, c.activeNotes == 1 ? "" : "s",
                                                        c.sustainedNotes, c.sustainedNotes == 1 ? "" : "s");
        } else {
            ImGui::TextDisabled("-");
        }

        // Lock indicator
        ImGui::TableNextColumn();
        vcenter(barH);
        if (L.any()) {
            if (ui::IconButton("unlock", ui::Icon::Lock, ImVec2(ImGui::GetContentRegionAvail().x, barH), false,
                               "Some controls are locked to your values.\nClick to release them and follow the file again."))
                player_->unlock(port, ch);
        }
    }

    // Keyboard
    ImGui::TableNextColumn();
    {
        float kh = std::max(10.0f, rowH - 2.0f);
        ImVec2 size(ImGui::GetContentRegionAvail().x, kh);
        ImU32 col = c.drum ? IM_COL32(255, 150, 60, 255) : ui::ChannelColor(ch);
        if (muted || (!solo && std::any_of(std::begin(snap_->solo), std::end(snap_->solo), [](uint16_t s) { return s != 0; })))
            col = IM_COL32(120, 120, 120, 255);
        ui::KeyboardResult kr = ui::Keyboard("kb", c.noteVel, c.noteSus, kbLo_, kbHi_, size, col, &heldNote_[port][ch]);
        if (kr.released >= 0) player_->previewNote(port, ch, kr.released, 0);
        if (kr.pressed >= 0) player_->previewNote(port, ch, kr.pressed, 100);
        if (kr.hovered >= 0) {
            std::string dn = c.drum ? drumNoteName(port, ch, kr.hovered) : std::string();
            ImGui::SetTooltip("%s (%d)%s%s", noteName(kr.hovered).c_str(), kr.hovered, dn.empty() ? "" : "  ", dn.c_str());
        }
    }
    ImGui::PopID();
}

void App::drawChannelDetail() {
    int port = std::clamp(selPort_, 0, snap_->numPorts - 1), ch = selCh_;
    const ChannelState& c = snap_->ports[port].ch[ch];
    const ChannelLocks& L = snap_->locks[port][ch];
    const DeviceProfile& prof = profile();
    ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::ChannelColor(ch)), "%s%d", file_ && file_->numPorts > 1 ? (std::string(1, char('A' + port)) + ":").c_str() : "Ch ", ch + 1);
    ImGui::SameLine();
    bool fb;
    ImGui::TextUnformatted(instrumentName(port, ch, &fb).c_str());
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("Bend range %d | Coarse %+d | Fine %+.1f c | Key shift %+d | Pressure %d | Portamento %s (time %d)", c.bendRange,
                        c.coarseTune, (c.fineTune - 8192) / 8192.0 * 100.0, c.keyShift, c.pressure, c.cc[65] >= 64 ? "on" : "off", c.cc[5]);

    // Sound parameters (NRPN / GM2 sound controllers)
    const SoundParam params[] = {SoundParam::Attack, SoundParam::Decay, SoundParam::Release, SoundParam::VibRate, SoundParam::VibDepth,
                                 SoundParam::VibDelay, SoundParam::Cutoff, SoundParam::Resonance, SoundParam::HpfCutoff,
                                 SoundParam::EqBassGain, SoundParam::EqTrebleGain, SoundParam::EqBassFreq, SoundParam::EqTrebleFreq};
    float w = (ImGui::GetContentRegionAvail().x - 8 * 12) / 13.0f;
    float h = ImGui::GetFrameHeight();
    for (int i = 0; i < 13; i++) {
        if (i) ImGui::SameLine(0, 8);
        ImGui::BeginGroup();
        SoundParam p = params[i];
        bool supported = player_->soundParamSupported(p);
        ImGui::TextDisabled("%s", soundParamShortName(p));
        int v = c.sound[int(p)];
        char text[16];
        if (p == SoundParam::EqBassFreq || p == SoundParam::EqTrebleFreq) snprintf(text, sizeof text, "%d", v);
        else snprintf(text, sizeof text, "%+d", v - 64);
        ImGui::PushID(i);
        ui::BarResult r = ui::ValueBar("snd", &v, 0, 127, soundParamDefault(p), ImVec2(w, h), text, kSndColor, (L.sound >> int(p)) & 1,
                                       p != SoundParam::EqBassFreq && p != SoundParam::EqTrebleFreq, supported);
        if (r.changed) player_->setSoundParam(port, ch, p, v, true);
        if (r.rightClicked) player_->unlock(port, ch);
        if (r.hovered) {
            int lsb = soundParamNrpnLsb(p), cc = soundParamGm2Cc(p);
            std::string how;
            if (lsb >= 0) {
                char b[48];
                snprintf(b, sizeof b, "NRPN 1/%d", lsb);
                how = b;
            }
            if (cc >= 0) how += (how.empty() ? "" : ", ") + std::string("GM2 CC#") + std::to_string(cc);
            ImGui::SetItemTooltip("%s: %d (%s)%s", soundParamName(p), v, how.c_str(), supported ? "" : "\nNot supported by the target device");
        }
        ImGui::PopID();
        ImGui::EndGroup();
    }

    // Part settings
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Part:");
    ImGui::SameLine();
    int ks = c.keyShift;
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("Key shift", &ks)) player_->setKeyShift(port, ch, std::clamp(ks, -24, 24));
    ImGui::SameLine(0, 16);
    int br = c.bendRange;
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("Bend range", &br)) player_->setBendRange(port, ch, std::clamp(br, 0, 24));
    ImGui::SameLine(0, 16);
    if (prof.family != MidiStandard::GM) {
        int map = c.drum ? ((c.drumMap == 2 || c.drumMap == 3) && prof.family != MidiStandard::GM2 ? 2 : 1) : 0;
        ImGui::SetNextItemWidth(130);
        const char* maps[] = {"Normal part", "Drum map 1", "Drum map 2"};
        if (ImGui::Combo("Part mode", &map, maps, prof.family == MidiStandard::GM2 ? 2 : 3)) player_->setDrumPart(port, ch, map);
    }
    if (prof.hasEfx) {
        ImGui::SameLine(0, 16);
        bool efx = c.efxAssign;
        if (ImGui::Checkbox("Insertion EFX", &efx)) player_->setEfxAssign(port, ch, efx);
    }
    ImGui::SameLine(0, 16);
    if (ImGui::Button("Unlock channel")) player_->unlock(port, ch);
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("Locked values (orange outline) override the file until unlocked.");
    ImGui::EndChild();
}

// ---------------------------------------------------------------- sound edit matrix

void App::drawSoundEdit() {
    soundEditPort_ = portSelector("sep", soundEditPort_);
    int port = std::clamp(soundEditPort_, 0, snap_->numPorts - 1);
    ImGui::TextDisabled("All 16 parts at once. Values are sent as NRPN (GS/XG) or GM2 sound controllers depending on the target device.");
    struct Row {
        const char* name;
        int kind;  // 0 = CC, 1 = sound param
        int index;
        bool centered;
        int def;
    };
    const Row rows[] = {
        {"Volume", 0, 7, false, 100},        {"Expression", 0, 11, false, 127},  {"Pan", 0, 10, true, 64},
        {"Reverb", 0, 91, false, 40},        {"Chorus", 0, 93, false, 0},        {"Delay / Var.", 0, 94, false, 0},
        {"Modulation", 0, 1, false, 0},      {"Portamento T.", 0, 5, false, 0},  {"Attack", 1, int(SoundParam::Attack), true, 64},
        {"Decay", 1, int(SoundParam::Decay), true, 64},        {"Release", 1, int(SoundParam::Release), true, 64},
        {"Vibrato Rate", 1, int(SoundParam::VibRate), true, 64}, {"Vibrato Depth", 1, int(SoundParam::VibDepth), true, 64},
        {"Vibrato Delay", 1, int(SoundParam::VibDelay), true, 64}, {"Cutoff", 1, int(SoundParam::Cutoff), true, 64},
        {"Resonance", 1, int(SoundParam::Resonance), true, 64}, {"HPF Cutoff", 1, int(SoundParam::HpfCutoff), true, 64},
        {"EQ Bass Gain", 1, int(SoundParam::EqBassGain), true, 64}, {"EQ Treble Gain", 1, int(SoundParam::EqTrebleGain), true, 64},
        {"EQ Bass Freq", 1, int(SoundParam::EqBassFreq), false, 0x0C}, {"EQ Treble Freq", 1, int(SoundParam::EqTrebleFreq), false, 0x36},
    };
    if (!ImGui::BeginTable("##soundedit", 17, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchSame,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, 120 * uiScale_);
    for (int ch = 0; ch < 16; ch++) {
        char h[8];
        snprintf(h, sizeof h, "%d", ch + 1);
        ImGui::TableSetupColumn(h);
    }
    ImGui::TableHeadersRow();
    float barH = ImGui::GetFrameHeight();
    for (int r = 0; r < int(sizeof rows / sizeof rows[0]); r++) {
        const Row& row = rows[r];
        bool supported = row.kind == 0 || player_->soundParamSupported(SoundParam(row.index));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (supported) ImGui::TextUnformatted(row.name);
        else ImGui::TextDisabled("%s", row.name);
        for (int ch = 0; ch < 16; ch++) {
            ImGui::TableNextColumn();
            const ChannelState& c = snap_->ports[port].ch[ch];
            const ChannelLocks& L = snap_->locks[port][ch];
            int v = row.kind == 0 ? c.cc[row.index] : c.sound[row.index];
            bool locked = row.kind == 0 ? L.cc[size_t(row.index)] : ((L.sound >> row.index) & 1);
            char text[12];
            if (row.centered) snprintf(text, sizeof text, "%+d", v - 64);
            else snprintf(text, sizeof text, "%d", v);
            ImGui::PushID(r * 16 + ch);
            ui::BarResult res = ui::ValueBar("m", &v, 0, 127, row.def, ImVec2(ImGui::GetContentRegionAvail().x, barH), text,
                                             row.kind == 0 ? kVolColor : kSndColor, locked, row.centered, supported);
            if (res.changed) {
                if (row.kind == 0) player_->setChannelCC(port, ch, row.index, v, true);
                else player_->setSoundParam(port, ch, SoundParam(row.index), v, true);
            }
            if (res.rightClicked) {
                if (row.kind == 0) player_->unlockCC(port, ch, row.index);
                else player_->unlock(port, ch);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

} // namespace immidi

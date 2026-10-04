#include "App.h"
#include "Util.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace immidi {

namespace {

const ImU32 kFxColor = IM_COL32(110, 150, 230, 255);

// Labelled value bar; returns true when changed.
bool paramBar(const char* label, int* v, int vmin, int vmax, int def, const char* text = nullptr, bool centered = false, float width = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(150);
    char buf[32];
    if (!text) {
        snprintf(buf, sizeof buf, "%d", *v);
        text = buf;
    }
    float w = width > 0 ? width : std::min(260.0f, ImGui::GetContentRegionAvail().x);
    ui::BarResult r = ui::ValueBar("v", v, vmin, vmax, def, ImVec2(w, ImGui::GetFrameHeight()), text, kFxColor, false, centered);
    ImGui::PopID();
    return r.changed;
}

int findNamed(const std::vector<NamedType>& list, uint8_t msb, uint8_t lsb) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].msb == msb && list[i].lsb == lsb) return int(i);
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].msb == msb && list[i].lsb == 0) return int(i);
    return -1;
}

bool namedCombo(const char* label, const std::vector<NamedType>& list, uint8_t msb, uint8_t lsb, int* out) {
    int cur = findNamed(list, msb, lsb);
    std::string preview = cur >= 0 ? list[size_t(cur)].name : xgEffectName(msb, lsb);
    bool changed = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(150);
    ImGui::SetNextItemWidth(260);
    ImGui::PushID(label);
    if (ImGui::BeginCombo("##c", preview.c_str())) {
        for (size_t i = 0; i < list.size(); i++) {
            char l[64];
            snprintf(l, sizeof l, "%s  (%02X %02X)", list[i].name, list[i].msb, list[i].lsb);
            if (ImGui::Selectable(l, int(i) == cur)) {
                *out = int(i);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

std::string signedText(int v, int center) {
    char b[16];
    snprintf(b, sizeof b, "%+d", v - center);
    return b;
}

} // namespace

void App::drawEffects() {
    effectsPort_ = portSelector("fxport", effectsPort_);
    int port = std::clamp(effectsPort_, 0, snap_->numPorts - 1);
    const DeviceProfile& prof = profile();
    ImGui::TextDisabled("Target: %s. Changes are sent immediately to the device on port %c; the song may overwrite them later.", prof.name, 'A' + port);
    ImGui::BeginChild("##fx", ImVec2(0, 0), ImGuiChildFlags_None);
    drawMasterEffects(port);
    switch (prof.family) {
    case MidiStandard::GS: drawGsEffects(port); break;
    case MidiStandard::XG: drawXgEffects(port); break;
    case MidiStandard::GM2: drawGm2Effects(port); break;
    default: ImGui::TextDisabled("General MIDI level 1 has no effect controls."); break;
    }
    ImGui::EndChild();
}

void App::drawMasterEffects(int port) {
    const PortState& ps = snap_->ports[port];
    const DeviceProfile& prof = profile();
    if (!ImGui::CollapsingHeader("Master", ImGuiTreeNodeFlags_DefaultOpen)) return;
    int vol = ps.masterVolume >> 7;
    if (paramBar("Master volume", &vol, 0, 127, 127)) player_->userSend(port, masterVolumeSysex(uint16_t(vol == 127 ? 16383 : vol << 7)));
    if (prof.family == MidiStandard::GS || prof.family == MidiStandard::XG) {
        int ks = ps.masterKeyShift + 64;
        if (paramBar("Master key shift", &ks, 0x28, 0x58, 64, signedText(ks, 64).c_str(), true)) {
            if (prof.family == MidiStandard::GS) player_->userSend(port, gsSysex(0x400005, {uint8_t(ks)}));
            else player_->userSend(port, xgSysex(0x00, 0x00, 0x06, {uint8_t(ks)}));
        }
        ImGui::SameLine();
        ui::HelpMarker("Device-level key shift (also transposes drums). The Transpose control in the toolbar skips drum parts.");
    }
    if (prof.family == MidiStandard::GS) {
        int pan = ps.masterPan;
        char t[8];
        snprintf(t, sizeof t, "%+d", pan - 64);
        if (paramBar("Master pan", &pan, 1, 127, 64, t, true)) player_->userSend(port, gsSysex(0x400006, {uint8_t(pan)}));
    }
    if (prof.family != MidiStandard::GM) {
        // 0.1 cent steps. GS: +-100.0 cents, XG: -102.4 to +102.3, GM2 (universal master fine tuning): +-100.
        int lo = prof.family == MidiStandard::XG ? -1024 : -1000, hi = prof.family == MidiStandard::XG ? 1023 : 1000;
        int tune = std::clamp(ps.masterTune, lo, hi);
        char t[48];
        snprintf(t, sizeof t, "%+.1f cents  (A = %.2f Hz)", tune / 10.0, 440.0 * std::pow(2.0, tune / 12000.0));
        if (paramBar("Master tune", &tune, lo, hi, 0, t, true)) {
            if (prof.family == MidiStandard::GS || prof.family == MidiStandard::XG) {
                int raw = std::clamp(tune + 0x400, 0, 0x7FF);
                uint8_t nib[4] = {uint8_t((raw >> 12) & 0xF), uint8_t((raw >> 8) & 0xF), uint8_t((raw >> 4) & 0xF), uint8_t(raw & 0xF)};
                if (prof.family == MidiStandard::GS) player_->userSend(port, gsSysex(0x400000, nib, 4));
                else player_->userSend(port, xgSysex(0x00, 0x00, 0x00, nib, 4));
            } else {
                player_->userSend(port, masterFineTuningSysex(tune));
            }
        }
        ImGui::SameLine();
        ui::HelpMarker("Fine tuning of the whole device in 0.1 cent steps (GS/XG MASTER TUNE, GM2 Master Fine Tuning).\n"
                       "Double-click to go back to A = 440 Hz.");
    }
}

void App::drawGsEffects(int port) {
    const PortState& ps = snap_->ports[port];
    const DeviceProfile& prof = profile();
    auto send = [&](uint32_t addr, int v) { player_->userSend(port, gsSysex(addr, {uint8_t(std::clamp(v, 0, 127))})); };

    bool twoCols = ImGui::GetContentRegionAvail().x > 900;
    if (twoCols) {
        ImGui::BeginTable("##gscols", 2, ImGuiTableFlags_SizingStretchSame);
        ImGui::TableNextColumn();
    }
    if (ImGui::CollapsingHeader("Reverb", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("gs_reverb");
        int macro = ps.gsReverb[0];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Reverb type");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("##revmacro", &macro, kGsReverbMacros, 8)) send(0x400130, macro);
        int v;
        v = ps.gsReverb[1];
        if (paramBar("Character", &v, 0, 7, 4)) send(0x400131, v);
        v = ps.gsReverb[2];
        if (paramBar("Pre-LPF", &v, 0, 7, 0)) send(0x400132, v);
        v = ps.gsReverb[3];
        if (paramBar("Level", &v, 0, 127, 64)) send(0x400133, v);
        v = ps.gsReverb[4];
        if (paramBar("Time", &v, 0, 127, 64)) send(0x400134, v);
        v = ps.gsReverb[5];
        if (paramBar("Delay feedback", &v, 0, 127, 0)) send(0x400135, v);
        if (prof.hasDelay) {
            v = ps.gsReverb[7];
            char t[16];
            snprintf(t, sizeof t, "%d ms", v);
            if (paramBar("Pre-delay time", &v, 0, 127, 0, t)) send(0x400137, v);
        }
        ImGui::PopID();
    }
    if (twoCols) ImGui::TableNextColumn();
    if (ImGui::CollapsingHeader("Chorus", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("gs_chorus");
        int macro = ps.gsChorus[0];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Chorus type");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("##chomacro", &macro, kGsChorusMacros, 8)) send(0x400138, macro);
        const char* names[8] = {"Pre-LPF", "Level", "Feedback", "Delay", "Rate", "Depth", "Send to reverb", "Send to delay"};
        const int defs[8] = {0, 64, 8, 80, 3, 19, 0, 0};
        const int maxv[8] = {7, 127, 127, 127, 127, 127, 127, 127};
        for (int i = 0; i < 8; i++) {
            if (i == 7 && !prof.hasDelay) break;
            int v = ps.gsChorus[i + 1];
            if (paramBar(names[i], &v, 0, maxv[i], defs[i])) send(0x400139 + uint32_t(i), v);
        }
        ImGui::PopID();
    }
    if (twoCols) ImGui::TableNextColumn();
    if (prof.hasDelay && ImGui::CollapsingHeader("Delay (SC-88 and later)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("gs_delay");
        int macro = ps.gsDelay[0];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Delay type");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("##dlymacro", &macro, kGsDelayMacros, 10)) send(0x400150, macro);
        struct P {
            const char* n;
            int lo, hi, def;
        } params[10] = {{"Pre-LPF", 0, 7, 0},        {"Time center", 1, 0x73, 0x61}, {"Time ratio L", 1, 0x78, 1}, {"Time ratio R", 1, 0x78, 1},
                        {"Level center", 0, 127, 127}, {"Level left", 0, 127, 0},    {"Level right", 0, 127, 0},   {"Level", 0, 127, 64},
                        {"Feedback", 0, 127, 0x50},    {"Send to reverb", 0, 127, 0}};
        for (int i = 0; i < 10; i++) {
            int v = ps.gsDelay[i + 1];
            std::string t = i == 8 ? signedText(v, 64) : std::to_string(v);
            if (paramBar(params[i].n, &v, params[i].lo, params[i].hi, params[i].def, t.c_str(), i == 8)) send(0x400151 + uint32_t(i), v);
        }
        ImGui::PopID();
    }
    if (twoCols) ImGui::TableNextColumn();
    if (prof.hasGsEq && ImGui::CollapsingHeader("Equalizer (SC-88 and later)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("gs_eq");
        int lf = ps.gsEq[0], hf = ps.gsEq[2];
        const char* lfs[] = {"200 Hz", "400 Hz"};
        const char* hfs[] = {"3 kHz", "6 kHz"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Low frequency");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("##eqlf", &lf, lfs, 2)) send(0x400200, lf);
        int v = ps.gsEq[1];
        if (paramBar("Low gain", &v, 0x34, 0x4C, 64, (signedText(v, 64) + " dB").c_str(), true)) send(0x400201, v);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("High frequency");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("##eqhf", &hf, hfs, 2)) send(0x400202, hf);
        v = ps.gsEq[3];
        if (paramBar("High gain", &v, 0x34, 0x4C, 64, (signedText(v, 64) + " dB").c_str(), true)) send(0x400203, v);
        ImGui::PopID();
    }
    if (twoCols) ImGui::EndTable();
    if (prof.hasEfx && ImGui::CollapsingHeader("Insertion effect (EFX, SC-88Pro / SC-8850)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("gs_efx");
        int cur = gsEfxFindType(ps.gsEfxMsb, ps.gsEfxLsb);
        char preview[96];
        if (cur >= 0) snprintf(preview, sizeof preview, "%d: %s", cur, gsEfxType(cur).name);
        else snprintf(preview, sizeof preview, "Unknown (%02X %02X)", ps.gsEfxMsb, ps.gsEfxLsb);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("EFX type");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("##efxtype", preview, ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < gsEfxTypeCount(); i++) {
                const GsEfxTypeDef& t = gsEfxType(i);
                char l[96];
                snprintf(l, sizeof l, "%2d: %s  (%02X %02X)", i, t.name, t.msb, t.lsb);
                if (ImGui::Selectable(l, i == cur)) player_->userSend(port, gsSysex(0x400300, {t.msb, t.lsb}));
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ui::HelpMarker("Selecting a type resets its parameters to the defaults (as the SC-88Pro does). "
                       "Parameters marked + / # can also be driven by EFX control source 1 / 2.");
        if (cur > 0) {
            const GsEfxTypeDef& t = gsEfxType(cur);
            if (ImGui::BeginTable("##efxparams", 2, ImGuiTableFlags_SizingStretchSame)) {
                for (int i = 0; i < t.paramCount; i++) {
                    const GsEfxParamDef& p = gsEfxParam(t.firstParam + i);
                    if (p.index < 1 || p.index > 20) continue;
                    ImGui::TableNextColumn();
                    int v = ps.gsEfxParam[p.index - 1];
                    char label[48];
                    snprintf(label, sizeof label, "%s%s [%d]", p.control == 1 ? "+" : p.control == 2 ? "#" : "", p.name, p.index);
                    uint32_t addr = 0x400302 + uint32_t(p.index);
                    if (p.kind == GsEfxKind::ENUM) {
                        auto opts = gsEfxEnumOptions(p);
                        std::string pv = gsEfxFormatValue(p, v);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(label);
                        ImGui::SameLine(150);
                        ImGui::SetNextItemWidth(std::min(200.0f, ImGui::GetContentRegionAvail().x));
                        ImGui::PushID(p.index);
                        if (ImGui::BeginCombo("##e", pv.c_str())) {
                            for (auto& o : opts)
                                if (ImGui::Selectable(o.second.c_str(), o.first == v)) send(addr, o.first);
                            ImGui::EndCombo();
                        }
                        ImGui::PopID();
                    } else {
                        std::string text = gsEfxFormatValue(p, v);
                        bool centered = p.kind == GsEfxKind::PAN || p.kind == GsEfxKind::BALANCE_DE || p.kind == GsEfxKind::BALANCE_AB ||
                                        (p.kind == GsEfxKind::LINEAR && p.dlo < 0);
                        int def = p.defaultValue >= 0 ? p.defaultValue : (centered ? (p.lo + p.hi + 1) / 2 : p.lo);
                        if (paramBar(label, &v, p.lo, p.hi, def, text.c_str(), centered, std::min(200.0f, ImGui::GetContentRegionAvail().x - 150)))
                            send(addr, v);
                    }
                }
                ImGui::EndTable();
            }
        }
        int v = ps.gsEfxSend[0];
        if (paramBar("EFX send to reverb", &v, 0, 127, 40)) send(0x400317, v);
        v = ps.gsEfxSend[1];
        if (paramBar("EFX send to chorus", &v, 0, 127, 0)) send(0x400318, v);
        v = ps.gsEfxSend[2];
        if (paramBar("EFX send to delay", &v, 0, 127, 0)) send(0x400319, v);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Parts using EFX");
        ImGui::SameLine(150);
        for (int ch = 0; ch < 16; ch++) {
            bool on = ps.ch[ch].efxAssign;
            char l[8];
            snprintf(l, sizeof l, "%d", ch + 1);
            if (ch) ImGui::SameLine(0, 2);
            if (ui::ToggleButton(l, on, ImVec2(28, 0), ui::ChannelColor(ch))) player_->setEfxAssign(port, ch, !on);
        }
        ImGui::PopID();
    }
}

void App::drawXgEffects(int port) {
    const PortState& ps = snap_->ports[port];
    auto send1 = [&](uint8_t hi, uint8_t mid, uint8_t lo, int v) { player_->userSend(port, xgSysex(hi, mid, lo, {uint8_t(std::clamp(v, 0, 127))})); };
    auto rawParams = [&](const char* id, int first, int count, uint8_t hi, uint8_t mid, uint8_t base, int stride) {
        if (!ImGui::TreeNode(id)) return;
        ImGui::TextDisabled("Raw data values (the meaning depends on the effect type, see the XG effect parameter list).");
        if (ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchSame)) {
            for (int i = 0; i < count; i++) {
                ImGui::TableNextColumn();
                uint8_t lo = uint8_t(base + i * stride);
                int v = stride == 2 ? ps.xgEffect[lo + 1] : ps.xgEffect[lo];
                char label[24];
                snprintf(label, sizeof label, "Param %d", first + i);
                ImGui::PushID(i);
                ImGui::TextUnformatted(label);
                ui::BarResult r = ui::ValueBar("p", &v, 0, 127, 64, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()),
                                               std::to_string(v).c_str(), kFxColor, false);
                if (r.changed) {
                    if (stride == 2) player_->userSend(port, xgSysex(hi, mid, lo, {ps.xgEffect[lo], uint8_t(v)}));
                    else send1(hi, mid, lo, v);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    };
    auto db = [](int v) {
        // XG return/send levels: 0 = -inf, 96 = 0 dB, 127 = +6 dB (approximate display)
        char b[24];
        if (v == 0) return std::string("-inf");
        double dbv = 20.0 * std::log10(double(v) / 96.0);
        snprintf(b, sizeof b, "%d (%+.1f dB)", v, dbv);
        return std::string(b);
    };

    bool twoCols = ImGui::GetContentRegionAvail().x > 900;
    if (twoCols) {
        ImGui::BeginTable("##xgcols", 2, ImGuiTableFlags_SizingStretchSame);
        ImGui::TableNextColumn();
    }
    if (ImGui::CollapsingHeader("Reverb", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("xg_reverb");
        int sel;
        if (namedCombo("Reverb type", xgReverbTypes(), ps.xgEffect[0x00], ps.xgEffect[0x01], &sel))
            player_->userSend(port, xgSysex(0x02, 0x01, 0x00, {xgReverbTypes()[size_t(sel)].msb, xgReverbTypes()[size_t(sel)].lsb}));
        int v = ps.xgEffect[0x0C];
        if (paramBar("Return", &v, 0, 127, 64, db(v).c_str())) send1(0x02, 0x01, 0x0C, v);
        v = ps.xgEffect[0x0D];
        if (paramBar("Pan", &v, 1, 127, 64, signedText(v, 64).c_str(), true)) send1(0x02, 0x01, 0x0D, v);
        rawParams("Reverb parameters 1-10", 1, 10, 0x02, 0x01, 0x02, 1);
        ImGui::PopID();
    }
    if (twoCols) ImGui::TableNextColumn();
    if (ImGui::CollapsingHeader("Chorus", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("xg_chorus");
        int sel;
        if (namedCombo("Chorus type", xgChorusTypes(), ps.xgEffect[0x20], ps.xgEffect[0x21], &sel))
            player_->userSend(port, xgSysex(0x02, 0x01, 0x20, {xgChorusTypes()[size_t(sel)].msb, xgChorusTypes()[size_t(sel)].lsb}));
        int v = ps.xgEffect[0x2C];
        if (paramBar("Return", &v, 0, 127, 64, db(v).c_str())) send1(0x02, 0x01, 0x2C, v);
        v = ps.xgEffect[0x2D];
        if (paramBar("Pan", &v, 1, 127, 64, signedText(v, 64).c_str(), true)) send1(0x02, 0x01, 0x2D, v);
        v = ps.xgEffect[0x2E];
        if (paramBar("Send to reverb", &v, 0, 127, 0, db(v).c_str())) send1(0x02, 0x01, 0x2E, v);
        rawParams("Chorus parameters 1-10", 1, 10, 0x02, 0x01, 0x22, 1);
        ImGui::PopID();
    }
    if (twoCols) ImGui::EndTable();
    if (ImGui::CollapsingHeader("Variation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("xg_variation");
        int sel;
        if (namedCombo("Variation type", xgVariationTypes(), ps.xgEffect[0x40], ps.xgEffect[0x41], &sel))
            player_->userSend(port, xgSysex(0x02, 0x01, 0x40, {xgVariationTypes()[size_t(sel)].msb, xgVariationTypes()[size_t(sel)].lsb}));
        int conn = ps.xgEffect[0x5A] ? 1 : 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Connection");
        ImGui::SameLine(150);
        if (ImGui::RadioButton("Insertion", conn == 0)) send1(0x02, 0x01, 0x5A, 0);
        ImGui::SameLine();
        if (ImGui::RadioButton("System", conn == 1)) send1(0x02, 0x01, 0x5A, 1);
        int part = ps.xgEffect[0x5B];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Insertion part");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(160);
        char pv[24];
        if (part >= 64) snprintf(pv, sizeof pv, "Off");
        else snprintf(pv, sizeof pv, "Part %c%d", 'A' + part / 16, part % 16 + 1);
        if (ImGui::BeginCombo("##varpart", pv)) {
            if (ImGui::Selectable("Off", part >= 64)) send1(0x02, 0x01, 0x5B, 127);
            for (int p = 0; p < 16 * std::max(1, snap_->numPorts); p++) {
                char l[24];
                snprintf(l, sizeof l, "Part %c%d", 'A' + p / 16, p % 16 + 1);
                if (ImGui::Selectable(l, part == p)) send1(0x02, 0x01, 0x5B, p);
            }
            ImGui::EndCombo();
        }
        int v = ps.xgEffect[0x56];
        if (paramBar("Return", &v, 0, 127, 64, db(v).c_str())) send1(0x02, 0x01, 0x56, v);
        v = ps.xgEffect[0x57];
        if (paramBar("Pan", &v, 1, 127, 64, signedText(v, 64).c_str(), true)) send1(0x02, 0x01, 0x57, v);
        v = ps.xgEffect[0x58];
        if (paramBar("Send to reverb", &v, 0, 127, 0, db(v).c_str())) send1(0x02, 0x01, 0x58, v);
        v = ps.xgEffect[0x59];
        if (paramBar("Send to chorus", &v, 0, 127, 0, db(v).c_str())) send1(0x02, 0x01, 0x59, v);
        rawParams("Variation parameters 1-10", 1, 10, 0x02, 0x01, 0x42, 2);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Insertion effects 1 / 2 (MU100 and later)")) {
        ImGui::PushID("xg_insertion");
        for (int n = 0; n < 2; n++) {
            ImGui::PushID(n);
            char l[32];
            snprintf(l, sizeof l, "Insertion %d type", n + 1);
            int sel;
            if (namedCombo(l, xgInsertionTypes(), ps.xgIns[n][0], ps.xgIns[n][1], &sel))
                player_->userSend(port, xgSysex(0x03, uint8_t(n), 0x00, {xgInsertionTypes()[size_t(sel)].msb, xgInsertionTypes()[size_t(sel)].lsb}));
            int part = ps.xgIns[n][0x0C];
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Part");
            ImGui::SameLine(150);
            ImGui::SetNextItemWidth(160);
            char pv[24];
            if (part >= 64) snprintf(pv, sizeof pv, "Off");
            else snprintf(pv, sizeof pv, "Part %c%d", 'A' + part / 16, part % 16 + 1);
            if (ImGui::BeginCombo("##inspart", pv)) {
                if (ImGui::Selectable("Off", part >= 64)) send1(0x03, uint8_t(n), 0x0C, 127);
                for (int p = 0; p < 16 * std::max(1, snap_->numPorts); p++) {
                    char pl[24];
                    snprintf(pl, sizeof pl, "Part %c%d", 'A' + p / 16, p % 16 + 1);
                    if (ImGui::Selectable(pl, part == p)) send1(0x03, uint8_t(n), 0x0C, p);
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Multi EQ")) {
        ImGui::PushID("xg_multieq");
        int t = std::min<int>(ps.xgEq[0], 4);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("EQ type");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("##eqtype", &t, kXgEqTypes, 5)) send1(0x02, 0x40, 0x00, t);
        const uint8_t gains[5] = {0x01, 0x05, 0x09, 0x0D, 0x11};
        for (int b = 0; b < 5; b++) {
            char l[24];
            snprintf(l, sizeof l, "Band %d gain", b + 1);
            int v = ps.xgEq[gains[b]];
            if (v == 0) v = 64;
            if (paramBar(l, &v, 0x34, 0x4C, 64, (signedText(v, 64) + " dB").c_str(), true)) send1(0x02, 0x40, gains[b], v);
        }
        ImGui::PopID();
    }
}

void App::drawGm2Effects(int port) {
    const PortState& ps = snap_->ports[port];
    if (ImGui::CollapsingHeader("Reverb (GM2 global parameter)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("gm2_gm2reverb");
        const int types[] = {0, 1, 2, 3, 4, 8};
        int cur = 0;
        for (int i = 0; i < 6; i++)
            if (types[i] == ps.gm2ReverbType) cur = i;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Reverb type");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##gm2rev", kGm2ReverbTypes[types[cur]])) {
            for (int i = 0; i < 6; i++)
                if (ImGui::Selectable(kGm2ReverbTypes[types[i]], i == cur)) player_->userSend(port, gm2GlobalReverb(0, uint8_t(types[i])));
            ImGui::EndCombo();
        }
        int v = ps.gm2ReverbTime;
        if (paramBar("Reverb time", &v, 0, 127, 64)) player_->userSend(port, gm2GlobalReverb(1, uint8_t(v)));
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Chorus (GM2 global parameter)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("gm2_gm2chorus");
        int t = std::min<int>(ps.gm2ChorusType, 5);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Chorus type");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("##gm2cho", &t, kGm2ChorusTypes, 6)) player_->userSend(port, gm2GlobalChorus(0, uint8_t(t)));
        int v = ps.gm2ChorusRate;
        if (paramBar("Mod rate", &v, 0, 127, 3)) player_->userSend(port, gm2GlobalChorus(1, uint8_t(v)));
        v = ps.gm2ChorusDepth;
        if (paramBar("Mod depth", &v, 0, 127, 19)) player_->userSend(port, gm2GlobalChorus(2, uint8_t(v)));
        v = ps.gm2ChorusFeedback;
        if (paramBar("Feedback", &v, 0, 127, 8)) player_->userSend(port, gm2GlobalChorus(3, uint8_t(v)));
        v = ps.gm2ChorusSendRev;
        if (paramBar("Send to reverb", &v, 0, 127, 0)) player_->userSend(port, gm2GlobalChorus(4, uint8_t(v)));
        ImGui::PopID();
    }
}

} // namespace immidi

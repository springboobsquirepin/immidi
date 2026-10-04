#include "App.h"
#include "Widgets.h"

#include <GLFW/glfw3.h>  // key codes

#include <algorithm>
#include <cstdio>

namespace immidi {

// The virtual keyboard: one channel played with the mouse, the computer keyboard (while the window
// has the focus) and a MIDI input (while the window is open).

namespace {

// Physical keys (GLFW key codes follow the US layout by position, so the piano layout stays the
// same on other keyboard layouts) and their offset from the octave's C, like a tracker or DAW:
// the lower row plays one octave, the row above it the next.
struct KeyNote {
    int key, offset;
};
const KeyNote kKeyNotes[] = {
    {GLFW_KEY_Z, 0},  {GLFW_KEY_S, 1},  {GLFW_KEY_X, 2},  {GLFW_KEY_D, 3},     {GLFW_KEY_C, 4},      {GLFW_KEY_V, 5},
    {GLFW_KEY_G, 6},  {GLFW_KEY_B, 7},  {GLFW_KEY_H, 8},  {GLFW_KEY_N, 9},     {GLFW_KEY_J, 10},     {GLFW_KEY_M, 11},
    {GLFW_KEY_COMMA, 12}, {GLFW_KEY_L, 13}, {GLFW_KEY_PERIOD, 14}, {GLFW_KEY_SEMICOLON, 15}, {GLFW_KEY_SLASH, 16},
    {GLFW_KEY_Q, 12}, {GLFW_KEY_2, 13}, {GLFW_KEY_W, 14}, {GLFW_KEY_3, 15},    {GLFW_KEY_E, 16},     {GLFW_KEY_R, 17},
    {GLFW_KEY_5, 18}, {GLFW_KEY_T, 19}, {GLFW_KEY_6, 20}, {GLFW_KEY_Y, 21},    {GLFW_KEY_7, 22},     {GLFW_KEY_U, 23},
    {GLFW_KEY_I, 24}, {GLFW_KEY_9, 25}, {GLFW_KEY_O, 26}, {GLFW_KEY_0, 27},    {GLFW_KEY_P, 28},     {GLFW_KEY_LEFT_BRACKET, 29},
    {GLFW_KEY_EQUAL, 30}, {GLFW_KEY_RIGHT_BRACKET, 31},
};

int keyOffset(int key) {
    for (const KeyNote& k : kKeyNotes)
        if (k.key == key) return k.offset;
    return -1;
}

std::string noteText(int n) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char b[16];
    snprintf(b, sizeof b, "%s%d", names[n % 12], n / 12 - 1);
    return b;
}

} // namespace

void App::vkNote(int note, bool on, int velocity) {
    if (note < 0 || note > 127) return;
    player_->previewNote(vkPort_, vkCh_, note, on ? std::clamp(velocity, 1, 127) : 0);
}

void App::vkReleaseAll() {
    for (int k = 0; k < 512; k++)
        if (vkKeyNote_[k] >= 0) {
            vkNote(vkKeyNote_[k], false, 0);
            vkKeyNote_[k] = -1;
        }
    if (vkMouseNote_ >= 0) {
        vkNote(vkMouseNote_, false, 0);
        vkMouseNote_ = -1;
    }
    if (vkSustain_) {
        vkSustain_ = false;
        player_->setChannelCC(vkPort_, vkCh_, 64, 0, false);
    }
}

void App::vkProgramStep(int delta) {
    int port = std::clamp(vkPort_, 0, std::max(0, snap_->numPorts - 1));
    const ChannelState& c = snap_->ports[port].ch[vkCh_];
    player_->setProgram(vkPort_, vkCh_, c.bankMsb, c.bankLsb, (c.program + delta + 128) % 128, true);
}

void App::vkSetInput(const std::string& name) {
    if (vkInput_ && vkInput_->isOpen()) {
        // Release whatever the input left sounding.
        uint32_t used = vkInChannels_.exchange(0);
        for (int ch = 0; ch < 16; ch++)
            if (used & (1u << ch)) {
                player_->userSend(vkInPort_.load(), Bytes{uint8_t(0xB0 | ch), 64, 0});
                player_->userSend(vkInPort_.load(), Bytes{uint8_t(0xB0 | ch), 123, 0});
            }
        vkInput_->close();
    }
    vkInputError_.clear();
    if (name.empty()) return;
    if (!vkInput_) vkInput_ = std::make_unique<MidiInput>();
    // Runs on the MIDI input thread: the player's methods are thread-safe, the routing is atomic.
    bool ok = vkInput_->open(name, [this](const uint8_t* m, size_t n) {
        uint8_t st = m[0];
        int port = vkInPort_.load(std::memory_order_relaxed);
        if (st >= 0x80 && st < 0xF0) {
            int ch = vkInChannel_.load(std::memory_order_relaxed);
            if (ch < 0) ch = st & 0x0F;
            Bytes b(m, m + n);
            b[0] = uint8_t((st & 0xF0) | ch);
            vkInChannels_.fetch_or(1u << ch);
            player_->userSend(port, b);
        } else if (st == 0xF0) {
            player_->userSend(port, Bytes(m, m + n));
        } else {
            return;
        }
        vkInActivity_.fetch_add(1, std::memory_order_relaxed);
        glfwPostEmptyEvent();  // show the notes even while the window is idling
    }, &vkInputError_);
    if (!ok && vkInputError_.empty()) vkInputError_ = "Could not open " + name;
}

void App::onKey(int key, int action, int mods) {
    if (!showVk_ || !vkFocused_ || key < 0 || key >= 512 || action == GLFW_REPEAT) return;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return;
    bool press = action == GLFW_PRESS;
    int off = keyOffset(key);
    if (off >= 0) {
        if (press) {
            if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER)) return;  // shortcuts, not notes
            if (vkKeyNote_[key] >= 0) return;
            int note = 12 * (vkOctave_ + 1) + off;
            if (note < 0 || note > 127) return;
            vkKeyNote_[key] = note;
            vkNote(note, true, vkVelocity_);
        } else if (vkKeyNote_[key] >= 0) {
            vkNote(vkKeyNote_[key], false, 0);  // the note that started, even if the octave changed since
            vkKeyNote_[key] = -1;
        }
        return;
    }
    if (key == GLFW_KEY_SPACE) {  // sustain pedal while held
        if (press != vkSustain_) {
            vkSustain_ = press;
            player_->setChannelCC(vkPort_, vkCh_, 64, press ? 127 : 0, false);
        }
        return;
    }
    if (!press) return;
    switch (key) {
    case GLFW_KEY_LEFT: vkOctave_ = std::max(-1, vkOctave_ - 1); break;
    case GLFW_KEY_RIGHT: vkOctave_ = std::min(8, vkOctave_ + 1); break;
    case GLFW_KEY_UP: vkVelocity_ = std::min(127, vkVelocity_ + 10); break;
    case GLFW_KEY_DOWN: vkVelocity_ = std::max(1, vkVelocity_ - 10); break;
    case GLFW_KEY_PAGE_UP: vkProgramStep(1); break;
    case GLFW_KEY_PAGE_DOWN: vkProgramStep(-1); break;
    default: break;
    }
}

void App::drawVirtualKeyboard() {
    if (!showVk_) {
        if (vkFocused_ || vkMouseNote_ >= 0) vkReleaseAll();
        vkFocused_ = false;
        if (vkInput_ && vkInput_->isOpen()) vkSetInput("");
        vkInputTried_ = false;
        return;
    }
    // Open the saved MIDI input once per showing of the window.
    if (!vkInputTried_) {
        vkInputTried_ = true;
        if (!vkInputName_.empty()) vkSetInput(vkInputName_);
    }
    // An input that is also one of the outputs (a loopback port such as "Midi Through") would send
    // everything straight back into ImMidi, forever.
    if (vkInput_ && vkInput_->isOpen()) {
        for (int p = 0; p < kMaxPorts; p++)
            if (outputs_->portDevice(p) == vkInput_->name()) {
                std::string n = vkInput_->name();
                vkSetInput("");
                vkInputError_ = n + " is also an output: its messages would loop back";
                break;
            }
    }
    const int ports = std::max(1, snap_->numPorts);
    vkPort_ = std::clamp(vkPort_, 0, ports - 1);
    vkInPort_.store(vkPort_);
    vkInChannel_.store(vkRemap_ ? vkCh_ : -1);

    ImGui::SetNextWindowSize(ImVec2(920 * uiScale_, 300 * uiScale_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Virtual keyboard", &showVk_)) {
        vkFocused_ = false;
        ImGui::End();
        return;
    }
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (vkFocused_ && !focused) {
        // Keys released while another window has the focus would never reach us.
        for (int k = 0; k < 512; k++)
            if (vkKeyNote_[k] >= 0) {
                vkNote(vkKeyNote_[k], false, 0);
                vkKeyNote_[k] = -1;
            }
        if (vkSustain_) {
            vkSustain_ = false;
            player_->setChannelCC(vkPort_, vkCh_, 64, 0, false);
        }
    }
    vkFocused_ = focused;
    const ChannelState& cs = snap_->ports[vkPort_].ch[vkCh_];

    // Row 1: where it plays and what.
    if (ports > 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60 * uiScale_);
        char cur[8];
        snprintf(cur, sizeof cur, "%c", char('A' + vkPort_));
        if (ImGui::BeginCombo("##vkport", cur)) {
            for (int p = 0; p < ports; p++) {
                snprintf(cur, sizeof cur, "%c", char('A' + p));
                if (ImGui::Selectable(cur, p == vkPort_) && p != vkPort_) {
                    vkReleaseAll();
                    vkPort_ = p;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Channel");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * uiScale_);
    char chLabel[16];
    snprintf(chLabel, sizeof chLabel, "%d%s", vkCh_ + 1, cs.drum ? " (drums)" : "");
    if (ImGui::BeginCombo("##vkch", chLabel)) {
        for (int c = 0; c < 16; c++) {
            char l[24];
            snprintf(l, sizeof l, "%d%s", c + 1, snap_->ports[vkPort_].ch[c].drum ? "  (drums)" : "");
            if (ImGui::Selectable(l, c == vkCh_) && c != vkCh_) {
                vkReleaseAll();
                vkCh_ = c;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Instrument");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##vkprev", ImGuiDir_Left)) vkProgramStep(-1);
    ImGui::SameLine();
    char prog[160];
    snprintf(prog, sizeof prog, "%03d %s###vkinst", cs.program + 1, instrumentName(vkPort_, vkCh_, nullptr).c_str());
    if (ImGui::Button(prog, ImVec2(220 * uiScale_, 0))) openInstrumentPicker(vkPort_, vkCh_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose the instrument (bank and program) of this channel");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##vknext", ImGuiDir_Right)) vkProgramStep(1);
    ImGui::SameLine();
    if (ImGui::Button("All notes off")) {
        vkReleaseAll();
        player_->userSend(vkPort_, Bytes{uint8_t(0xB0 | vkCh_), 120, 0});
        player_->userSend(vkPort_, Bytes{uint8_t(0xB0 | vkCh_), 123, 0});
    }

    // Row 2: how the computer keyboard and the mouse play.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Velocity");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * uiScale_);
    ImGui::SliderInt("##vkvel", &vkVelocity_, 1, 127);
    ImGui::SameLine();
    ImGui::Checkbox("Mouse: by click height", &vkMouseVelocityByPos_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clicking lower on a key plays louder (like the front of a real key)");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Octave");
    ImGui::SameLine();
    if (ImGui::Button("-##vkoct")) vkOctave_ = std::max(-1, vkOctave_ - 1);
    ImGui::SameLine();
    ImGui::Text("%s", noteText(12 * (vkOctave_ + 1)).c_str());
    ImGui::SameLine();
    if (ImGui::Button("+##vkoct")) vkOctave_ = std::min(8, vkOctave_ + 1);
    ImGui::SameLine();
    bool sustain = vkSustain_;
    if (ui::ToggleButton("Sustain", sustain, ImVec2(0, 0), IM_COL32(90, 170, 90, 255))) {
        vkSustain_ = !vkSustain_;
        player_->setChannelCC(vkPort_, vkCh_, 64, vkSustain_ ? 127 : 0, false);
    }

    // Row 3: MIDI input.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("MIDI input");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * uiScale_);
    std::string shown = vkInput_ && vkInput_->isOpen() ? vkInput_->name() : (vkInputName_.empty() ? std::string("(none)") : vkInputName_);
    if (ImGui::BeginCombo("##vkin", shown.c_str())) {
        if (ImGui::IsWindowAppearing()) vkInputList_ = MidiInput::availableDevices();
        if (ImGui::Selectable("(none)", vkInputName_.empty())) {
            vkInputName_.clear();
            vkSetInput("");
        }
        for (const std::string& n : vkInputList_)
            if (ImGui::Selectable(n.c_str(), n == vkInputName_)) {
                vkInputName_ = n;
                vkSetInput(n);
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    {
        // Activity light: lit for a moment after each incoming message.
        static uint32_t lastSeen = 0;
        static double litUntil = 0;
        uint32_t act = vkInActivity_.load();
        if (act != lastSeen) {
            lastSeen = act;
            litUntil = ImGui::GetTime() + 0.12;
        }
        bool open = vkInput_ && vkInput_->isOpen();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = ImGui::GetTextLineHeight() * 0.3f;
        ImU32 col = !open ? ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.4f)
                    : ImGui::GetTime() < litUntil ? IM_COL32(90, 230, 90, 255) : IM_COL32(40, 90, 40, 255);
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetFrameHeight() * 0.5f), r, col);
        ImGui::Dummy(ImVec2(r * 2 + 2, ImGui::GetFrameHeight()));
    }
    ImGui::SameLine();
    ImGui::Checkbox("Play on this channel", &vkRemap_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: everything from the input plays on the channel above.\nOff: the input's own channels are kept.");
    if (!vkInputError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", vkInputError_.c_str());
        ImGui::PopStyleColor();
    }

    // Wheels and keyboard.
    float helpH = ImGui::GetTextLineHeightWithSpacing();
    float kbH = std::max(60.0f * uiScale_, ImGui::GetContentRegionAvail().y - helpH - ImGui::GetStyle().ItemSpacing.y);
    float wheelW = 22 * uiScale_;
    if (ImGui::VSliderInt("##vkbend", ImVec2(wheelW, kbH), &vkBend_, 0, 16383, ""))
        player_->userSend(vkPort_, Bytes{uint8_t(0xE0 | vkCh_), uint8_t(vkBend_ & 0x7F), uint8_t(vkBend_ >> 7)});
    if (ImGui::IsItemDeactivated()) {  // springs back like a pitch wheel
        vkBend_ = 8192;
        player_->userSend(vkPort_, Bytes{uint8_t(0xE0 | vkCh_), 0x00, 0x40});
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pitch bend");
    ImGui::SameLine();
    if (ImGui::VSliderInt("##vkmod", ImVec2(wheelW, kbH), &vkMod_, 0, 127, "")) player_->setChannelCC(vkPort_, vkCh_, 1, vkMod_, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Modulation (CC#1): %d", vkMod_);
    ImGui::SameLine();
    int lo = std::clamp(12 * (vkOctave_ + 1) - 12, 0, 127), hi = std::min(127, lo + 60);
    ImU32 col = cs.drum ? IM_COL32(255, 150, 60, 255) : ui::ChannelColor(vkCh_);
    int held = vkMouseNote_;
    ui::KeyboardResult kr = ui::Keyboard("vkkb", cs.noteVel, cs.noteSus, lo, hi, ImVec2(ImGui::GetContentRegionAvail().x, kbH), col, &held);
    int vel = vkVelocity_;
    if (vkMouseVelocityByPos_) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float t = std::clamp((ImGui::GetIO().MousePos.y - a.y) / std::max(1.0f, b.y - a.y), 0.0f, 1.0f);
        vel = 20 + int(t * 107.0f);
    }
    if (kr.released >= 0) vkNote(kr.released, false, 0);
    if (kr.pressed >= 0) vkNote(kr.pressed, true, vel);
    vkMouseNote_ = held;
    if (kr.hovered >= 0) ImGui::SetTooltip("%s (%d)", noteText(kr.hovered).c_str(), kr.hovered);

    int base = 12 * (vkOctave_ + 1);
    ImGui::TextDisabled("Keys (click here first): Z S X D C V ... = %s-%s, Q 2 W 3 E ... = %s-%s   Left/Right octave   Up/Down velocity   "
                        "PgUp/PgDn program   Space sustain",
                        noteText(base).c_str(), noteText(std::min(127, base + 16)).c_str(), noteText(std::min(127, base + 12)).c_str(),
                        noteText(std::min(127, base + 31)).c_str());
    ImGui::End();
}

} // namespace immidi

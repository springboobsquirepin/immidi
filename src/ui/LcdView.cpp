#include "App.h"
#include "Renderer.h"
#include "Widgets.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace immidi {

// A display in the style of the Roland SC-55's LCD: the selected part's values on the left and the
// 16x16 dot matrix on the right. The matrix shows the level of the 16 parts, or the dot pictures a
// song sends (model 45h, 10 01 00); text messages (10 00 00) scroll through the instrument field.

namespace {

// How long a dot picture / a short text stays after the last display message. The SC-55 returns to
// its normal screen by itself after a few seconds; this is an approximation.
constexpr double kDotsHoldSeconds = 3.0;
constexpr double kTextHoldSeconds = 3.0;
constexpr double kTextScrollCharsPerSecond = 6.0;
constexpr int kInstrumentChars = 16;

// As the SC-55 shows it: L63..0..R63 (CC#10 0 and 1 are both the leftmost position), Rnd for a
// part pan set to random by SysEx.
std::string panText(int pan, bool random) {
    char b[8];
    if (random) return "Rnd";
    if (pan == 64) return "  0";
    if (pan < 64) snprintf(b, sizeof b, "L%2d", 64 - std::max(pan, 1));
    else snprintf(b, sizeof b, "R%2d", pan - 64);
    return b;
}

// Display color presets: the SC-55's orange LCD, the yellow-green LCD of Yamaha's MU modules, and
// other common LCD and display colors.
struct LcdPreset {
    const char* name;
    LcdColors colors;
};
const LcdPreset kLcdPresets[] = {
    {"Sound Canvas (orange)", {IM_COL32(255, 111, 15, 255), IM_COL32(0, 0, 0, 255), IM_COL32(200, 80, 0, 255)}},
    {"MU (yellow-green)", {IM_COL32(180, 200, 60, 255), IM_COL32(20, 30, 10, 255), IM_COL32(160, 180, 54, 255)}},
    {"Green", {IM_COL32(110, 170, 60, 255), IM_COL32(10, 30, 10, 255), IM_COL32(96, 150, 52, 255)}},
    {"Blue with white dots", {IM_COL32(30, 60, 200, 255), IM_COL32(230, 240, 255, 255), IM_COL32(44, 76, 212, 255)}},
    {"Gray", {IM_COL32(200, 205, 198, 255), IM_COL32(20, 22, 26, 255), IM_COL32(182, 187, 180, 255)}},
    {"Amber on black", {IM_COL32(20, 12, 0, 255), IM_COL32(255, 176, 0, 255), IM_COL32(52, 32, 0, 255)}},
    {"Red on black", {IM_COL32(18, 0, 0, 255), IM_COL32(255, 40, 30, 255), IM_COL32(56, 8, 6, 255)}},
    {"Fluorescent (cyan on black)", {IM_COL32(5, 15, 15, 255), IM_COL32(60, 255, 220, 255), IM_COL32(12, 46, 40, 255)}},
    {"White on black", {IM_COL32(0, 0, 0, 255), IM_COL32(235, 235, 235, 255), IM_COL32(30, 30, 30, 255)}},
};

struct MatrixPreset {
    const char* name;
    LcdMatrixColors colors;
};
const ImU32 kGreen = IM_COL32(40, 200, 70, 255), kRed = IM_COL32(255, 45, 30, 255), kBlue = IM_COL32(40, 90, 255, 255);
const MatrixPreset kMatrixPresets[] = {
    {"As the display", {LcdMatrixMode::Display, kGreen, kRed, kGreen, kRed}},
    {"Green, red top row", {LcdMatrixMode::TopRow, kGreen, kRed, kGreen, kRed}},
    {"Green, red top dots", {LcdMatrixMode::TopDot, kGreen, kRed, kGreen, kRed}},
    {"Green to red (meter)", {LcdMatrixMode::Gradient, kGreen, kRed, kGreen, kRed}},
    {"Cold to warm (blue to red)", {LcdMatrixMode::Gradient, kGreen, kRed, kBlue, kRed}},
    {"Cyan", {LcdMatrixMode::One, IM_COL32(60, 230, 255, 255), kRed, kGreen, kRed}},
};

const char* const kMatrixModeNames[int(LcdMatrixMode::Count)] = {"As the display", "One color", "Top row in its own color",
                                                                  "Top dot of each bar in its own color", "Gradient, bottom to top"};
const char* const kMatrixModeKeys[int(LcdMatrixMode::Count)] = {"display", "one", "toprow", "topdot", "gradient"};

bool sameColors(const LcdColors& a, const LcdColors& b) { return a.backlight == b.backlight && a.ink == b.ink && a.unlit == b.unlit; }

// The colors that matter in the mode.
bool sameMatrix(const LcdMatrixColors& a, const LcdMatrixColors& b) {
    if (a.mode != b.mode) return false;
    switch (a.mode) {
    case LcdMatrixMode::One: return a.lit == b.lit;
    case LcdMatrixMode::TopRow:
    case LcdMatrixMode::TopDot: return a.lit == b.lit && a.top == b.top;
    case LcdMatrixMode::Gradient: return a.low == b.low && a.high == b.high;
    default: return true;
    }
}

int displayPreset(const LcdColors& c) {
    for (int i = 0; i < int(IM_ARRAYSIZE(kLcdPresets)); i++)
        if (sameColors(kLcdPresets[i].colors, c)) return i;
    return -1;
}

int matrixPreset(const LcdMatrixColors& c) {
    for (int i = 0; i < int(IM_ARRAYSIZE(kMatrixPresets)); i++)
        if (sameMatrix(kMatrixPresets[i].colors, c)) return i;
    return -1;
}

// From `low` (t = 0) to `high` (t = 1) through the hues between them, so green to red passes yellow and
// blue to red passes cyan, green and yellow (cold to warm). A gray end takes the other end's hue.
ImU32 hueBlend(ImU32 low, ImU32 high, float t) {
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(low), b = ImGui::ColorConvertU32ToFloat4(high);
    float ha, sa, va, hb, sb, vb;
    ImGui::ColorConvertRGBtoHSV(a.x, a.y, a.z, ha, sa, va);
    ImGui::ColorConvertRGBtoHSV(b.x, b.y, b.z, hb, sb, vb);
    if (sa < 0.01f) ha = hb;
    if (sb < 0.01f) hb = ha;
    float r, g, bl;
    ImGui::ColorConvertHSVtoRGB(ha + (hb - ha) * t, sa + (sb - sa) * t, va + (vb - va) * t, r, g, bl);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, bl, 1.0f));
}

std::string colorText(ImU32 c) {
    char b[8];
    snprintf(b, sizeof b, "#%02X%02X%02X", unsigned(c & 0xFF), unsigned((c >> 8) & 0xFF), unsigned((c >> 16) & 0xFF));
    return b;
}

bool parseColor(const std::string& t, ImU32& out) {
    if (t.size() != 7 || t[0] != '#' || !std::all_of(t.begin() + 1, t.end(), [](char c) { return std::isxdigit(uint8_t(c)) != 0; }))
        return false;
    unsigned long v = std::strtoul(t.c_str() + 1, nullptr, 16);
    out = IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 255);
    return true;
}

bool colorEdit(const char* label, ImU32& c) {
    ImVec4 f = ImGui::ColorConvertU32ToFloat4(c);
    if (!ImGui::ColorEdit3(label, &f.x, ImGuiColorEditFlags_NoInputs)) return false;
    c = ImGui::ColorConvertFloat4ToU32(ImVec4(f.x, f.y, f.z, 1.0f));
    return true;
}

} // namespace

void App::loadLcdColors() {
    LcdColors c;
    parseColor(cfg_.get("lcd.backlight"), c.backlight);
    parseColor(cfg_.get("lcd.dots"), c.ink);
    parseColor(cfg_.get("lcd.unlit"), c.unlit);
    lcdColors_ = c;
    LcdMatrixColors m;
    std::string mode = cfg_.get("lcd.matrix", kMatrixModeKeys[0]);
    for (int i = 0; i < int(LcdMatrixMode::Count); i++)
        if (mode == kMatrixModeKeys[i]) m.mode = LcdMatrixMode(i);
    parseColor(cfg_.get("lcd.matrix.color"), m.lit);
    parseColor(cfg_.get("lcd.matrix.top"), m.top);
    parseColor(cfg_.get("lcd.matrix.bottom"), m.low);
    parseColor(cfg_.get("lcd.matrix.topEnd"), m.high);
    lcdMatrix_ = m;
}

void App::storeLcdColors() {
    cfg_.set("lcd.backlight", colorText(lcdColors_.backlight));
    cfg_.set("lcd.dots", colorText(lcdColors_.ink));
    cfg_.set("lcd.unlit", colorText(lcdColors_.unlit));
    cfg_.set("lcd.matrix", kMatrixModeKeys[int(lcdMatrix_.mode)]);
    cfg_.set("lcd.matrix.color", colorText(lcdMatrix_.lit));
    cfg_.set("lcd.matrix.top", colorText(lcdMatrix_.top));
    cfg_.set("lcd.matrix.bottom", colorText(lcdMatrix_.low));
    cfg_.set("lcd.matrix.topEnd", colorText(lcdMatrix_.high));
}

void App::lcdColorMenus() {
    if (ImGui::BeginMenu("Colors")) {
        int cur = displayPreset(lcdColors_);
        for (int i = 0; i < int(IM_ARRAYSIZE(kLcdPresets)); i++)
            if (ImGui::MenuItem(kLcdPresets[i].name, nullptr, i == cur)) lcdColors_ = kLcdPresets[i].colors;
        if (cur < 0) ImGui::MenuItem("Custom", nullptr, true, false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Level matrix colors")) {
        int cur = matrixPreset(lcdMatrix_);
        for (int i = 0; i < int(IM_ARRAYSIZE(kMatrixPresets)); i++)
            if (ImGui::MenuItem(kMatrixPresets[i].name, nullptr, i == cur)) lcdMatrix_ = kMatrixPresets[i].colors;
        if (cur < 0) ImGui::MenuItem("Custom", nullptr, true, false);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Choose colors in Settings...")) requestedTab_ = Tab::Settings;
}

void App::drawLcdColorSettings() {
    ImGui::SeparatorText("Sound Canvas display");
    ImGui::PushID("lcdcolors");
    int cur = displayPreset(lcdColors_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Colors");
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##preset", cur >= 0 ? kLcdPresets[cur].name : "Custom")) {
        for (int i = 0; i < int(IM_ARRAYSIZE(kLcdPresets)); i++)
            if (ImGui::Selectable(kLcdPresets[i].name, i == cur)) lcdColors_ = kLcdPresets[i].colors;
        ImGui::EndCombo();
    }
    ImGui::SetCursorPosX(200);
    colorEdit("Backlight", lcdColors_.backlight);
    ImGui::SameLine();
    colorEdit("Dots and characters", lcdColors_.ink);
    ImGui::SameLine();
    colorEdit("Unlit dots", lcdColors_.unlit);

    int mcur = matrixPreset(lcdMatrix_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Level matrix");
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##mpreset", mcur >= 0 ? kMatrixPresets[mcur].name : "Custom")) {
        for (int i = 0; i < int(IM_ARRAYSIZE(kMatrixPresets)); i++)
            if (ImGui::Selectable(kMatrixPresets[i].name, i == mcur)) lcdMatrix_ = kMatrixPresets[i].colors;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ui::HelpMarker("The lit dots of the 16 x 16 matrix: in the display's color, in one color of their own, with the top row "
                   "(or the top dot of each bar) in another color, or in a gradient from the bottom row to the top row "
                   "through the hues between the two colors (green to red passes yellow; blue to red goes from cold to warm). "
                   "Dot pictures sent by songs use the same colors.");
    ImGui::SetCursorPosX(200);
    int mode = int(lcdMatrix_.mode);
    ImGui::SetNextItemWidth(320);
    if (ImGui::Combo("##mode", &mode, kMatrixModeNames, int(LcdMatrixMode::Count))) lcdMatrix_.mode = LcdMatrixMode(mode);
    switch (lcdMatrix_.mode) {
    case LcdMatrixMode::One:
        ImGui::SetCursorPosX(200);
        colorEdit("Lit dots", lcdMatrix_.lit);
        break;
    case LcdMatrixMode::TopRow:
    case LcdMatrixMode::TopDot:
        ImGui::SetCursorPosX(200);
        colorEdit(lcdMatrix_.mode == LcdMatrixMode::TopRow ? "Top row" : "Top dots", lcdMatrix_.top);
        ImGui::SameLine();
        colorEdit("Other dots", lcdMatrix_.lit);
        break;
    case LcdMatrixMode::Gradient:
        ImGui::SetCursorPosX(200);
        colorEdit("Bottom row", lcdMatrix_.low);
        ImGui::SameLine();
        colorEdit("Top row", lcdMatrix_.high);
        break;
    default: break;
    }
    ImGui::PopID();
}

void App::drawLcdWindow() {
    if (!showLcd_) return;
    // The display is designed in "units" = pixels of the Nuked SC-55 emulator window: 724 x 300,
    // matrix cells 24 x 9 with 2-unit gaps like the SC-55's wide LCD segments. That is the native
    // (largest) size. It is drawn into an offscreen texture at native resolution, or a whole multiple
    // of it on high-DPI screens, and shown scaled with bilinear filtering. The texture is never more
    // than twice the size it is shown at, so every screen pixel blends the texels it covers and all
    // cells come out alike, instead of rounding cells to different pixel sizes.
    const float sizes[3] = {0.5f, 0.75f, 1.0f};
    const int sizeIdx = std::clamp(lcdSize_, 0, 2);
    const float screenU = sizes[sizeIdx] * uiScale_;  // ImGui units per display unit
    const float fb = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    const int k = std::max(1, int(std::ceil(screenU * fb - 0.001f)));  // texture pixels per unit
    const ImVec2 native(724, 300);
    const ImVec2 size(native.x * screenU, native.y * screenU);

    ImGui::SetNextWindowSize(ImVec2(0, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (lcdShownSize_.x > 0 && (size.x != lcdShownSize_.x || size.y != lcdShownSize_.y)) {
        // The display changed size (its size setting, the UI scale): keep the grown window on screen.
        // The window is placed before it is drawn; otherwise it stays where it is put, also partly
        // past an edge (moving it after Begin would leave its frame and its contents apart).
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 ws(lcdWinSize_.x + size.x - lcdShownSize_.x, lcdWinSize_.y + size.y - lcdShownSize_.y), np = lcdWinPos_;
        np.x = std::max(std::min(np.x, vp->WorkPos.x + vp->WorkSize.x - ws.x), vp->WorkPos.x);
        np.y = std::max(std::min(np.y, vp->WorkPos.y + vp->WorkSize.y - ws.y), vp->WorkPos.y);
        if (np.x != lcdWinPos_.x || np.y != lcdWinPos_.y) ImGui::SetNextWindowPos(np);
    }
    if (!ImGui::Begin("Sound Canvas display", &showLcd_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    lcdWinPos_ = ImGui::GetWindowPos();
    lcdWinSize_ = ImGui::GetWindowSize();
    lcdShownSize_ = size;
    int port = std::clamp(selPort_, 0, std::max(0, snap_->numPorts - 1));
    int ch = std::clamp(selCh_, 0, 15);
    const PortState& ps = snap_->ports[port];
    const ChannelState& cs = ps.ch[ch];
    double now = ImGui::GetTime();
    if (port != lcdPortSeen_) {
        lcdPortSeen_ = port;
        lcdDotsSeen_ = ps.lcdDotsSerial;
        lcdTextSeen_ = ps.lcdTextSerial;
        lcdDotsUntil_ = 0;
        lcdTextStart_ = -1;
    }
    if (ps.lcdDotsSerial != lcdDotsSeen_) {
        lcdDotsSeen_ = ps.lcdDotsSerial;
        lcdDotsUntil_ = ps.lcdShownPage ? now + kDotsHoldSeconds : 0;
    }
    if (ps.lcdTextSerial != lcdTextSeen_) {
        lcdTextSeen_ = ps.lcdTextSerial;
        lcdTextStart_ = ps.lcdText[0] ? now : -1;
    }

    // Contents of the display at `u` pixels per unit, with the top left corner at p0.
    char buf[64];
    snprintf(buf, sizeof buf, "%c%02d", snap_->numPorts > 1 ? char('A' + port) : ' ', ch + 1);
    const std::string partText = buf;
    std::string inst;
    if (lcdTextStart_ >= 0) {
        // Text message: scrolls through the instrument field when it is longer than the field.
        std::string msg = ps.lcdText;
        double t = now - lcdTextStart_;
        if (int(msg.size()) <= kInstrumentChars) {
            inst = msg;
            if (t > kTextHoldSeconds) lcdTextStart_ = -1;
        } else {
            std::string track = std::string(kInstrumentChars, ' ') + msg + std::string(kInstrumentChars, ' ');
            int off = int(t * kTextScrollCharsPerSecond);
            if (off + kInstrumentChars > int(track.size())) lcdTextStart_ = -1;
            else inst = track.substr(size_t(off), size_t(kInstrumentChars));
        }
    }
    if (lcdTextStart_ < 0) {
        char num[8];
        snprintf(num, sizeof num, "%03d ", cs.program + 1);
        inst = num + instrumentName(port, ch, nullptr);
        if (int(inst.size()) > kInstrumentChars + 4) inst.resize(size_t(kInstrumentChars + 4));
    }
    uint16_t rows[16] = {};
    bool picture = ps.lcdShownPage >= 1 && ps.lcdShownPage <= 10 && now < lcdDotsUntil_;
    if (picture) {
        std::memcpy(rows, ps.lcdPage[ps.lcdShownPage - 1], sizeof rows);
    } else {
        // Level bars of the 16 parts, from the bottom up. The bottom dot is always lit: on the SC-55 it is a part's
        // idle state, shown whether or not the part plays.
        for (int c = 0; c < 16; c++) {
            int h = std::clamp(int(std::lround(meter_[port][c] * 16.0f)), 1, 16);
            for (int r = 16 - h; r < 16; r++) rows[r] |= uint16_t(1u << (15 - c));
        }
    }
    // Lit dots of the matrix: per row (top row, gradient) or per dot (each bar's top dot).
    ImU32 rowColor[16];
    int topDot[16];
    for (int r = 0; r < 16; r++) {
        const LcdMatrixColors& m = lcdMatrix_;
        switch (m.mode) {
        case LcdMatrixMode::One: rowColor[r] = m.lit; break;
        case LcdMatrixMode::TopRow: rowColor[r] = r == 0 ? m.top : m.lit; break;
        case LcdMatrixMode::TopDot: rowColor[r] = m.lit; break;
        case LcdMatrixMode::Gradient: rowColor[r] = hueBlend(m.low, m.high, float(15 - r) / 15.0f); break;
        default: rowColor[r] = lcdColors_.ink; break;
        }
    }
    for (int c = 0; c < 16; c++) {
        topDot[c] = -1;
        for (int r = 0; r < 16 && topDot[c] < 0; r++)
            if ((rows[r] >> (15 - c)) & 1) topDot[c] = r;
        if (!picture && topDot[c] == 15) topDot[c] = -1;  // only the idle dot: no level to mark
    }
    auto paint = [&](ImDrawList* dl, ImVec2 p0, float u) {
        const ImU32 bg = lcdColors_.backlight;
        const ImU32 ink = lcdColors_.ink;      // lit segments and characters
        const ImU32 ghost = lcdColors_.unlit;  // unlit cells
        const float cellW = 24 * u, cellH = 9 * u, pitchX = 26 * u, pitchY = 11 * u;
        ImFont* font = ImGui::GetFont();
        auto text = [&](float x, float y, float px, ImU32 col, const std::string& t) { dl->AddText(font, px * u, ImVec2(p0.x + x * u, p0.y + y * u), col, t.c_str()); };
        // Printed legend and value below it, as on the SC-55: LEVEL/PAN, REVERB/CHORUS, K SHIFT/MIDI CH.
        auto field = [&](float x, float y, const char* name, const std::string& value) {
            text(x, y, 19, ink, name);
            text(x, y + 20, 40, ink, value);
        };
        dl->AddRectFilled(p0, ImVec2(p0.x + native.x * u, p0.y + native.y * u), bg);
        text(30, 34, 44, ink, partText);
        text(150, 34, 44, ink, inst);
        char v[16];
        snprintf(v, sizeof v, "%3d", cs.cc[7]);
        field(35, 86, "LEVEL", v);
        field(155, 86, "PAN", panText(cs.cc[10], cs.randomPan));
        snprintf(v, sizeof v, "%3d", cs.cc[91]);
        field(35, 150, "REVERB", v);
        snprintf(v, sizeof v, "%3d", cs.cc[93]);
        field(155, 150, "CHORUS", v);
        snprintf(v, sizeof v, "%+3d", int(cs.keyShift));
        field(35, 210, "K SHIFT", v);
        snprintf(v, sizeof v, "%c%02d", char('A' + port), ch + 1);
        field(155, 210, "MIDI CH", v);

        // The 16 x 16 matrix with the L/R scale on its left and the part numbers below.
        ImVec2 m0(p0.x + 293 * u, p0.y + 101 * u);
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++) {
                ImVec2 a(m0.x + c * pitchX, m0.y + r * pitchY);
                ImU32 lit = lcdMatrix_.mode == LcdMatrixMode::TopDot && topDot[c] == r ? lcdMatrix_.top : rowColor[r];
                dl->AddRectFilled(a, ImVec2(a.x + cellW, a.y + cellH), (rows[r] >> (15 - c)) & 1 ? lit : ghost);
            }
        text(262, 94, 20, ghost, "L");
        text(262, 258, 20, ghost, "R");
        for (int r = 0; r < 16; r++) {
            // Large marks at the ends and the centre of the scale, small ones in between.
            bool big = r == 0 || r == 8 || r == 15;
            dl->AddCircleFilled(ImVec2(p0.x + 288 * u, m0.y + r * pitchY + cellH * 0.5f), (big ? 3.0f : 1.5f) * u, ink);
        }
        for (int c = 0; c < 16; c++) {
            char num[4];
            snprintf(num, sizeof num, "%d", c + 1);
            float tw = font->CalcTextSizeA(20 * u, FLT_MAX, 0.0f, num).x;
            dl->AddText(font, 20 * u, ImVec2(m0.x + c * pitchX + (cellW - tw) * 0.5f, m0.y + 16 * pitchY + 4 * u), ink, num);
        }
    };

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##lcd", size);
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    // Offscreen copy at k pixels per unit, rendered after this frame (renderOffscreen) and shown
    // from the previous frame's texture.
    if (!lcdList_) lcdList_ = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    lcdTexW_ = int(native.x) * k;
    lcdTexH_ = int(native.y) * k;
    lcdList_->_ResetForNewFrame();
    lcdList_->Flags = ImGui::GetWindowDrawList()->Flags;
    lcdList_->PushTexture(ImGui::GetIO().Fonts->TexRef);
    lcdList_->PushClipRect(ImVec2(0, 0), ImVec2(float(lcdTexW_), float(lcdTexH_)));
    paint(lcdList_, ImVec2(0, 0), float(k));
    lcdPending_ = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (lcdTarget_ && lcdTarget_->texture() && lcdTarget_->width() == lcdTexW_ && lcdTarget_->height() == lcdTexH_)
        lcdTarget_->draw(dl, p0, p1);
    else
        paint(dl, p0, screenU);  // first frame, or no framebuffer objects: draw directly

    // Clicking a column of the level display selects that part.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        float ux = (mp.x - p0.x) / screenU - 293, uy = (mp.y - p0.y) / screenU - 101;
        int c = int(std::floor(ux / 26));
        if (c >= 0 && c < 16 && uy >= 0 && uy < 16 * 11 + 24) selCh_ = c;
    }
    if (ImGui::BeginPopupContextItem("##lcdctx")) {
        const char* names[3] = {"Small (50%)", "Medium (75%)", "Large (100%, native 724 x 300)"};
        for (int i = 0; i < 3; i++)
            if (ImGui::MenuItem(names[i], nullptr, lcdSize_ == i)) lcdSize_ = i;
        ImGui::Separator();
        lcdColorMenus();
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered() && !picture) ImGui::SetItemTooltip("Click a column to select that part. Right-click for the size and colors.");
    ImGui::End();
}

void App::renderOffscreen() {
    if (!lcdPending_ || !lcdList_) return;
    lcdPending_ = false;
    if (!lcdTarget_ && activeRenderer()) lcdTarget_ = activeRenderer()->createOffscreenTarget();
    if (!lcdTarget_) return;
    lcdTarget_->render(lcdList_, lcdTexW_, lcdTexH_);
}

} // namespace immidi

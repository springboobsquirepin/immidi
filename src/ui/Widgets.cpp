#include "Widgets.h"
#include "Renderer.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>
#include <cstdio>

namespace ui {

ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t, ca.z + (cb.z - ca.z) * t,
                                                 ca.w + (cb.w - ca.w) * t));
}

ImU32 ChannelColor(int ch, float alpha) {
    float h = std::fmod(float(ch) * 0.61803398875f, 1.0f);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, 0.55f, 0.95f, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
}

BarResult ValueBar(const char* id, int* v, int vmin, int vmax, int def, ImVec2 size, const char* text, ImU32 color, bool locked,
                   bool centered, bool enabled) {
    BarResult res;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return res;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    ImGui::InvisibleButton("##bar", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImGui::EndDisabled();
    res.hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(pos.x + size.x, pos.y + size.y);
    int range = std::max(1, vmax - vmin);

    if (enabled) {
        // After a double-click resets the value, the second press must not drag it away again:
        // dragging stays off for that bar until the button is released.
        static ImGuiID resetLatch = 0;
        ImGuiID itemId = ImGui::GetItemID();
        if (resetLatch == itemId && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) resetLatch = 0;
        if (res.hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            resetLatch = itemId;
            if (*v != def) {
                *v = def;
                res.changed = true;
            }
        }
        if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && resetLatch != itemId) {
            float t = (ImGui::GetIO().MousePos.x - pos.x) / std::max(1.0f, size.x);
            t = std::clamp(t, 0.0f, 1.0f);
            int nv = vmin + int(std::lround(t * range));
            if (nv != *v) {
                *v = nv;
                res.changed = true;
            }
        }
        if (res.hovered && ImGui::GetIO().MouseWheel != 0.0f) {
            int step = ImGui::GetIO().KeyShift ? std::max(1, range / 16) : 1;
            int nv = std::clamp(*v + (ImGui::GetIO().MouseWheel > 0 ? step : -step), vmin, vmax);
            if (nv != *v) {
                *v = nv;
                res.changed = true;
                res.released = true;
            }
            ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetItemID());
        }
        if (ImGui::IsItemDeactivated()) res.released = true;
        if (res.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) res.rightClicked = true;
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    ImU32 bg = ImGui::GetColorU32(res.hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    // Plain rectangles only: rounded rectangles and lines go through ImGui's anti-aliased path
    // builders (AddConvexPolyFilled / AddPolyline), which cost far more with 16 rows of bars.
    dl->AddRectFilled(pos, p1, bg);
    float t = float(std::clamp(*v, vmin, vmax) - vmin) / float(range);
    ImU32 fill = enabled ? color : ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.4f);
    if (centered) {
        float cx = pos.x + size.x * 0.5f;
        float x = pos.x + size.x * t;
        if (std::fabs(x - cx) < 1.0f) x = cx + 1.0f;
        dl->AddRectFilled(ImVec2(std::min(cx, x), pos.y + 1), ImVec2(std::max(cx, x), p1.y - 1), fill);
        float tick = std::max(2.0f, size.y * 0.2f);
        float tx = std::floor(cx);
        ImU32 tc = ImGui::GetColorU32(ImGuiCol_Border);
        dl->AddRectFilled(ImVec2(tx, pos.y), ImVec2(tx + 1, pos.y + tick), tc);
        dl->AddRectFilled(ImVec2(tx, p1.y - tick), ImVec2(tx + 1, p1.y), tc);
    } else if (t > 0.0f) {
        dl->AddRectFilled(ImVec2(pos.x + 1, pos.y + 1), ImVec2(pos.x + 1 + (size.x - 2) * t, p1.y - 1), fill);
    }
    if (locked) dl->AddRect(pos, p1, IM_COL32(255, 170, 40, 255), 2.0f, 0, 1.5f);
    if (text && *text) {
        // Clipped on the CPU: pushing a clip rectangle per bar would start a new draw call for each.
        ImFont* font = ImGui::GetFont();
        float fs = ImGui::GetFontSize();
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
        ImVec2 tp(std::floor(pos.x + (size.x - ts.x) * 0.5f), std::floor(pos.y + (size.y - ts.y) * 0.5f));
        ImVec4 clip(pos.x, pos.y, p1.x, p1.y);
        dl->AddText(font, fs, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 160), text, nullptr, 0.0f, &clip);
        dl->AddText(font, fs, tp, ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), text, nullptr, 0.0f, &clip);
    }
    (void)st;
    ImGui::PopID();
    return res;
}

void Meter(float level, ImVec2 size, bool vertical) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, p1, IM_COL32(20, 22, 26, 255));
    level = std::clamp(level, 0.0f, 1.0f);
    if (level <= 0.0f) return;
    const ImU32 green = IM_COL32(60, 200, 90, 255), yellow = IM_COL32(230, 210, 60, 255), red = IM_COL32(235, 70, 60, 255);
    if (vertical) {
        float h = (size.y - 2) * level;
        dl->AddRectFilledMultiColor(ImVec2(pos.x + 1, p1.y - 1 - h), ImVec2(p1.x - 1, p1.y - 1), LerpColor(green, red, level),
                                    LerpColor(green, red, level), green, green);
    } else {
        float w = (size.x - 2) * level;
        ImU32 end = level < 0.7f ? LerpColor(green, yellow, level / 0.7f) : LerpColor(yellow, red, (level - 0.7f) / 0.3f);
        dl->AddRectFilledMultiColor(ImVec2(pos.x + 1, pos.y + 1), ImVec2(pos.x + 1 + w, p1.y - 1), green, end, end, green);
    }
}

// Keyboard images at rest, by size (see Keyboard()).
struct KeyboardTexture {
    int lo, hi, w, h, frac, blackH;
    bool marker;
    float fb;
    uint64_t tex = 0;
    int lastFrame = 0;
};
static std::vector<KeyboardTexture> g_keyboardTextures;

static KeyboardTexture& keyboardTexture(int lo, int hi, int w, int h, int frac, int blackH, bool marker, float fb) {
    int frame = ImGui::GetFrameCount();
    for (KeyboardTexture& k : g_keyboardTextures)
        if (k.lo == lo && k.hi == hi && k.w == w && k.h == h && k.frac == frac && k.blackH == blackH && k.marker == marker && k.fb == fb) {
            k.lastFrame = frame;
            return k;
        }
    // Reuse the least recently used texture once a few sizes are cached (e.g. while resizing), but
    // never one drawn earlier in this frame.
    auto old = std::min_element(g_keyboardTextures.begin(), g_keyboardTextures.end(),
                                [](const KeyboardTexture& x, const KeyboardTexture& y) { return x.lastFrame < y.lastFrame; });
    if (g_keyboardTextures.size() >= 6 && old->lastFrame != frame) {
        uint64_t tex = old->tex;
        *old = KeyboardTexture{lo, hi, w, h, frac, blackH, marker, fb, 0, frame};
        immidi::deletePixelTexture(tex);
        return *old;
    }
    g_keyboardTextures.push_back(KeyboardTexture{lo, hi, w, h, frac, blackH, marker, fb, 0, frame});
    return g_keyboardTextures.back();
}

void ReleaseCachedTextures() {
    for (KeyboardTexture& k : g_keyboardTextures) immidi::deletePixelTexture(k.tex);
    g_keyboardTextures.clear();
}

static bool isBlack(int n) {
    int k = n % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

KeyboardResult Keyboard(const char* id, const uint8_t vel[128], const uint8_t* sustained, int lo, int hi, ImVec2 size, ImU32 activeColor,
                        int* heldNote) {
    KeyboardResult res;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return res;
    lo = std::clamp(lo, 0, 127);
    hi = std::clamp(hi, lo, 127);
    while (isBlack(lo) && lo > 0) lo--;
    while (isBlack(hi) && hi < 127) hi++;
    int whites = 0;
    for (int n = lo; n <= hi; n++)
        if (!isBlack(n)) whites++;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##kb", size);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float ww = size.x / float(std::max(1, whites));
    float bw = ww * 0.62f, bh = size.y * 0.62f;

    // Key geometry
    float wx[128] = {};
    int wi = 0;
    for (int n = lo; n <= hi; n++) {
        if (!isBlack(n)) {
            wx[n] = pos.x + wi * ww;
            wi++;
        } else {
            wx[n] = pos.x + wi * ww - bw * 0.5f;
        }
    }
    auto noteAt = [&](ImVec2 m) -> int {
        if (m.x < pos.x || m.x >= pos.x + size.x || m.y < pos.y || m.y >= pos.y + size.y) return -1;
        if (m.y < pos.y + bh)
            for (int n = lo; n <= hi; n++)
                if (isBlack(n) && m.x >= wx[n] && m.x < wx[n] + bw) return n;
        for (int n = lo; n <= hi; n++)
            if (!isBlack(n) && m.x >= wx[n] && m.x < wx[n] + ww) return n;
        return -1;
    };
    ImVec2 mouse = ImGui::GetIO().MousePos;
    if (hovered) res.hovered = noteAt(mouse);
    if (heldNote) {
        if (active) {
            int n = noteAt(mouse);
            if (n != *heldNote) {
                if (*heldNote >= 0) res.released = *heldNote;
                if (n >= 0) res.pressed = n;
                *heldNote = n;
            }
        } else if (*heldNote >= 0) {
            res.released = *heldNote;
            *heldNote = -1;
        }
    }

    ImVec4 ac = ImGui::ColorConvertU32ToFloat4(activeColor);
    auto velColor = [&](int v, bool black) {
        float t = 0.35f + 0.65f * float(v) / 127.0f;
        ImVec4 base = black ? ImVec4(0.1f, 0.1f, 0.12f, 1) : ImVec4(0.95f, 0.95f, 0.95f, 1);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(base.x + (ac.x - base.x) * t, base.y + (ac.y - base.y) * t,
                                                     base.z + (ac.z - base.z) * t, 1.0f));
    };
    // The keyboard at rest comes from a texture shared by all keyboards of the same size (the 16
    // channel keyboards are identical when no key is held), so it costs one quad instead of ~130
    // rectangles; held and hovered keys are drawn over it. All edges sit on whole pixels.
    const float x0 = std::floor(pos.x), y0 = std::floor(pos.y), x1 = std::floor(pos.x + size.x), y1 = std::floor(pos.y + size.y);
    const float bb = std::floor(pos.y + bh);
    auto px = [](float x) { return std::floor(x + 0.5f); };
    auto whiteRect = [&](int n, float& a, float& b) {
        a = std::max(x0 + 1, px(wx[n]));
        b = std::min(x1 - 1, px(wx[n] + ww) - 1);
        return b > a;
    };
    auto blackRect = [&](int n, float& a, float& b) {
        a = std::max(x0 + 1, px(wx[n]));
        b = std::min(x1 - 1, px(wx[n] + bw));
        return b > a;
    };
    const ImU32 kBg = IM_COL32(70, 70, 72, 255), kWhite = IM_COL32(235, 235, 232, 255), kBlack = IM_COL32(28, 28, 32, 255),
                kMark = IM_COL32(150, 150, 160, 255);
    const bool marker = ww >= 6.0f && size.y >= 18.0f;
    if (x1 > x0 + 2 && y1 > y0 + 2) {
        float fb = ImGui::GetIO().DisplayFramebufferScale.x;
        if (fb <= 0.0f) fb = 1.0f;
        int W = int(std::lround((x1 - x0) * fb)), H = int(std::lround((y1 - y0) * fb));
        int frac = int((pos.x - x0) * 64.0f);  // the key edges depend on where the keyboard starts
        KeyboardTexture& kt = keyboardTexture(lo, hi, W, H, frac, int(bb - y0), marker, fb);
        if (!kt.tex) {
            std::vector<uint32_t> img(size_t(W) * size_t(H), kBg);
            auto fill = [&](float ax, float ay, float bx, float by, ImU32 col) {
                int ix0 = std::max(0, int(std::lround((ax - x0) * fb))), ix1 = std::min(W, int(std::lround((bx - x0) * fb)));
                int iy0 = std::max(0, int(std::lround((ay - y0) * fb))), iy1 = std::min(H, int(std::lround((by - y0) * fb)));
                for (int y = iy0; y < iy1; y++) std::fill(img.begin() + y * W + ix0, img.begin() + y * W + std::max(ix0, ix1), col);
            };
            float a, b;
            for (int n = lo; n <= hi; n++) {
                if (isBlack(n) || !whiteRect(n, a, b)) continue;
                fill(a, y0 + 1, b, y1 - 1, kWhite);
                if (n % 12 == 0 && marker) fill(a + 1, y1 - 3, b - 1, y1 - 2, kMark);
            }
            for (int n = lo; n <= hi; n++)
                if (isBlack(n) && blackRect(n, a, b)) fill(a, y0 + 1, b, bb, kBlack);
            kt.tex = immidi::uploadPixelTexture(kt.tex, W, H, img.data());
        }
        dl->AddImage(ImTextureRef(ImTextureID(kt.tex)), ImVec2(x0, y0), ImVec2(x1, y1));
    }
    // Held, pedal-sustained and hovered keys. A black key is redrawn when it changes or when a white
    // key next to it was drawn over (black keys overlap the white ones). Sustained notes (released
    // keys a pedal keeps sounding) are a greyed shade of the active colour: darker than any held
    // white key, and paler than a held black key (softer notes shade those towards black).
    const ImU32 susWhite = LerpColor(activeColor, IM_COL32(60, 60, 66, 255), 0.5f), susBlack = LerpColor(activeColor, IM_COL32(140, 140, 146, 255), 0.6f);
    auto sus = [&](int n) { return sustained && sustained[n]; };
    bool whiteDrawn[128] = {};
    float a, b;
    for (int n = lo; n <= hi; n++) {
        if (isBlack(n) || (!vel[n] && !sus(n) && res.hovered != n) || !whiteRect(n, a, b)) continue;
        ImU32 col = vel[n] ? velColor(vel[n], false) : sus(n) ? susWhite : IM_COL32(210, 220, 240, 255);
        dl->AddRectFilled(ImVec2(a, y0 + 1), ImVec2(b, y1 - 1), col);
        if (n % 12 == 0 && marker) dl->AddRectFilled(ImVec2(a + 1, y1 - 3), ImVec2(b - 1, y1 - 2), kMark);
        whiteDrawn[n] = true;
    }
    for (int n = lo; n <= hi; n++) {
        if (!isBlack(n)) continue;
        bool neighbour = (n > 0 && whiteDrawn[n - 1]) || (n < 127 && whiteDrawn[n + 1]);
        if (!vel[n] && !sus(n) && res.hovered != n && !neighbour) continue;
        if (!blackRect(n, a, b)) continue;
        ImU32 col = vel[n] ? velColor(vel[n], true) : sus(n) ? susBlack : res.hovered == n ? IM_COL32(70, 80, 110, 255) : kBlack;
        dl->AddRectFilled(ImVec2(a, y0 + 1), ImVec2(b, bb), col);
    }
    ImGui::PopID();
    return res;
}

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 col) {
    switch (icon) {
    case Icon::Play:
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.6f, c.y + r), ImVec2(c.x + r * 0.9f, c.y), col);
        break;
    case Icon::Pause:
        dl->AddRectFilled(ImVec2(c.x - r * 0.75f, c.y - r), ImVec2(c.x - r * 0.2f, c.y + r), col);
        dl->AddRectFilled(ImVec2(c.x + r * 0.2f, c.y - r), ImVec2(c.x + r * 0.75f, c.y + r), col);
        break;
    case Icon::Stop:
        dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), col);
        break;
    case Icon::Prev:
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.85f), ImVec2(c.x - r * 0.7f, c.y + r * 0.85f), col);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.9f, c.y - r * 0.85f), ImVec2(c.x + r * 0.9f, c.y + r * 0.85f), ImVec2(c.x - r * 0.6f, c.y), col);
        break;
    case Icon::Next:
        dl->AddRectFilled(ImVec2(c.x + r * 0.7f, c.y - r * 0.85f), ImVec2(c.x + r, c.y + r * 0.85f), col);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.9f, c.y - r * 0.85f), ImVec2(c.x - r * 0.9f, c.y + r * 0.85f), ImVec2(c.x + r * 0.6f, c.y), col);
        break;
    case Icon::Loop:
        dl->PathArcTo(c, r * 0.75f, 0.4f, 5.6f, 16);
        dl->PathStroke(col, 0, r * 0.25f);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.05f), ImVec2(c.x + r * 0.35f, c.y - r * 0.05f),
                              ImVec2(c.x + r * 0.65f, c.y + r * 0.45f), col);
        break;
    case Icon::Shuffle:
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r * 0.6f, c.y + r * 0.6f), col, r * 0.22f);
        dl->AddLine(ImVec2(c.x - r, c.y + r * 0.6f), ImVec2(c.x + r * 0.6f, c.y - r * 0.6f), col, r * 0.22f);
        dl->AddTriangleFilled(ImVec2(c.x + r, c.y - r * 0.6f), ImVec2(c.x + r * 0.4f, c.y - r), ImVec2(c.x + r * 0.4f, c.y - r * 0.2f), col);
        dl->AddTriangleFilled(ImVec2(c.x + r, c.y + r * 0.6f), ImVec2(c.x + r * 0.4f, c.y + r * 0.2f), ImVec2(c.x + r * 0.4f, c.y + r), col);
        break;
    case Icon::Panic:
        dl->AddTriangle(ImVec2(c.x, c.y - r), ImVec2(c.x - r, c.y + r * 0.8f), ImVec2(c.x + r, c.y + r * 0.8f), col, r * 0.18f);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.4f), ImVec2(c.x, c.y + r * 0.25f), col, r * 0.2f);
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.5f), r * 0.1f, col);
        break;
    case Icon::Lock:
        dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r * 0.1f), ImVec2(c.x + r * 0.7f, c.y + r * 0.9f), col, 1.0f);
        dl->PathArcTo(ImVec2(c.x, c.y - r * 0.15f), r * 0.45f, 3.1416f, 6.2832f, 10);
        dl->PathStroke(col, 0, r * 0.2f);
        break;
    case Icon::Folder:
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x + r, c.y + r * 0.75f), col, 1.0f);
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.8f), ImVec2(c.x - r * 0.1f, c.y - r * 0.4f), col, 1.0f);
        break;
    case Icon::Plus:
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.15f), ImVec2(c.x + r, c.y + r * 0.15f), col);
        dl->AddRectFilled(ImVec2(c.x - r * 0.15f, c.y - r), ImVec2(c.x + r * 0.15f, c.y + r), col);
        break;
    }
}

bool IconButton(const char* id, Icon icon, ImVec2 size, bool active, const char* tooltip) {
    ImGui::PushID(id);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool pressed = ImGui::Button("##icon", size);
    if (active) ImGui::PopStyleColor();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    float r = std::min(b.x - a.x, b.y - a.y) * 0.28f;
    DrawIcon(ImGui::GetWindowDrawList(), icon, c, r, ImGui::GetColorU32(ImGuiCol_Text));
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

bool ToggleButton(const char* label, bool on, ImVec2 size, ImU32 onColor) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, onColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, LerpColor(onColor, IM_COL32(255, 255, 255, 255), 0.15f));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(15, 15, 15, 255));
    }
    bool pressed = ImGui::Button(label, size);
    if (on) ImGui::PopStyleColor(3);
    return pressed;
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void TextCentered(const char* text, float width) {
    float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - w) * 0.5f));
    ImGui::TextUnformatted(text);
}

} // namespace ui

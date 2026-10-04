#pragma once
#include "imgui.h"

#include <cstdint>

namespace ui {

// Result flags of interactive widgets
struct BarResult {
    bool changed = false;      // value changed by the user (while dragging, every step)
    bool released = false;     // mouse released after an edit
    bool rightClicked = false; // context click (used for "unlock")
    bool hovered = false;
};

// Horizontal value bar: click/drag sets the value from the mouse position, wheel steps it,
// double-click resets to `def`. `centered` draws from the middle (pan, bend).
BarResult ValueBar(const char* id, int* v, int vmin, int vmax, int def, ImVec2 size, const char* text, ImU32 color, bool locked,
                   bool centered = false, bool enabled = true);

// Level meter 0..1
void Meter(float level, ImVec2 size, bool vertical = false);

struct KeyboardResult {
    int pressed = -1;   // note that started sounding from a mouse press
    int released = -1;  // note that stopped
    int hovered = -1;
};

// Piano keyboard for notes lo..hi. `vel` holds the velocity (0 = off) of all 128 notes; `sustained`
// (optional, 128 entries) marks released notes that a pedal keeps sounding, drawn in a darker shade.
KeyboardResult Keyboard(const char* id, const uint8_t vel[128], const uint8_t* sustained, int lo, int hi, ImVec2 size, ImU32 activeColor,
                        int* heldNote);
// Frees the textures the widgets cache (call while the OpenGL context still exists).
void ReleaseCachedTextures();

enum class Icon { Play, Pause, Stop, Prev, Next, Loop, Shuffle, Panic, Lock, Folder, Plus };
bool IconButton(const char* id, Icon icon, ImVec2 size, bool active = false, const char* tooltip = nullptr);
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float r, ImU32 col);

bool ToggleButton(const char* label, bool on, ImVec2 size, ImU32 onColor);

// Small helpers
void HelpMarker(const char* text);
ImU32 ChannelColor(int ch, float alpha = 1.0f);
ImU32 LerpColor(ImU32 a, ImU32 b, float t);
void TextCentered(const char* text, float width);

} // namespace ui

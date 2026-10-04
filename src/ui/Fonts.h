#pragma once
#include <string>

struct ImFont;

namespace ui {

struct FontSet {
    ImFont* main = nullptr;
    ImFont* mono = nullptr;
    std::string cjkPath;  // merged CJK font (empty when none was found)
};

// Loads the UI fonts. `extraFont` (optional) is merged before the system CJK candidates.
FontSet LoadFonts(float baseSize, const std::string& extraFont);
extern FontSet g_fonts;

} // namespace ui

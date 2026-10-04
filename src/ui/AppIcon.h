#pragma once
#include <vector>

struct GLFWwindow;

namespace immidi {

enum class AppIconKind { Player, Bridge, Editor };

// The application icon as tools/gen_icon.py draws it (RGBA, top row first): an orange LCD with black
// level bars on a rounded square, with anti-aliased edges. ImMidi Bridge's LCD fades from that orange
// to the yellow-green of Yamaha's MU modules, the Conversion Editor's to a light blue.
std::vector<unsigned char> renderAppIcon(int size, AppIconKind kind);

// Linux: the window's icon (Windows takes it from the program's resources, macOS from the app bundle).
// X11 shows it; on Wayland, compositors with the xdg-toplevel-icon protocol (KDE Plasma 6.3 and later)
// do too (patched GLFW), others take it from the desktop entry named after the app ID.
void setWindowIcon(GLFWwindow* window, AppIconKind kind);

} // namespace immidi

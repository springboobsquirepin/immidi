#include "AppIcon.h"

#define GLFW_INCLUDE_NONE  // no OpenGL headers
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace immidi {

std::vector<unsigned char> renderAppIcon(int size, AppIconKind kind) {
    static const int kBars[8] = {5, 9, 12, 8, 14, 11, 6, 10};  // lit cells of 16, per column
    const unsigned char bg0[3] = {255, 111, 15}, ghost0[3] = {200, 80, 0}, ink[3] = {0, 0, 0};
    // The right side: the bridge's MU yellow-green, the editor's light blue.
    const bool fade = kind != AppIconKind::Player;
    const unsigned char bg1[3] = {uint8_t(kind == AppIconKind::Editor ? 110 : 180), uint8_t(kind == AppIconKind::Editor ? 180 : 200),
                                  uint8_t(kind == AppIconKind::Editor ? 255 : 60)};
    const unsigned char ghost1[3] = {uint8_t(kind == AppIconKind::Editor ? 70 : 140), uint8_t(kind == AppIconKind::Editor ? 130 : 160),
                                     uint8_t(kind == AppIconKind::Editor ? 210 : 40)};
    std::vector<unsigned char> px(size_t(size) * size_t(size) * 4, 0);
    const double s = size / 1024.0;
    const double x0 = 100 * s, y0 = 100 * s, x1 = 924 * s, y1 = 924 * s, rad = 185 * s;
    const double mx0 = 200 * s, my0 = 250 * s, mw = 624 * s, mh = 560 * s;
    const double pitchX = mw / 8, pitchY = mh / 16, cellW = pitchX * 0.86, cellH = pitchY * 0.78;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            double cx = x + 0.5, cy = y + 0.5;
            // Signed distance to the rounded square, for the coverage of its edge.
            double dx = std::max({x0 + rad - cx, 0.0, cx - (x1 - rad)}), dy = std::max({y0 + rad - cy, 0.0, cy - (y1 - rad)});
            double d;
            if (cx < x0 || cx > x1 || cy < y0 || cy > y1) {
                d = std::max({x0 - cx, cx - x1, y0 - cy, cy - y1});
                if (dx > 0 && dy > 0) d = std::sqrt(dx * dx + dy * dy) - rad;
            } else {
                d = (dx > 0 && dy > 0) ? std::sqrt(dx * dx + dy * dy) - rad : -1;
            }
            double a = std::clamp(0.5 - d, 0.0, 1.0);
            if (a <= 0) continue;
            double t = fade ? std::clamp((cx - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
            unsigned char bg[3], ghost[3];
            for (int k = 0; k < 3; k++) {
                bg[k] = (unsigned char)std::lround(bg0[k] + (bg1[k] - bg0[k]) * t);
                ghost[k] = (unsigned char)std::lround(ghost0[k] + (ghost1[k] - ghost0[k]) * t);
            }
            const unsigned char* col = bg;
            double fx = cx - mx0, fy = cy - my0;
            if (fx >= 0 && fx < mw && fy >= 0 && fy < mh) {
                int c = int(fx / pitchX), r = int(fy / pitchY);
                if (fx - c * pitchX < cellW && fy - r * pitchY < cellH) col = r >= 16 - kBars[c] ? ink : ghost;
            }
            unsigned char* p = &px[(size_t(y) * size_t(size) + size_t(x)) * 4];
            p[0] = col[0];
            p[1] = col[1];
            p[2] = col[2];
            p[3] = (unsigned char)std::lround(a * 255);
        }
    return px;
}

void setWindowIcon(GLFWwindow* window, AppIconKind kind) {
#if defined(__linux__)
    std::vector<std::vector<unsigned char>> pixels;
    std::vector<GLFWimage> images;
    for (int size : {16, 24, 32, 48, 64, 128}) {
        pixels.push_back(renderAppIcon(size, kind));
        images.push_back(GLFWimage{size, size, pixels.back().data()});
    }
    GLFWerrorfun previous = glfwSetErrorCallback(nullptr);  // "not supported" from such a compositor is expected
    glfwSetWindowIcon(window, int(images.size()), images.data());
    glfwSetErrorCallback(previous);
#else
    (void)window;
    (void)kind;
#endif
}

} // namespace immidi

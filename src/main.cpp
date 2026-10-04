// ImMidi - a MIDI file player with a Dear ImGui interface.
#include "App.h"
#include "AppIcon.h"
#include "Config.h"
#include "Fonts.h"
#include "Renderer.h"
#include "Util.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_internal.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // sprintf inside stb_image_write (macOS SDK)
#endif
#include "stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include "FileDropTarget.h"
#include "Widgets.h"

using namespace immidi;

static std::vector<std::string> g_dropped;

static double g_dropX = -1, g_dropY = -1;
// Last position reported while files were dragged over the window (X11, Cocoa).
static bool g_hoverSeen = false;
static double g_hoverX = 0, g_hoverY = 0;
static void dropCallback(GLFWwindow* window, int count, const char** paths) {
    for (int i = 0; i < count; i++) g_dropped.push_back(paths[i]);
    if (g_hoverSeen) {
        g_dropX = g_hoverX;
        g_dropY = g_hoverY;
    } else {
        // GLFW moves the cursor position to the drop point before calling this.
        glfwGetCursorPos(window, &g_dropX, &g_dropY);
    }
    g_hoverSeen = false;
}

// Files being dragged over the window (for the playlist's insertion mark).
static bool g_dragInside = false;
static double g_dragX = 0, g_dragY = 0;
static void dragCallback(GLFWwindow*, int inside, double x, double y) {
    g_dragInside = inside != 0;
    g_dragX = x;
    g_dragY = y;
    if (inside) {
        g_hoverSeen = true;
        g_hoverX = x;
        g_hoverY = y;
    }
}

#if defined(_WIN32)
// While a window is moved or resized (or its system menu is open), Windows runs its own modal message
// loop and glfwPollEvents() does not return until it ends, which froze the UI. A timer running during
// those loops keeps rendering frames from inside the window procedure.
static WNDPROC g_prevWndProc = nullptr;
static std::function<void()>* g_modalFrame = nullptr;
static bool g_inFrame = false;
static constexpr UINT_PTR kModalTimer = 0x494D;  // "IM"

static LRESULT CALLBACK modalLoopWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP: SetTimer(hwnd, kModalTimer, USER_TIMER_MINIMUM, nullptr); break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP: KillTimer(hwnd, kModalTimer); break;
    case WM_TIMER:
        if (wp == kModalTimer) {
            if (g_modalFrame && !g_inFrame) (*g_modalFrame)();
            return 0;
        }
        break;
    default: break;
    }
    return CallWindowProcW(g_prevWndProc, hwnd, msg, wp, lp);
}
#endif

// UI scale for the window's monitor. The content scale says how much larger the UI should be; on
// macOS and Wayland the window is measured in logical points and the framebuffer already has the
// extra pixels (ImGui rasterizes fonts at the framebuffer density), so only the part of the content
// scale that the framebuffer does not cover is applied. Windows and X11 use pixel coordinates.
static float uiDpiScale(GLFWwindow* window) {
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    float fbScale = (ww > 0 && fw > 0) ? float(fw) / float(ww) : 1.0f;
    float s = xs / std::max(1.0f, fbScale);
    return s > 0.25f ? s : 1.0f;
}

static void contentScaleCallback(GLFWwindow* window, float, float) {
    if (ImGui::GetCurrentContext()) ImGui::GetStyle().FontScaleDpi = uiDpiScale(window);
}

// Physical key events for the virtual keyboard, after ImGui's own key callback.
static GLFWkeyfun g_prevKeyCallback = nullptr;
static App* g_app = nullptr;
static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (g_prevKeyCallback) g_prevKeyCallback(window, key, scancode, action, mods);
    if (g_app) g_app->onKey(key, action, mods);
}

static void glfwErrorCallback(int error, const char* desc) { fprintf(stderr, "GLFW error %d: %s\n", error, desc); }

struct Options {
    std::vector<std::string> files;
    std::string screenshot;
    // Testing: whenever this file appears, save a screenshot to <file>.png and delete the file.
    std::string shotTrigger;
    int frames = 90;
    int tab = -1;
    int view = -1;
    int device = -2;
    std::string output;
    bool play = false;
    double seek = -1;
    int width = 1600, height = 960;
    int picker = -1;
    bool frameStats = false;
    std::string renderer;  // "d3d11", "metal", "vulkan" or "opengl" (default: the setting, else the platform's API)
    std::string platform;  // Linux: "wayland" or "x11" (default: follow the session, Wayland first)
    // Testing: simulate a file drop at window position (x, y) on frame 10.
    float dropX = -1, dropY = -1;
    std::vector<std::string> dropFiles;
};

static Options parseArgs(const std::vector<std::string>& args) {
    Options o;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--screenshot") o.screenshot = next();
        else if (a == "--frames") o.frames = atoi(next().c_str());
        else if (a == "--screenshot-trigger") o.shotTrigger = next();
        else if (a == "--tab") {
            std::string t = lowerAscii(next());
            const char* names[] = {"channels", "soundedit", "effects", "lyrics", "playlist", "info", "settings"};
            for (int k = 0; k < 7; k++)
                if (t == names[k]) o.tab = k;
        } else if (a == "--view") o.view = atoi(next().c_str());
        else if (a == "--device") {
            std::string d = lowerAscii(next());
            const char* names[] = {"gm", "gm2", "sc55", "sc88", "sc88pro", "sc8850", "xg"};
            o.device = -1;
            for (int k = 0; k < 7; k++)
                if (d == names[k]) o.device = k;
        } else if (a == "--out") o.output = next();
        else if (a == "--play") o.play = true;
        else if (a == "--picker") o.picker = atoi(next().c_str());
        else if (a == "--frame-stats") o.frameStats = true;
        else if (a == "--platform") o.platform = lowerAscii(next());
        else if (a == "--renderer") o.renderer = lowerAscii(next());
        else if (a == "--drop") {
            o.dropX = float(atof(next().c_str()));
            o.dropY = float(atof(next().c_str()));
            o.dropFiles.push_back(next());
        }
        else if (a == "--seek") o.seek = atof(next().c_str());
        else if (a == "--size") {
            std::string s = next();
            sscanf(s.c_str(), "%dx%d", &o.width, &o.height);
        } else if (a == "--help" || a == "-h") {
            printf("usage: ImMidi [files or folders...] [--play] [--device gm|gm2|sc55|sc88|sc88pro|sc8850|xg]\n"
                   "              [--out <MIDI output name>] [--seek <seconds>] [--tab <name>] [--view 0-3]\n"
                   "              [--size WxH] [--screenshot out.png [--frames N]] [--platform wayland|x11]\n"
                   "              [--renderer d3d11|metal|vulkan|opengl]  (opengl is deprecated)\n");
            exit(0);
        } else o.files.push_back(a);
    }
    return o;
}

static int run(const std::vector<std::string>& args) {
    Options opt = parseArgs(args);
    glfwSetErrorCallback(glfwErrorCallback);
    if (opt.platform == "wayland") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    else if (opt.platform == "x11") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit()) return 1;

    // Graphics API: --renderer, else the setting, else the platform's native API (Direct3D 11 on
    // Windows, Metal on macOS, Vulkan on Linux). If it cannot start, the deprecated OpenGL renderer
    // is used instead.
    RendererKind kind = defaultRenderer();
    {
        Config saved;
        saved.load(configDirectory() + "/settings.ini");
        RendererKind k;
        if (parseRendererName(saved.get("gfx.renderer"), &k) && rendererAvailable(k)) kind = k;
        if (!opt.renderer.empty()) {
            if (parseRendererName(opt.renderer, &k) && rendererAvailable(k)) kind = k;
            else fprintf(stderr, "renderer %s is not available here\n", opt.renderer.c_str());
        }
    }
    std::unique_ptr<Renderer> renderer = createRenderer(kind);
    GLFWwindow* window = nullptr;
    auto makeWindow = [&]() {
        glfwDefaultWindowHints();
        renderer->windowHints();
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, opt.screenshot.empty() ? GLFW_TRUE : GLFW_FALSE);
        // Linux desktops tie the window to ImMidi's desktop entry, org.immidi.ImMidi.desktop, by the
        // Wayland app ID and the X11 window class (StartupWMClass), and show its icon and name.
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "org.immidi.ImMidi");
        glfwWindowHintString(GLFW_X11_CLASS_NAME, "ImMidi");
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "immidi");
        window = glfwCreateWindow(opt.width, opt.height, "ImMidi", nullptr, nullptr);
    };
    makeWindow();
    std::string rendererError;
    if (!window || !renderer->init(window, &rendererError)) {
        if (kind == RendererKind::OpenGL) {
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        fprintf(stderr, "%s: %s; using OpenGL (deprecated) instead\n", rendererName(kind), rendererError.c_str());
        renderer->shutdown();  // before the window its surface / swap chain belongs to
        if (window) glfwDestroyWindow(window);
        kind = RendererKind::OpenGL;
        renderer = createRenderer(kind);
        makeWindow();
        if (!window || !renderer->init(window, &rendererError)) {
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }
    setActiveRenderer(renderer.get());
    setWindowIcon(window, AppIconKind::Player);  // Linux; the others take it from the program's resources / the app bundle
    glfwSetDropCallback(window, dropCallback);
    glfwSetDragCallback(window, dragCallback);  // X11 and Cocoa (patched GLFW)
#if defined(_WIN32)
    FileDropCallbacks dropCbs;
    dropCbs.hover = [](bool inside, double x, double y) { dragCallback(nullptr, inside, x, y); };
    dropCbs.drop = [](const std::vector<std::string>& paths, double x, double y) {
        g_dropped.insert(g_dropped.end(), paths.begin(), paths.end());
        g_dropX = x;
        g_dropY = y;
    };
    installFileDropTarget(glfwGetWin32Window(window), dropCbs);
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string iniPath = configDirectory() + "/imgui.ini";
    io.IniFilename = iniPath.c_str();

    renderer->initImGui();

    ImGui::StyleColorsDark();
    ImGui::GetStyle().FontScaleDpi = uiDpiScale(window);
    glfwSetWindowContentScaleCallback(window, contentScaleCallback);  // moved to another monitor
    ui::LoadFonts(15.0f, "");

    {
        App app(window);
        g_app = &app;
        g_prevKeyCallback = glfwSetKeyCallback(window, keyCallback);
        if (!opt.output.empty()) app.setOutputForAllPorts(opt.output);
        if (opt.device >= -1) app.setDevice(opt.device);
        if (opt.view >= 0 && opt.view < int(ViewMode::Count)) app.setViewMode(ViewMode(opt.view));
        if (!opt.files.empty()) app.openPaths(opt.files, true, opt.play);
        if (opt.seek >= 0) app.seekAfterLoad(int64_t(opt.seek * 1e6));
        if (opt.tab >= 0) app.selectTab(Tab(opt.tab));
        bool pickerPending = opt.picker >= 0;

        std::string lastTitle;
        int frame = 0;
        auto lastFrame = std::chrono::steady_clock::now();
        bool done = false;
        // One UI frame. Runs from the main loop and, on Windows, from the modal move/size loop.
        std::function<void()> runFrame = [&]() {
            if (done) return;
#if defined(_WIN32)
            g_inFrame = true;
            struct ClearInFrame {
                ~ClearInFrame() { g_inFrame = false; }
            } clearInFrame;
#endif
            app.setExternalDrag(g_dragInside, float(g_dragX), float(g_dragY));
            if (!g_dropped.empty()) {
                std::vector<std::string> d;
                d.swap(g_dropped);
                app.dropPaths(d, float(g_dropX), float(g_dropY));
            }
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return;
            }
            renderer->newFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            if (!opt.dropFiles.empty() && frame == 10) app.dropPaths(opt.dropFiles, opt.dropX, opt.dropY);
            if (pickerPending && frame == 5) {
                app.showInstrumentPicker(opt.picker / 16, opt.picker % 16);
                pickerPending = false;
            }
            auto frameStart = std::chrono::steady_clock::now();
            app.frame();
            ImGui::Render();
            double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
            static const float kClearColor[4] = {0.08f, 0.09f, 0.1f, 1.0f};
            renderer->render(ImGui::GetDrawData(), kClearColor);
            app.renderOffscreen();
            if (opt.frameStats) {
                // Build = ImGui frame + draw list generation (CPU); total adds the GPU submission.
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
                static double worst = 0, total = 0, build = 0, vtx = 0, cmds = 0;
                static int count = 0;
                worst = std::max(worst, ms);
                total += ms;
                build += buildMs;
                vtx += ImGui::GetDrawData()->TotalVtxCount;
                for (ImDrawList* l : ImGui::GetDrawData()->CmdLists) cmds += l->CmdBuffer.Size;
                if (++count % 60 == 0) {
                    printf("frames %d: build avg %.2f ms, total avg %.2f ms (worst %.2f), %.0f vertices, %.0f draw calls/frame\n", count,
                           build / count, total / count, worst, vtx / count, cmds / count);
                    fflush(stdout);
                }
            }
            if (!opt.shotTrigger.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(opt.shotTrigger, ec)) {
                    std::filesystem::remove(opt.shotTrigger, ec);
                    bool ok = renderer->screenshot(opt.shotTrigger + ".png");
                    printf("screenshot %s.png: %s\n", opt.shotTrigger.c_str(), ok ? "ok" : "failed");
                    fflush(stdout);
                }
            }
            if (!opt.screenshot.empty() && ++frame >= opt.frames) {
                bool ok = renderer->screenshot(opt.screenshot);
                printf("screenshot %s: %s\n", opt.screenshot.c_str(), ok ? "ok" : "failed");
                done = true;
                return;
            }
            renderer->present();
            std::string title = app.windowTitle();
            if (title != lastTitle) {
                glfwSetWindowTitle(window, title.c_str());
                lastTitle = title;
            }
            // Cap the frame rate when vsync is not available (e.g. virtual displays).
            auto now = std::chrono::steady_clock::now();
            auto elapsed = now - lastFrame;
            if (elapsed < std::chrono::milliseconds(15)) std::this_thread::sleep_for(std::chrono::milliseconds(15) - elapsed);
            lastFrame = std::chrono::steady_clock::now();
        };
#if defined(_WIN32)
        HWND hwnd = glfwGetWin32Window(window);
        g_modalFrame = &runFrame;
        g_prevWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(modalLoopWndProc)));
#endif
        // Redraw at the display rate only while the user interacts or fast things move, less often
        // while only slow things change, otherwise sleep until an event arrives (at most half a
        // second). Test runs (screenshots, frame statistics) always redraw.
        const bool alwaysRender = !opt.screenshot.empty() || !opt.shotTrigger.empty() || opt.frameStats || !opt.dropFiles.empty() ||
                                  opt.picker >= 0;
        double lastInput = glfwGetTime();
        bool mouseInside = false;
        // ImGui's GLFW backend reads the cursor position every frame while the window has the focus,
        // wherever the cursor is; moves outside the window are not interaction.
        auto sawInput = [&]() {
            const ImGuiIO& io = ImGui::GetIO();
            bool any = false;
            for (const ImGuiInputEvent& e : ImGui::GetCurrentContext()->InputEventsTrail) {
                if (e.Type != ImGuiInputEventType_MousePos) {
                    any = true;
                    continue;
                }
                bool inside = e.MousePos.PosX >= 0 && e.MousePos.PosY >= 0 && e.MousePos.PosX < io.DisplaySize.x && e.MousePos.PosY < io.DisplaySize.y;
                if (inside || mouseInside) any = true;  // moves inside, and the one that leaves (hover ends)
                mouseInside = inside;
            }
            return any || ImGui::IsAnyMouseDown() || io.WantTextInput;
        };
        double frameStart = 0.0;
        while (!done && !glfwWindowShouldClose(window) && !app.quitRequested()) {
            // How long to wait for events before the next frame: no wait while the user interacts or
            // meters and keyboards move (the display's vsync paces the frames), the app's interval
            // when only slower things change (the transport on the other tabs, the mini player), half
            // a second when nothing changes. Input and worker threads end the wait at once.
            double wait = 0.0;
            if (!alwaysRender && glfwGetTime() - lastInput >= 1.0 && g_dropped.empty()) {
                double interval = app.frameInterval();
                wait = interval < 0.0 ? 0.5 : frameStart + interval - glfwGetTime();
            }
            if (wait > 0.0) glfwWaitEventsTimeout(wait);
            else glfwPollEvents();
            frameStart = glfwGetTime();
            runFrame();
            // Input seen by this frame (mouse, keys, wheel, focus) keeps the next second fluid, for
            // hover effects and tooltip delays.
            if (sawInput()) lastInput = glfwGetTime();
        }
#if defined(_WIN32)
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevWndProc));
        g_modalFrame = nullptr;
#endif
        glfwSetKeyCallback(window, g_prevKeyCallback);
        g_app = nullptr;
    }

    ui::ReleaseCachedTextures();
    renderer->shutdownImGui();
    ImGui::DestroyContext();
    setActiveRenderer(nullptr);
    renderer->shutdown();
#if defined(_WIN32)
    removeFileDropTarget(glfwGetWin32Window(window));
#endif
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#if defined(_WIN32)
int main() {
    // Get UTF-8 arguments on Windows.
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(pathToUtf8(std::filesystem::path(wargv[i])));
    LocalFree(wargv);
    return run(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    return run(args);
}
#endif

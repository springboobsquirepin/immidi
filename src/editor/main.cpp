// ImMidi Conversion Editor - edits the conversion tables (conversion/*.json) that ImMidi's emulation
// converts songs with: the sounds and drum kits that play another module's sounds, the drum note maps
// and the controller rules.
#include "AppIcon.h"
#include "Config.h"
#include "Editor.h"
#include "Fonts.h"
#include "Renderer.h"
#include "Util.h"
#include "Widgets.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_internal.h"  // the frame's input events

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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
#endif

using namespace immidi;

namespace {

// As ImMidi's player: the part of the monitor's content scale that the framebuffer does not cover.
float uiDpiScale(GLFWwindow* window) {
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    float fbScale = (ww > 0 && fw > 0) ? float(fw) / float(ww) : 1.0f;
    float s = xs / std::max(1.0f, fbScale);
    return s > 0.25f ? s : 1.0f;
}

void contentScaleCallback(GLFWwindow* window, float, float) {
    if (ImGui::GetCurrentContext()) ImGui::GetStyle().FontScaleDpi = uiDpiScale(window);
}

void glfwErrorCallback(int error, const char* desc) { fprintf(stderr, "GLFW error %d: %s\n", error, desc); }

// The close button asks first when the tables have unsaved changes.
void closeCallback(GLFWwindow* window) {
    auto* editor = static_cast<ConvEditor*>(glfwGetWindowUserPointer(window));
    if (!editor) return;
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    editor->requestClose();
}

struct Options {
    std::string renderer, platform, shotTrigger;
    int width = 1280, height = 800;
};

Options parseArgs(const std::vector<std::string>& args) {
    Options o;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--renderer") o.renderer = lowerAscii(next());
        else if (a == "--platform") o.platform = lowerAscii(next());
        else if (a == "--screenshot-trigger") o.shotTrigger = next();
        else if (a == "--size") {
            std::string s = next();
            sscanf(s.c_str(), "%dx%d", &o.width, &o.height);
        } else if (a == "--help" || a == "-h") {
            printf("usage: ImMidiConvEditor [--size WxH] [--platform wayland|x11] [--renderer d3d11|metal|vulkan|opengl]\n"
                   "  Edits the conversion tables of ImMidi's emulation (conversion/*.json).\n");
            exit(0);
        }
    }
    return o;
}

int run(const std::vector<std::string>& args) {
    Options opt = parseArgs(args);
    glfwSetErrorCallback(glfwErrorCallback);
    if (opt.platform == "wayland") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    else if (opt.platform == "x11") glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit()) return 1;

    // The graphics API as ImMidi's player chooses it: --renderer, else ImMidi's setting, else the
    // platform's native API; OpenGL (deprecated) when that cannot start.
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
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "org.immidi.ImMidiConvEditor");
        glfwWindowHintString(GLFW_X11_CLASS_NAME, "ImMidiConvEditor");
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "immidiconveditor");
        window = glfwCreateWindow(opt.width, opt.height, "ImMidi Conversion Editor", nullptr, nullptr);
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
        renderer->shutdown();
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
    setWindowIcon(window, AppIconKind::Editor);
    glfwSetWindowSizeLimits(window, 900, 560, GLFW_DONT_CARE, GLFW_DONT_CARE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // one fixed window: nothing to remember
    renderer->initImGui();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowRounding = 0.0f;
    style.ItemSpacing = ImVec2(6, 5);
    style.FramePadding = ImVec2(6, 3);
    style.WindowPadding = ImVec2(10, 8);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);  // dialogs darken the window behind them
    style.FontScaleDpi = uiDpiScale(window);
    glfwSetWindowContentScaleCallback(window, contentScaleCallback);
    ui::LoadFonts(15.0f, "");

    {
        ConvEditor editor(window);
        glfwSetWindowUserPointer(window, &editor);
        glfwSetWindowCloseCallback(window, closeCallback);
        // As ImMidi's player: at the display rate for a second after input (hover effects, tooltip
        // delays), 20 frames a second while something changes on its own, otherwise on events.
        double lastInput = glfwGetTime();
        bool mouseInside = false;
        auto sawInput = [&]() {
            const ImGuiIO& io = ImGui::GetIO();
            bool any = false;
            for (const ImGuiInputEvent& e : ImGui::GetCurrentContext()->InputEventsTrail) {
                if (e.Type != ImGuiInputEventType_MousePos) {
                    any = true;
                    continue;
                }
                bool inside = e.MousePos.PosX >= 0 && e.MousePos.PosY >= 0 && e.MousePos.PosX < io.DisplaySize.x && e.MousePos.PosY < io.DisplaySize.y;
                if (inside || mouseInside) any = true;
                mouseInside = inside;
            }
            return any || ImGui::IsAnyMouseDown() || io.WantTextInput;
        };
        auto lastFrame = std::chrono::steady_clock::now();
        std::string lastTitle;
        while (!editor.quitRequested()) {
            if (glfwGetTime() - lastInput < 1.0) glfwPollEvents();
            else glfwWaitEventsTimeout(editor.busy() ? 1.0 / 20 : 1.0);
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            renderer->newFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            editor.frame();
            ImGui::Render();
            static const float kClearColor[4] = {0.09f, 0.10f, 0.12f, 1.0f};
            renderer->render(ImGui::GetDrawData(), kClearColor);
            if (!opt.shotTrigger.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(opt.shotTrigger, ec)) {
                    std::filesystem::remove(opt.shotTrigger, ec);
                    bool ok = renderer->screenshot(opt.shotTrigger + ".png");
                    printf("screenshot %s.png: %s\n", opt.shotTrigger.c_str(), ok ? "ok" : "failed");
                    fflush(stdout);
                }
            }
            renderer->present();
            std::string title = editor.windowTitle();
            if (title != lastTitle) {
                glfwSetWindowTitle(window, title.c_str());
                lastTitle = title;
            }
            if (sawInput()) lastInput = glfwGetTime();
            // Without vsync (virtual displays), at most about 60 frames a second.
            auto now = std::chrono::steady_clock::now();
            if (now - lastFrame < std::chrono::milliseconds(15)) std::this_thread::sleep_for(std::chrono::milliseconds(15) - (now - lastFrame));
            lastFrame = std::chrono::steady_clock::now();
        }
        glfwSetWindowUserPointer(window, nullptr);
    }

    ui::ReleaseCachedTextures();
    renderer->shutdownImGui();
    ImGui::DestroyContext();
    setActiveRenderer(nullptr);
    renderer->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace

#if defined(_WIN32)
int main() {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(pathToUtf8(std::filesystem::path(wargv[i])));
    LocalFree(wargv);
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif

// ImMidi Bridge - ImMidi's emulation of the MIDI standards as a realtime MIDI filter: the messages of a
// MIDI input are converted for another sound module (GM, GS or XG) and sent to a MIDI output, so that
// other MIDI programs can use it. It converts as ImMidi's player does, with the same conversion tables.
#include "AppIcon.h"
#include "Config.h"
#include "Fonts.h"
#include "MidiBridge.h"
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

const char* const kToneMaps[5] = {"Off", "SC-55 map", "SC-88 map", "SC-88Pro map", "SC-8850 map"};

class BridgeWindow {
public:
    BridgeWindow() {
        cfg_.load(configDirectory() + "/bridge.ini");
        DeviceKind d;
        if (deviceFromId(cfg_.get("source", "gm"), d)) settings_.source = d;
        if (deviceFromId(cfg_.get("target", "sc88pro"), d)) settings_.target = d;
        settings_.followResets = cfg_.getBool("followResets", true);
        settings_.useTables = cfg_.getBool("tables", true);
        settings_.forcedToneMap = std::clamp(cfg_.getInt("forceMap", 0), 0, 4);
        tablesFolder_ = ConversionTables::defaultFolder();  // as ImMidi finds them
        if (!bridge_.loadTables(tablesFolder_, tablesError_)) tablesFolder_.clear();
        bridge_.configure(settings_);
        bridge_.setWake([] { glfwPostEmptyEvent(); });
        inWanted_ = cfg_.get("input", MidiBridge::kNone);
        outWanted_ = cfg_.get("output", MidiBridge::kNone);
        refreshPorts();  // opens the saved devices
        store();
        savedText_ = cfg_.serialize();  // the settings as loaded (with the defaults): written once one changes
    }

    // Saved when a setting changes (not on a timer); the ports by the names chosen, also while a
    // device is missing, so it is used again once it is back.
    void saveIfChanged() {
        store();
        std::string text = cfg_.serialize();
        if (text != savedText_ && cfg_.save(configDirectory() + "/bridge.ini")) savedText_ = text;
    }

    void frame() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##bridge", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        bool changed = false;
        const float labelW = 150.0f * ImGui::GetStyle().FontScaleDpi;
        const float comboW = std::max(200.0f, ImGui::GetContentRegionAvail().x - labelW - 90.0f * ImGui::GetStyle().FontScaleDpi);
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(comboW);
        };

        ImGui::SeparatorText("MIDI");
        label("Input");
        if (portCombo("##in", inputs_, inWanted_, bridge_.input())) {
            inError_.clear();
            if (!bridge_.setInput(inWanted_, &inError_)) inError_ = "Input: " + inError_;
            changed = true;
        }
        ImGui::SameLine();
        activity(bridge_.secondsSinceIn(), bridge_.messagesIn(), "Messages received");
        label("Output");
        if (portCombo("##out", outputs_, outWanted_, bridge_.output())) {
            outError_.clear();
            if (!bridge_.setOutput(outWanted_, &outError_)) outError_ = "Output: " + outError_;
            changed = true;
        }
        ImGui::SameLine();
        activity(bridge_.secondsSinceOut(), bridge_.messagesOut(), "Messages sent");
        ImGui::SetCursorPosX(labelW);
        if (ImGui::SmallButton("Refresh the device lists")) refreshPorts();
        ImGui::SameLine();
        ui::HelpMarker("Programs send to the input and the module listens to the output: choose a loopback port (loopMIDI "
                       "on Windows, the IAC Driver on macOS, Midi Through on Linux) as the output of the other program and as "
                       "the input here. On Linux and macOS the bridge's own virtual ports can be used instead.");
        if (inWanted_ != MidiBridge::kNone && inWanted_ == outWanted_) {
            ImGui::SetCursorPosX(labelW);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                               "The input and the output are the same device: right for a MIDI interface, but a loopback port "
                               "would send every message back in.");
            ImGui::PopTextWrapPos();
        }

        ImGui::SeparatorText("Conversion");
        DeviceKind current = bridge_.source();
        bool sourceChosen = false;
        label("Source");
        if (deviceCombo("##source", current)) {
            settings_.source = current;
            sourceChosen = changed = true;
        }
        ImGui::SameLine();
        ui::HelpMarker("The standard or sound module the incoming messages are written for (as \"Song made for\" in ImMidi). "
                       "With \"Follow resets\", a GM, GM2, GS or XG reset in the stream switches it; a GS reset to the GS "
                       "module last chosen here (the SC-55 when none was, as ImMidi assumes for a GS song).");
        ImGui::SetCursorPosX(labelW);
        changed |= ImGui::Checkbox("Follow resets (GM, GM2, GS and XG resets switch the source)", &settings_.followResets);
        label("Destination");
        if (deviceCombo("##target", settings_.target)) changed = true;
        ImGui::SameLine();
        ui::HelpMarker("The sound module at the output: the messages are converted for it.");
        ImGui::SetCursorPosX(labelW);
        changed |= ImGui::Checkbox("Use the instrument conversion tables", &settings_.useTables);
        ImGui::SameLine();
        if (tablesFolder_.empty()) ui::HelpMarker(("Not found: " + tablesError_ + ". They are looked up in a 'conversion' folder next to the program, in the working directory and in the settings folder.").c_str());
        else ui::HelpMarker(("The same tables as ImMidi's player (gs-to-xg.json, xg-to-gs.json, gs-maps.json): closest voices and drum kits, drum notes, volumes and controller scaling. Folder: " + tablesFolder_).c_str());
        // Only for the Sound Canvases with several tone maps (SC-88 and later).
        const DeviceProfile& tp = deviceProfile(settings_.target);
        const int maxMap = tp.family == MidiStandard::GS && tp.gsMaxMap >= 2 ? tp.gsMaxMap : 0;
        if (settings_.forcedToneMap > maxMap) {
            settings_.forcedToneMap = 0;
            changed = true;
        }
        label("Tone map forcer");
        ImGui::BeginDisabled(maxMap == 0);
        if (ImGui::BeginCombo("##map", kToneMaps[settings_.forcedToneMap])) {
            for (int m = 0; m <= maxMap; m++)
                if (ImGui::Selectable(kToneMaps[m], m == settings_.forcedToneMap)) {
                    settings_.forcedToneMap = m;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ui::HelpMarker("Sound Canvases with several tone maps (SC-88 and later): every part plays with the chosen map.");

        // A source a reset switched to stays when other settings change.
        if (changed) bridge_.configure(settings_, !sourceChosen);

        ImGui::Spacing();
        std::string conv = bridge_.conversion();
        DeviceKind src = bridge_.source();
        if (conv.empty())
            ImGui::TextDisabled("Nothing to convert: the messages pass unchanged (%s to %s).", deviceShortName(src), deviceShortName(settings_.target));
        else
            ImGui::Text("Converting %s to %s: %s", deviceShortName(src), deviceShortName(settings_.target), conv.c_str());
        if (src != settings_.source) {
            ImGui::SameLine();
            ImGui::TextDisabled("(switched by a reset)");
        }
        ImGui::Spacing();
        if (ImGui::Button("Send reset to the module")) bridge_.sendReset();
        ImGui::SetItemTooltip("Sends the module's reset (GS reset, XG System On, GM or GM2 System On) and the forced tone map.");
        ImGui::SameLine();
        if (ImGui::Button("All notes off")) bridge_.allNotesOff();
        ImGui::SetItemTooltip("All Sound Off and All Notes Off on the 16 channels.");
        for (const std::string& e : {inError_, outError_})
            if (!e.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", e.c_str());
        ImGui::End();
        if (changed) saveIfChanged();
    }

    bool active() const { return bridge_.secondsSinceIn() < 1.0 || bridge_.secondsSinceOut() < 1.0; }

private:
    void store() {
        cfg_.set("input", inWanted_);
        cfg_.set("output", outWanted_);
        cfg_.set("source", deviceId(settings_.source));
        cfg_.set("target", deviceId(settings_.target));
        cfg_.setBool("followResets", settings_.followResets);
        cfg_.setBool("tables", settings_.useTables);
        cfg_.setInt("forceMap", settings_.forcedToneMap);
    }

    // The device lists; a chosen device that was missing is opened when it is back.
    void refreshPorts() {
        inputs_ = MidiBridge::inputDevices();
        outputs_ = bridge_.outputDevices();
        auto listed = [](const std::vector<std::string>& v, const std::string& n) { return std::find(v.begin(), v.end(), n) != v.end(); };
        if (inWanted_ != MidiBridge::kNone && bridge_.input() != inWanted_ && listed(inputs_, inWanted_)) {
            inError_.clear();
            if (!bridge_.setInput(inWanted_, &inError_)) inError_ = "Input: " + inError_;
        }
        if (outWanted_ != MidiBridge::kNone && bridge_.output() != outWanted_ && listed(outputs_, outWanted_)) {
            outError_.clear();
            if (!bridge_.setOutput(outWanted_, &outError_)) outError_ = "Output: " + outError_;
        }
    }

    // A device list; a saved device that is missing stays chosen (shown as missing).
    bool portCombo(const char* id, const std::vector<std::string>& list, std::string& wanted, const std::string& open) {
        bool missing = wanted != MidiBridge::kNone && open != wanted && std::find(list.begin(), list.end(), wanted) == list.end();
        std::string shown = missing ? wanted + " (not found)" : wanted;
        bool changed = false;
        if (ImGui::BeginCombo(id, shown.c_str())) {
            if (ImGui::IsWindowAppearing()) refreshPorts();
            for (const std::string& name : list)
                if (ImGui::Selectable(name.c_str(), name == wanted)) {
                    wanted = name;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    static bool deviceCombo(const char* id, DeviceKind& k) {
        bool changed = false;
        if (ImGui::BeginCombo(id, deviceProfile(k).name)) {
            for (int i = 0; i < int(DeviceKind::Count); i++)
                if (ImGui::Selectable(deviceProfile(DeviceKind(i)).name, DeviceKind(i) == k)) {
                    k = DeviceKind(i);
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    // A light that is lit while messages flow, and their count.
    static void activity(double since, uint64_t count, const char* what) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight(), r = h * 0.22f;
        float glow = since < 0.15 ? 1.0f : since < 0.6 ? float(1.0 - (since - 0.15) / 0.45) * 0.6f : 0.0f;
        ImU32 col = ui::LerpColor(IM_COL32(45, 60, 45, 255), IM_COL32(80, 255, 110, 255), glow);
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r + 2, p.y + h * 0.5f), r, col);
        ImGui::Dummy(ImVec2(r * 2 + 6, h));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%llu", (unsigned long long)count);
        ImGui::SetItemTooltip("%s", what);
    }

    Config cfg_;
    std::string savedText_;
    MidiBridge bridge_;
    MidiBridge::Settings settings_;
    std::vector<std::string> inputs_, outputs_;
    std::string inWanted_ = MidiBridge::kNone, outWanted_ = MidiBridge::kNone, inError_, outError_;
    std::string tablesFolder_, tablesError_;
};

struct Options {
    std::string renderer, platform, shotTrigger;
    int width = 720, height = 390;
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
            printf("usage: ImMidiBridge [--size WxH] [--platform wayland|x11] [--renderer d3d11|metal|vulkan|opengl]\n"
                   "  Converts the messages of a MIDI input for another sound module and sends them to a MIDI output.\n");
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
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "org.immidi.ImMidiBridge");
        glfwWindowHintString(GLFW_X11_CLASS_NAME, "ImMidiBridge");
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "immidibridge");
        window = glfwCreateWindow(opt.width, opt.height, "ImMidi Bridge", nullptr, nullptr);
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
    setWindowIcon(window, AppIconKind::Bridge);
    glfwSetWindowSizeLimits(window, 560, 360, GLFW_DONT_CARE, GLFW_DONT_CARE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // one fixed panel: nothing to remember
    renderer->initImGui();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowRounding = 0.0f;
    style.ItemSpacing = ImVec2(6, 5);
    style.FramePadding = ImVec2(6, 3);
    style.WindowPadding = ImVec2(12, 10);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
    style.FontScaleDpi = uiDpiScale(window);
    glfwSetWindowContentScaleCallback(window, contentScaleCallback);
    ui::LoadFonts(15.0f, "");

    {
        BridgeWindow app;
        // As ImMidi's player: at the display rate for a second after input (hover effects, tooltip
        // delays), 15 frames a second while the activity lights move, otherwise on events (the bridge
        // wakes the window when messages start to come in) and once a second.
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
            return any || ImGui::IsAnyMouseDown();
        };
        auto lastFrame = std::chrono::steady_clock::now();
        while (!glfwWindowShouldClose(window)) {
            if (glfwGetTime() - lastInput < 1.0) glfwPollEvents();
            else glfwWaitEventsTimeout(app.active() ? 1.0 / 15 : 1.0);
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            renderer->newFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame();
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
            if (sawInput()) lastInput = glfwGetTime();
            // Without vsync (virtual displays), at most about 60 frames a second.
            auto now = std::chrono::steady_clock::now();
            if (now - lastFrame < std::chrono::milliseconds(15)) std::this_thread::sleep_for(std::chrono::milliseconds(15) - (now - lastFrame));
            lastFrame = std::chrono::steady_clock::now();
        }
        app.saveIfChanged();
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

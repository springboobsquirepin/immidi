#include "Fonts.h"
#include "Util.h"
#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "RobotoMedium.inc"

namespace ui {

FontSet g_fonts;

static std::vector<std::string> cjkCandidates() {
    std::vector<std::string> v;
#if defined(_WIN32)
    const char* windir = getenv("WINDIR");
    std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    for (const char* f : {"meiryo.ttc", "YuGothM.ttc", "msgothic.ttc", "msyh.ttc", "malgun.ttf", "simsun.ttc"}) v.push_back(fonts + f);
#elif defined(__APPLE__)
    for (const char* f : {"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                          "/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                          "/Library/Fonts/Arial Unicode.ttf", "/System/Library/Fonts/AppleSDGothicNeo.ttc"})
        v.push_back(f);
#else
    for (const char* f : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
                          "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/truetype/noto/NotoSansJP-Regular.ttf", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                          "/usr/share/fonts/truetype/takao-gothic/TakaoPGothic.ttf", "/usr/share/fonts/truetype/vlgothic/VL-PGothic-Regular.ttf",
                          "/usr/share/fonts/opentype/ipafont-gothic/ipagp.ttf", "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
                          "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"})
        v.push_back(f);
    if (const char* home = getenv("HOME")) {
        v.push_back(std::string(home) + "/.local/share/fonts/NotoSansCJK-Regular.ttc");
        v.push_back(std::string(home) + "/.fonts/NotoSansCJK-Regular.ttc");
    }
#endif
    // Bundled fonts next to the executable take precedence over nothing.
    std::string exeFonts = immidi::resourceDirectory() + "/fonts";
    std::error_code ec;
    for (auto& de : std::filesystem::directory_iterator(immidi::pathFromUtf8(exeFonts), ec)) {
        std::string p = immidi::pathToUtf8(de.path());
        if (immidi::endsWithNoCase(p, ".ttf") || immidi::endsWithNoCase(p, ".otf") || immidi::endsWithNoCase(p, ".ttc")) v.push_back(p);
    }
    return v;
}

static std::vector<std::string> symbolCandidates() {
#if defined(_WIN32)
    const char* windir = getenv("WINDIR");
    std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    return {fonts + "segoeui.ttf", fonts + "arial.ttf"};
#elif defined(__APPLE__)
    return {"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"};
#else
    return {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
#endif
}

FontSet LoadFonts(float baseSize, const std::string& extraFont) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    FontSet fs;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    snprintf(cfg.Name, sizeof cfg.Name, "Roboto Medium");
    fs.main = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoMedium_compressed_data, int(RobotoMedium_compressed_size), baseSize, &cfg);

    std::error_code ec;
    auto exists = [&](const std::string& p) { return !p.empty() && std::filesystem::is_regular_file(immidi::pathFromUtf8(p), ec); };
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.OversampleH = 1;
    if (exists(extraFont) && io.Fonts->AddFontFromFileTTF(extraFont.c_str(), baseSize, &merge)) fs.cjkPath = extraFont;
    if (fs.cjkPath.empty()) {
        for (const std::string& p : cjkCandidates()) {
            if (!exists(p)) continue;
            if (io.Fonts->AddFontFromFileTTF(p.c_str(), baseSize, &merge)) {
                fs.cjkPath = p;
                break;
            }
        }
    }
    for (const std::string& p : symbolCandidates()) {
        if (!exists(p)) continue;
        io.Fonts->AddFontFromFileTTF(p.c_str(), baseSize, &merge);
        break;
    }
    ImFontConfig monoCfg;
    monoCfg.SizePixels = 13.0f;
    fs.mono = io.Fonts->AddFontDefault(&monoCfg);
    g_fonts = fs;
    return fs;
}

} // namespace ui

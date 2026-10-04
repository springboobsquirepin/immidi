#include "FileBrowser.h"
#include "Util.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace immidi {

namespace fs = std::filesystem;

void FileBrowser::open(Mode mode, const std::string& title, const std::string& startDir, const std::vector<std::string>& extensions,
                       const std::string& defaultName) {
    mode_ = mode;
    title_ = title;
    exts_ = extensions;
    open_ = true;
    justOpened_ = true;
    selection_.clear();
    error_.clear();
    snprintf(nameBuf_, sizeof nameBuf_, "%s", defaultName.c_str());
    filterBuf_[0] = 0;
    std::error_code ec;
    std::string start = startDir;
    if (start.empty() || !fs::is_directory(pathFromUtf8(start), ec)) {
        const char* home = getenv("HOME");
#if defined(_WIN32)
        if (!home) home = getenv("USERPROFILE");
#endif
        start = home ? home : pathToUtf8(fs::current_path(ec));
    }
    navigate(start);
}

bool FileBrowser::matches(const std::string& name) const {
    if (showAll_ || exts_.empty()) return true;
    for (const std::string& e : exts_)
        if (endsWithNoCase(name, e)) return true;
    return false;
}

void FileBrowser::navigate(const std::string& dir) {
    std::error_code ec;
    fs::path p = pathFromUtf8(dir);
    fs::path canon = fs::weakly_canonical(p, ec);
    if (!ec) p = canon;
    if (!fs::is_directory(p, ec)) {
        error_ = "Not a directory: " + dir;
        return;
    }
    std::vector<Entry> list;
    for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const fs::directory_entry& de = *it;
        std::error_code ec2;
        Entry e;
        e.name = pathToUtf8(de.path().filename());
        if (e.name.empty() || e.name[0] == '.') continue;
        e.path = pathToUtf8(de.path());
        e.isDir = de.is_directory(ec2);
        if (!e.isDir && !matches(e.name)) continue;
        e.size = e.isDir ? 0 : de.file_size(ec2);
        list.push_back(std::move(e));
    }
    if (ec) {
        error_ = "Cannot read directory: " + ec.message();
        if (list.empty() && !entries_.empty()) return;
    } else {
        error_.clear();
    }
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return lowerAscii(a.name) < lowerAscii(b.name);
    });
    entries_ = std::move(list);
    selected_.assign(entries_.size(), false);
    lastClicked_ = -1;
    dir_ = pathToUtf8(p);
    snprintf(pathBuf_, sizeof pathBuf_, "%s", dir_.c_str());
}

bool FileBrowser::draw() {
    if (!open_) return false;
    if (justOpened_) {
        ImGui::OpenPopup(title_.c_str());
        justOpened_ = false;
    }
    bool confirmed = false;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(900.0f, vp->WorkSize.x * 0.9f), std::min(600.0f, vp->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool keepOpen = true;
    if (ImGui::BeginPopupModal(title_.c_str(), &keepOpen)) {
        if (ImGui::Button("Up")) {
            fs::path parent = pathFromUtf8(dir_).parent_path();
            if (!parent.empty()) navigate(pathToUtf8(parent));
        }
        ImGui::SameLine();
        if (ImGui::Button("Home")) {
            const char* home = getenv("HOME");
#if defined(_WIN32)
            if (!home) home = getenv("USERPROFILE");
#endif
            if (home) navigate(home);
        }
#if defined(_WIN32)
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::BeginCombo("##drive", dir_.substr(0, 2).c_str())) {
            DWORD mask = GetLogicalDrives();
            for (int i = 0; i < 26; i++)
                if (mask & (1u << i)) {
                    char d[4] = {char('A' + i), ':', '\\', 0};
                    if (ImGui::Selectable(d)) navigate(d);
                }
            ImGui::EndCombo();
        }
#endif
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##path", pathBuf_, sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue)) navigate(pathBuf_);

        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##filter", "Filter...", filterBuf_, sizeof filterBuf_);
        ImGui::SameLine();
        if (ImGui::Checkbox("Show all files", &showAll_)) navigate(dir_);
        if (mode_ == Mode::SelectFolder) {
            ImGui::SameLine();
            ImGui::Checkbox("Include subfolders", &recursive_);
        }
        if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());

        float footer = ImGui::GetFrameHeightWithSpacing() * (mode_ == Mode::SaveFile ? 2.2f : 1.2f);
        if (ImGui::BeginTable("##files", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter,
                              ImVec2(0, -footer))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();
            std::string filter = lowerAscii(filterBuf_);
            std::string navigateTo;
            for (int i = 0; i < int(entries_.size()); i++) {
                const Entry& e = entries_[size_t(i)];
                if (!filter.empty() && lowerAscii(e.name).find(filter) == std::string::npos) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                std::string label = e.isDir ? "[" + e.name + "]" : e.name;
                bool sel = selected_[size_t(i)];
                if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (e.isDir) navigateTo = e.path;
                        else if (mode_ != Mode::SelectFolder) {
                            std::fill(selected_.begin(), selected_.end(), false);
                            selected_[size_t(i)] = true;
                            confirmed = true;
                        }
                    } else if (io.KeyShift && lastClicked_ >= 0 && mode_ == Mode::OpenFiles) {
                        int a = std::min(lastClicked_, i), b = std::max(lastClicked_, i);
                        for (int k = a; k <= b; k++)
                            if (!entries_[size_t(k)].isDir) selected_[size_t(k)] = true;
                    } else if (io.KeyCtrl && mode_ == Mode::OpenFiles) {
                        selected_[size_t(i)] = !sel;
                        lastClicked_ = i;
                    } else {
                        std::fill(selected_.begin(), selected_.end(), false);
                        selected_[size_t(i)] = true;
                        lastClicked_ = i;
                        if (!e.isDir && mode_ == Mode::SaveFile) snprintf(nameBuf_, sizeof nameBuf_, "%s", e.name.c_str());
                    }
                }
                ImGui::TableNextColumn();
                if (!e.isDir) {
                    if (e.size < 10240) ImGui::Text("%llu B", (unsigned long long)e.size);
                    else ImGui::Text("%.1f KB", double(e.size) / 1024.0);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!navigateTo.empty()) navigate(navigateTo);
        }
        if (mode_ == Mode::SaveFile) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", nameBuf_, sizeof nameBuf_, ImGuiInputTextFlags_EnterReturnsTrue)) confirmed = true;
        }
        const char* okLabel = mode_ == Mode::SelectFolder ? "Use this folder" : (mode_ == Mode::SaveFile ? "Save" : "Open");
        if (ImGui::Button(okLabel, ImVec2(140, 0))) confirmed = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            open_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (confirmed) {
            selection_.clear();
            if (mode_ == Mode::SelectFolder) {
                // A selected sub folder wins over the current directory.
                for (size_t i = 0; i < entries_.size(); i++)
                    if (selected_[i] && entries_[i].isDir) selection_.push_back(entries_[i].path);
                if (selection_.empty()) selection_.push_back(dir_);
            } else if (mode_ == Mode::SaveFile) {
                if (nameBuf_[0]) selection_.push_back(pathToUtf8(pathFromUtf8(dir_) / pathFromUtf8(nameBuf_)));
            } else {
                for (size_t i = 0; i < entries_.size(); i++)
                    if (selected_[i]) {
                        if (entries_[i].isDir) {
                            navigate(entries_[i].path);
                            selection_.clear();
                            confirmed = false;
                            break;
                        }
                        selection_.push_back(entries_[i].path);
                    }
            }
            if (confirmed && !selection_.empty()) {
                open_ = false;
                ImGui::CloseCurrentPopup();
            } else {
                confirmed = false;
            }
        }
        ImGui::EndPopup();
    }
    if (!keepOpen) open_ = false;
    return confirmed;
}

} // namespace immidi

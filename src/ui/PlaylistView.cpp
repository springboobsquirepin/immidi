#include "App.h"
#include "Util.h"
#include "Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace immidi {

// The key that removes the selected tracks. Macs also take Cmd+Backspace, as MacBook keyboards have no forward
// delete key (ImGui reports the Cmd key as Ctrl on macOS).
#if defined(__APPLE__)
static const char kRemoveKeyName[] = "Cmd+Backspace";
#else
static const char kRemoveKeyName[] = "Delete";
#endif

static bool removeKeyPressed() {
    bool pressed = ImGui::IsKeyPressed(ImGuiKey_Delete, false);
#if defined(__APPLE__)
    pressed = pressed || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
#endif
    return pressed;
}

void App::selectOnly(int index) {
    for (auto& e : playlist_.entries) e.selected = false;
    if (index >= 0 && index < int(playlist_.entries.size())) playlist_.entries[size_t(index)].selected = true;
    plCursor_ = plAnchor_ = index;
}

void App::selectRange(int a, int b, bool keepOthers) {
    if (!keepOthers)
        for (auto& e : playlist_.entries) e.selected = false;
    int n = int(playlist_.entries.size());
    for (int k = std::max(0, std::min(a, b)); k <= std::min(n - 1, std::max(a, b)); k++) playlist_.entries[size_t(k)].selected = true;
}

int App::playlistInsertIndex(const PlaylistGeometry& g, ImVec2 p) const {
    if (p.x < g.min.x || p.x >= g.max.x || p.y < g.min.y || p.y >= g.max.y) return -1;
    if (g.rows.empty()) return int(playlist_.entries.size());
    if (p.y < g.rows.front().top) return g.rows.front().index;  // header row
    for (const PlaylistRowRect& r : g.rows)
        if (p.y >= r.top && p.y < r.bottom) return p.y < (r.top + r.bottom) * 0.5f ? r.index : r.index + 1;
    return g.rows.back().index + 1;  // below the last row
}

// A thick line between two rows across the whole table, with short end caps, like the insertion
// mark of a Windows list view.
void App::drawInsertionMark(const PlaylistGeometry& g, int index) const {
    if (index < 0 || g.rows.empty()) return;
    float y = -1;
    for (const PlaylistRowRect& r : g.rows) {
        if (r.index == index) y = r.top;
        else if (r.index == index - 1 && y < 0) y = r.bottom;
    }
    if (y < 0) return;
    y = std::max(y, g.bodyTop + 1.0f);
    float scale = ImGui::GetFontSize() / 16.0f;
    float thick = std::max(2.0f, 2.5f * scale), cap = std::max(3.0f, 4.0f * scale);
    float x0 = g.min.x + 1.0f, x1 = g.bodyMaxX - 1.0f;
    ImU32 col = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(g.min, g.max, true);
    dl->AddRectFilled(ImVec2(x0, y - thick * 0.5f), ImVec2(x1, y + thick * 0.5f), col);
    dl->AddRectFilled(ImVec2(x0, y - cap), ImVec2(x0 + thick, y + cap), col);
    dl->AddRectFilled(ImVec2(x1 - thick, y - cap), ImVec2(x1, y + cap), col);
    dl->PopClipRect();
}

void App::handlePlaylistKeys(const PlaylistGeometry& g) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    int n = int(playlist_.entries.size());
    if (n == 0) return;
    int cur = plCursor_ < 0 ? -1 : std::min(plCursor_, n - 1);

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        for (auto& e : playlist_.entries) e.selected = true;
        return;
    }
    if (removeKeyPressed()) {
        std::vector<int> sel = playlist_.selectedIndices();
        if (sel.empty()) return;
        playlist_.removeEntries(sel);
        n = int(playlist_.entries.size());
        int next = std::min(sel.front(), n - 1);
        selectOnly(next);
        plScrollTo_ = next;
        return;
    }
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && cur >= 0) {
        if (io.KeyAlt) openFileInfo(cur);  // Alt+Enter: like the properties of a file
        else loadIndex(cur, true);
        return;
    }

    bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if (io.KeyCtrl && !io.KeyShift && (up || down)) {
        // Ctrl+Up/Down moves the selected entries by one place.
        std::vector<int> sel = playlist_.selectedIndices();
        if (sel.empty() && cur >= 0) {
            selectOnly(cur);
            sel = {cur};
        }
        if (sel.empty() || (up && sel.front() == 0) || (down && sel.back() == n - 1)) return;
        if (up)
            for (int i : sel) playlist_.move(i, i - 1);
        else
            for (auto it = sel.rbegin(); it != sel.rend(); ++it) playlist_.move(*it, *it + 1);
        int d = up ? -1 : 1;
        if (plCursor_ >= 0) plCursor_ = std::max(0, std::min(n - 1, plCursor_ + d));
        if (plAnchor_ >= 0) plAnchor_ = std::max(0, std::min(n - 1, plAnchor_ + d));
        plScrollTo_ = plCursor_;
        return;
    }

    int page = std::max(1, int(g.rows.size()) - 2);
    int target = -2;
    if (up) target = cur < 0 ? 0 : cur - 1;
    else if (down) target = cur < 0 ? 0 : cur + 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_Home)) target = 0;
    else if (ImGui::IsKeyPressed(ImGuiKey_End)) target = n - 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) target = cur < 0 ? 0 : cur - page;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) target = cur < 0 ? 0 : cur + page;
    if (target == -2) return;
    target = std::max(0, std::min(n - 1, target));
    if (io.KeyShift) {
        if (plAnchor_ < 0 || plAnchor_ >= n) plAnchor_ = cur < 0 ? target : cur;
        selectRange(plAnchor_, target, io.KeyCtrl);
        plCursor_ = target;
    } else if (io.KeyCtrl) {
        plCursor_ = target;  // Ctrl moves the focus without changing the selection
    } else {
        selectOnly(target);
    }
    plScrollTo_ = target;
}

void App::drawPlaylist(bool pane) {
    const int view = pane ? 1 : 0;
    PlaylistGeometry& geom = plGeom_[view];
    const float areaTop = ImGui::GetCursorScreenPos().y;
    float bh = ImGui::GetFrameHeight();
    if (ui::IconButton("add", ui::Icon::Plus, ImVec2(bh * 1.4f, bh), false, "Add files")) showFileDialog(DialogPurpose::AddFiles);
    ImGui::SameLine();
    if (ui::IconButton("addDir", ui::Icon::Folder, ImVec2(bh * 1.4f, bh), false, "Add folder")) showFileDialog(DialogPurpose::AddFolder);
    ImGui::SameLine();
    std::vector<int> selection = playlist_.selectedIndices();
    ImGui::BeginDisabled(selection.empty());
    if (ImGui::Button("Remove")) {
        playlist_.removeEntries(selection);
        selectOnly(-1);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Remove the selected tracks (%s)", kRemoveKeyName);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        player_->stop();
        playlist_.clear();
        plCursor_ = plAnchor_ = -1;
    }
    if (!pane) {
        ImGui::SameLine();
        if (ImGui::Button("Import...")) showFileDialog(DialogPurpose::ImportPlaylist);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replace the playlist with an .m3u / .m3u8 file");
        ImGui::SameLine();
        if (ImGui::Button("Append...")) showFileDialog(DialogPurpose::AppendPlaylist);
        ImGui::SameLine();
        if (ImGui::Button("Export...")) showFileDialog(DialogPurpose::ExportPlaylist);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the playlist as .m3u (or .m3u8) for other players");
    }
    ImGui::SameLine();
    selection = playlist_.selectedIndices();
    if (selection.size() > 1) ImGui::TextDisabled("%zu (%zu selected)", playlist_.entries.size(), selection.size());
    else ImGui::TextDisabled("%zu", playlist_.entries.size());

    if (plFocusView_ == view && geom.frame >= ImGui::GetFrameCount() - 1) handlePlaylistKeys(geom);

    int64_t total = 0;
    bool allKnown = true;
    for (auto& e : playlist_.entries) {
        if (e.info.durationUs > 0) total += e.info.durationUs;
        if (e.infoState != InfoState::Known) allKnown = false;
    }

    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    bool internalDrag = payload && payload->IsDataType("IMMIDI_PL");

    // Columns can be shown and hidden with a right click on the header. (A new table ID: the
    // column layout saved for the old playlist table does not fit these columns.)
    ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                         ImGuiTableFlags_Hideable;
    if (!ImGui::BeginTable("##songs", PlColumnCount, tf, ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing())))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 30);
    ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 3.0f);
    ImGui::TableSetupColumn("File name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 2.0f);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch | (pane ? ImGuiTableColumnFlags_DefaultHide : 0), 3.0f);
    drawPlaylistHeader(view);
    geom.bodyTop = ImGui::GetItemRectMax().y;
    geom.rows.clear();

    ImGuiIO& io = ImGui::GetIO();
    const bool focused = plFocusView_ == view;
    int removeIdx = -1, playIdx = -1;
    enum { None, ToTop, Up, Down, ToBottom, SelectAll } ctxAction = None;
    bool rowHovered = false;
    ImVec2 cursorMin(0, 0), cursorMax(0, 0);
    bool cursorVisible = false;
    for (int i = 0; i < int(playlist_.entries.size()); i++) {
        PlaylistEntry& e = playlist_.entries[size_t(i)];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(i);
        bool current = i == playlist_.current;
        if (current) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(60, 110, 60, 110));
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        // No hover highlight while files from another program are dragged over: the insertion mark shows the target.
        if (extDragInside_) ImGui::PushStyleColor(ImGuiCol_HeaderHovered, e.selected ? ImGui::GetColorU32(ImGuiCol_Header) : IM_COL32(0, 0, 0, 0));
        ImGui::Selectable(num, e.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
        if (extDragInside_) ImGui::PopStyleColor();
        ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        if (ImGui::IsItemVisible()) geom.rows.push_back({i, rmin.y, rmax.y});
        if (i == plCursor_) {
            cursorMin = rmin;
            cursorMax = rmax;
            cursorVisible = ImGui::IsItemVisible();
        }
        if (i == plScrollTo_) {
            // Keep the keyboard cursor inside the visible part of the list.
            float top = geom.bodyTop, bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
            if (rmin.y < top) ImGui::SetScrollY(ImGui::GetScrollY() - (top - rmin.y));
            else if (rmax.y > bottom) ImGui::SetScrollY(ImGui::GetScrollY() + (rmax.y - bottom));
            plScrollTo_ = -1;
        }
        bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hovered && !internalDrag) {
            rowHovered = true;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                playIdx = i;
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (io.KeyShift) {
                    if (plAnchor_ < 0) plAnchor_ = i;
                    selectRange(plAnchor_, i, io.KeyCtrl);
                    plCursor_ = i;
                } else if (io.KeyCtrl) {
                    e.selected = !e.selected;
                    plCursor_ = plAnchor_ = i;
                } else if (e.selected) {
                    plCollapseTo_ = i;  // might be the start of dragging the whole selection
                    plCursor_ = plAnchor_ = i;
                } else {
                    selectOnly(i);
                }
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (!e.selected) selectOnly(i);
                plCursor_ = i;
            }
        }
        if (ImGui::BeginDragDropSource()) {
            if (!e.selected) selectOnly(i);
            plCollapseTo_ = -1;
            int count = int(playlist_.selectedIndices().size());
            ImGui::SetDragDropPayload("IMMIDI_PL", &count, sizeof count);
            if (count > 1) ImGui::Text("Move %d tracks", count);
            else ImGui::Text("Move %s", e.shownTitle().c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            std::vector<int> sel = playlist_.selectedIndices();
            int n = int(playlist_.entries.size());
            if (ImGui::MenuItem("Play", "Enter")) playIdx = i;
            if (ImGui::MenuItem("File info...", "Alt+Enter")) openFileInfo(i);
            if (ImGui::BeginMenu("Song made for", !sel.empty())) {
                std::vector<std::string> paths;
                for (int k : sel) paths.push_back(playlist_.entries[size_t(k)].path);
                songModuleMenu(paths);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Move to top", nullptr, false, !sel.empty() && sel.back() >= int(sel.size()))) ctxAction = ToTop;
            if (ImGui::MenuItem("Move up", "Ctrl+Up", false, !sel.empty() && sel.front() > 0)) ctxAction = Up;
            if (ImGui::MenuItem("Move down", "Ctrl+Down", false, !sel.empty() && sel.back() < n - 1)) ctxAction = Down;
            if (ImGui::MenuItem("Move to bottom", nullptr, false, !sel.empty() && sel.front() < n - int(sel.size()))) ctxAction = ToBottom;
            ImGui::Separator();
            if (ImGui::MenuItem("Select all", "Ctrl+A")) ctxAction = SelectAll;
            char label[48];
            snprintf(label, sizeof label, sel.size() > 1 ? "Remove %zu tracks" : "Remove", sel.size());
            if (ImGui::MenuItem(label, kRemoveKeyName, false, !sel.empty())) removeIdx = i;
            ImGui::EndPopup();
        }
        // Title (the file name when the song names none), file name, length, path.
        if (ImGui::TableSetColumnIndex(PlColTitle)) {
            if (current) ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s", e.shownTitle().c_str());
            else ImGui::TextUnformatted(e.shownTitle().c_str());
        }
        if (ImGui::TableSetColumnIndex(PlColFileName)) ImGui::TextUnformatted(e.name.c_str());
        if (ImGui::TableSetColumnIndex(PlColTime)) {
            if (e.infoState == InfoState::Unreadable) {
                ImGui::TextDisabled("?");
                ImGui::SetItemTooltip("Cannot be read: %s", e.infoError.c_str());
            } else if (e.info.durationUs >= 0) {
                ImGui::TextUnformatted(formatTime(e.info.durationUs / 1e6).c_str());
            } else {
                ImGui::TextDisabled("-");
            }
        }
        if (ImGui::TableSetColumnIndex(PlColPath)) ImGui::TextDisabled("%s", e.path.c_str());
        ImGui::PopID();
    }

    // Scroll while something is dragged near the top or bottom edge of the list.
    ImVec2 dragPos = internalDrag ? io.MousePos : extDragPos_;
    if (internalDrag || extDragInside_) {
        float top = geom.bodyTop, bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
        float zone = ImGui::GetTextLineHeightWithSpacing() * 1.5f;
        float x0 = ImGui::GetWindowPos().x, x1 = x0 + ImGui::GetWindowWidth();
        if (dragPos.x >= x0 && dragPos.x < x1 && dragPos.y >= top - zone && dragPos.y < bottom + zone) {
            float speed = ImGui::GetTextLineHeightWithSpacing() * 15.0f * io.DeltaTime;
            if (dragPos.y < top + zone) ImGui::SetScrollY(ImGui::GetScrollY() - speed * std::min(1.0f, (top + zone - dragPos.y) / zone));
            else if (dragPos.y > bottom - zone) ImGui::SetScrollY(ImGui::GetScrollY() + speed * std::min(1.0f, (dragPos.y - bottom + zone) / zone));
        }
    }
    geom.bodyMaxX = ImGui::GetCurrentWindow()->InnerClipRect.Max.x;
    if (focused && cursorVisible) {
        // Focus rectangle on the keyboard cursor.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(ImVec2(cursorMin.x + 1, cursorMin.y), ImVec2(cursorMax.x - 1, cursorMax.y), ImGui::GetColorU32(ImGuiCol_NavCursor, 0.8f));
    }
    ImGui::EndTable();
    geom.min = ImGui::GetItemRectMin();
    geom.max = ImGui::GetItemRectMax();
    geom.frame = ImGui::GetFrameCount();

    // Drop target for moving entries: the whole table, with an insertion mark between rows.
    if (internalDrag && ImGui::BeginDragDropTargetCustom(ImRect(geom.min, geom.max), ImGui::GetID("##pltarget"))) {
        int at = playlistInsertIndex(geom, io.MousePos);
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IMMIDI_PL", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            drawInsertionMark(geom, at);
            if (p->IsDelivery() && at >= 0) {
                std::vector<int> sel = playlist_.selectedIndices();
                int before = int(std::count_if(sel.begin(), sel.end(), [at](int k) { return k < at; }));
                playlist_.moveEntries(sel, at);
                // Keep the moved block selected; the cursor follows its first entry.
                plCursor_ = plAnchor_ = at - before;
                plFocusView_ = view;
            }
        }
        ImGui::EndDragDropTarget();
    }
    // Files dragged in from another program.
    if (extDragInside_) drawInsertionMark(geom, playlistInsertIndex(geom, extDragPos_));

    // Mouse focus: a click inside the list gives it the keyboard, a click elsewhere takes it away. A click on a menu
    // opened from the list (a track's context menu, the columns menu) is not a click on the list, even where the menu
    // covers it: below the last row, that click would clear the selection the menu item is about to act on.
    bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    bool inside = ImGui::IsMouseHoveringRect(geom.min, geom.max, false) &&
                  ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_NoPopupHierarchy |
                                         ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (clicked && inside) {
        plFocusView_ = view;
        // A click below the last row clears the selection.
        if (!rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !geom.rows.empty() && io.MousePos.y > geom.rows.back().bottom &&
            io.MousePos.x < geom.bodyMaxX)
            selectOnly(-1);
    } else if (clicked && plFocusView_ == view && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        plFocusView_ = -1;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (plCollapseTo_ >= 0 && !internalDrag) selectOnly(plCollapseTo_);
        plCollapseTo_ = -1;
    }

    if (ctxAction != None) {
        // Applied after the loop: these reorder the entries the rows were drawn from.
        std::vector<int> sel = playlist_.selectedIndices();
        int n = int(playlist_.entries.size());
        switch (ctxAction) {
        case ToTop: playlist_.moveEntries(sel, 0); break;
        case ToBottom: playlist_.moveEntries(sel, n); break;
        case Up:
            for (int k : sel) playlist_.move(k, k - 1);
            break;
        case Down:
            for (auto it = sel.rbegin(); it != sel.rend(); ++it) playlist_.move(*it, *it + 1);
            break;
        case SelectAll:
            for (auto& x : playlist_.entries) x.selected = true;
            break;
        default: break;
        }
        sel = playlist_.selectedIndices();
        if (!sel.empty()) {
            plCursor_ = plAnchor_ = sel.front();
            plScrollTo_ = sel.front();
        }
    }
    if (removeIdx >= 0) {
        std::vector<int> sel = playlist_.selectedIndices();
        playlist_.removeEntries(sel);
        int n = int(playlist_.entries.size());
        selectOnly(sel.empty() ? -1 : std::min(sel.front(), n - 1));
    }
    if (playIdx >= 0) loadIndex(playIdx, true);
    if (size_t reading = infoReader_->pending())
        ImGui::TextDisabled("Total so far: %s (reading %zu file%s)  |  Mode: %s", formatTime(total / 1e6).c_str(), reading, reading == 1 ? "" : "s",
                            playModeName(playlist_.mode));
    else
        ImGui::TextDisabled("Total%s: %s  |  Mode: %s", allKnown ? "" : " (known)", formatTime(total / 1e6).c_str(), playModeName(playlist_.mode));
    // Files dropped anywhere in this area go into the playlist without interrupting playback.
    const ImRect& work = ImGui::GetCurrentWindow()->WorkRect;
    geom.areaMin = ImVec2(work.Min.x, areaTop);
    geom.areaMax = ImVec2(work.Max.x, ImGui::GetItemRectMax().y);
}

// The header row: like ImGui::TableHeadersRow() (a right click opens the column menu), and a click
// sorts the playlist by that column. The arrow stays while the entries are in the sorted order.
void App::drawPlaylistHeader(int view) {
    const float rowH = ImGui::TableGetHeaderRowHeight();
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, rowH);
    const float rowY = ImGui::GetCursorScreenPos().y;
    const bool sorted = plSortedOrder_ == playlist_.orderRevision;
    int clicked = -1;
    for (int c = 0; c < PlColumnCount; c++) {
        if (!ImGui::TableSetColumnIndex(c)) continue;
        const char* name = ImGui::TableGetColumnName(c);
        ImVec2 labelPos = ImGui::GetCursorScreenPos();
        ImGui::PushID(c);
        ImGui::TableHeader(name);
        ImGui::PopID();
        if (c == PlColNumber) continue;
        // A click: pressed and released over the same header, like a button (not a drag that resizes a column).
        int& pressed = plHeaderPressed_[view];
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) pressed = c;
        if (pressed == c && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) clicked = c;
            pressed = -1;
        }
        bool mark = sorted && plSortColumn_ == c;
        std::string what = lowerAscii(name);
        ImGui::SetItemTooltip("Sort the playlist by %s%s", what.c_str(), mark ? " (click again for the reverse order)" : "");
        if (mark) {
            // Where ImGui puts the arrow of its own sortable tables: after the label, inside the cell.
            const float scale = 0.65f, w = ImGui::GetFontSize() * scale;
            float x = std::min(labelPos.x + ImGui::CalcTextSize(name).x + ImGui::GetStyle().ItemInnerSpacing.x, ImGui::GetItemRectMax().x - w);
            ImGui::RenderArrow(ImGui::GetWindowDrawList(), ImVec2(x, labelPos.y), ImGui::GetColorU32(ImGuiCol_Text),
                               plSortAscending_ ? ImGuiDir_Up : ImGuiDir_Down, scale);
        }
    }
    ImVec2 mouse = ImGui::GetMousePos();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::TableGetHoveredColumn() == PlColumnCount && mouse.y >= rowY &&
        mouse.y < rowY + rowH)
        ImGui::TableOpenContextMenu(PlColumnCount);
    if (clicked >= 0) sortPlaylist(clicked);
}

void App::sortPlaylist(int column) {
    PlaylistSortKey key = column == PlColFileName ? PlaylistSortKey::FileName
                          : column == PlColTime   ? PlaylistSortKey::Duration
                          : column == PlColPath   ? PlaylistSortKey::Path
                                                  : PlaylistSortKey::Title;
    bool again = plSortColumn_ == column && plSortedOrder_ == playlist_.orderRevision;
    plSortAscending_ = again ? !plSortAscending_ : true;
    plSortColumn_ = column;
    std::vector<int> moved = playlist_.sortBy(key, plSortAscending_);
    plSortedOrder_ = playlist_.orderRevision;
    // The keyboard cursor and the Shift anchor stay on their entries.
    auto follow = [&](int& i) {
        if (i >= 0 && i < int(moved.size())) i = moved[size_t(i)];
    };
    follow(plCursor_);
    follow(plAnchor_);
    plScrollTo_ = plCursor_;
}

void App::openFileInfo(int index) {
    if (index < 0 || index >= int(playlist_.entries.size())) return;
    FileInfoWindow& w = fileInfoWin_;
    w = FileInfoWindow();
    w.path = playlist_.entries[size_t(index)].path;
    w.request = true;
    std::error_code ec;
    std::filesystem::path p = pathFromUtf8(w.path);
    w.exists = std::filesystem::is_regular_file(p, ec);
    if (!w.exists) return;
    w.size = uint64_t(std::filesystem::file_size(p, ec));
    auto ft = std::filesystem::last_write_time(p, ec);
    if (ec) return;
    // C++17 has no conversion between the file clock and the system clock: go through "now".
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - std::filesystem::file_time_type::clock::now() +
                                                                               std::chrono::system_clock::now());
    std::time_t t = std::chrono::system_clock::to_time_t(sys);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm)) w.modified = buf;
}

void App::drawFileInfo() {
    FileInfoWindow& w = fileInfoWin_;
    if (w.request) {
        ImGui::OpenPopup("File info");
        w.request = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(ImGui::GetMainViewport()->Size.x * 0.9f, FLT_MAX));
    if (!ImGui::BeginPopupModal("File info", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const PlaylistEntry* e = nullptr;
    for (const PlaylistEntry& x : playlist_.entries)
        if (x.path == w.path) {
            e = &x;
            break;
        }
    PlaylistEntry gone;  // removed from the playlist while the window is open
    if (!e) {
        gone.path = w.path;
        gone.name = fileNameOf(w.path);
        e = &gone;
    }
    const SongInfo& i = e->info;
    const bool known = e->infoState == InfoState::Known;
    if (ImGui::BeginTable("##fileinfo", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* label, const std::string& value, bool dim = false) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            if (dim) ImGui::TextDisabled("%s", value.c_str());
            else ImGui::TextUnformatted(value.c_str());
        };
        std::string pending = e->infoState == InfoState::Unreadable ? "cannot be read: " + e->infoError : "not read yet";
        row("Title", known ? (i.title.empty() ? std::string("(none in the file)") : i.title) : pending, !known || i.title.empty());
        row("File name", e->name);
        row("Folder", pathToUtf8(pathFromUtf8(e->path).parent_path()));
        if (known) {
            row("Length", i.durationUs >= 0 ? formatTime(i.durationUs / 1e6, true) : std::string("-"));
            std::string fmt = i.format;
            if (i.tracks > 0) fmt += ", " + std::to_string(i.tracks) + (i.tracks == 1 ? " track" : " tracks");
            if (i.ports > 0) fmt += ", " + std::to_string(i.ports) + (i.ports == 1 ? " port" : " ports");
            row("Format", fmt);
            if (!i.copyright.empty()) row("Copyright", i.copyright);
        }
        if (!w.exists) {
            row("Size", "the file is missing", true);
        } else {
            char size[64];
            if (w.size >= 1024) snprintf(size, sizeof size, "%.1f KiB (%llu bytes)", w.size / 1024.0, (unsigned long long)w.size);
            else snprintf(size, sizeof size, "%llu bytes", (unsigned long long)w.size);
            row("Size", size);
            if (!w.modified.empty()) row("Modified", w.modified);
        }
        ImGui::EndTable();
    }
    // The full path, selectable for copying parts of it.
    std::string path = e->path;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, ImGui::CalcTextSize(path.c_str()).x + ImGui::GetStyle().FramePadding.x * 2));
    ImGui::InputText("##path", path.data(), path.size() + 1, ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Button("Copy path")) ImGui::SetClipboardText(e->path.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        w.path.clear();
    }
    ImGui::EndPopup();
}

} // namespace immidi

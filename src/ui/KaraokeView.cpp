#include "App.h"
#include "Util.h"
#include "Widgets.h"

#include <algorithm>
#include <cstdio>

namespace immidi {

void App::drawLyrics() {
    if (!file_) {
        ImGui::TextDisabled("No file loaded.");
        return;
    }
    const std::vector<LyricLine>& lines = file_->lyrics;
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("Text size", &lyricsFontScale_, 1.0f, 4.0f, "%.1fx");
    ImGui::SameLine(0, 16);
    ImGui::Checkbox("Show all lyrics", &lyricsShowAll_);
    ImGui::SameLine(0, 16);
    if (lines.empty()) {
        ImGui::TextDisabled("This file has no lyrics (no FF 05 lyric events or .KAR text).");
    } else {
        size_t verses = 0;
        for (const LyricLine& l : lines) verses += l.paragraphStart ? 1 : 0;
        ImGui::TextDisabled("%zu lines in %zu %s, source: %s, encoding: %s", lines.size(), verses, verses == 1 ? "verse" : "verses",
                            file_->lyricsFromKar ? "KAR text events" : "lyric events", textEncodingName(file_->encoding));
    }
    ImGui::Separator();
    if (lines.empty()) {
        // Fall back to showing text/marker events around the current position.
        ImGui::BeginChild("##texts");
        for (const TextItem& t : file_->texts) {
            if (t.type != 1 && t.type != 6 && t.type != 5) continue;
            bool past = t.timeUs <= snap_->positionUs;
            ImGui::TextDisabled("%s", formatTime(t.timeUs / 1e6).c_str());
            ImGui::SameLine(80);
            if (past) ImGui::TextUnformatted(t.text.c_str());
            else ImGui::TextDisabled("%s", t.text.c_str());
        }
        ImGui::EndChild();
        return;
    }

    int64_t now = snap_->positionUs;
    int cur = -1;
    for (int i = 0; i < int(lines.size()); i++) {
        if (lines[size_t(i)].startUs() <= now) cur = i;
        else break;
    }

    // When a syllable has been sung: at the next syllable. A line's last one (a whole phrase, in songs that
    // put one in each lyric event) takes as long as its text to sing, until the next line at most or the
    // blank lyric that ends it.
    auto sylEnd = [&](int li, int si) -> int64_t {
        const LyricLine& l = lines[size_t(li)];
        const LyricSyllable& s = l.syllables[size_t(si)];
        if (si + 1 < int(l.syllables.size())) return l.syllables[size_t(si + 1)].timeUs;
        int64_t end = s.timeUs + std::max<int64_t>(600000, int64_t(lyricColumns(s.text)) * 100000);
        if (l.endUs > s.timeUs) end = std::min(end, l.endUs);
        if (li + 1 < int(lines.size())) end = std::min(end, lines[size_t(li + 1)].startUs());
        return end;
    };

    const ImU32 sung = IM_COL32(90, 200, 255, 255);
    const ImU32 unsung = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    // A line in rows that fit the view, each centred. It wraps after a space (ASCII or full-width), and text
    // wider than the view wraps between its characters, as Japanese text does.
    struct Piece {
        int syl;
        std::string text;
        float w, ink, at;  // width, width without its trailing spaces, where it starts within its syllable
    };
    struct Row {
        size_t first, last;
        float ink;
    };
    auto drawLine = [&](int li, bool active, float scale) {
        const LyricLine& l = lines[size_t(li)];
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
        const float avail = ImGui::GetContentRegionAvail().x;
        std::vector<Piece> pieces;
        std::vector<float> sylW(l.syllables.size(), 0.0f);
        auto add = [&](int si, const std::string& t) {
            size_t n = t.size();
            while (n && t[n - 1] == ' ') n--;
            if (n >= 3 && t.compare(n - 3, 3, "\xE3\x80\x80") == 0) n -= 3;  // U+3000, the full-width space
            float w = ImGui::CalcTextSize(t.c_str()).x;
            pieces.push_back({si, t, w, ImGui::CalcTextSize(t.c_str(), t.c_str() + n).x, sylW[size_t(si)]});
            sylW[size_t(si)] += w;
        };
        for (int si = 0; si < int(l.syllables.size()); si++) {
            const std::string& t = l.syllables[size_t(si)].text;
            size_t a = 0;
            for (size_t i = 0; i <= t.size();) {
                size_t sp = i < t.size() && t[i] == ' ' ? 1 : t.compare(i, 3, "\xE3\x80\x80") == 0 ? 3 : 0;
                if (i < t.size() && !sp) {
                    i++;
                    continue;
                }
                i += sp ? sp : 1;
                std::string word = t.substr(a, std::min(i, t.size()) - a);
                a = i;
                if (word.empty()) continue;
                if (ImGui::CalcTextSize(word.c_str()).x <= avail) {
                    add(si, word);
                    continue;
                }
                for (size_t c = 0; c < word.size();) {  // character by character
                    uint8_t b = uint8_t(word[c]);
                    size_t len = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
                    add(si, word.substr(c, len));
                    c += len;
                }
            }
        }
        std::vector<Row> rows;
        float x = 0;
        size_t first = 0;
        for (size_t i = 0; i < pieces.size(); i++) {
            if (i > first && x + pieces[i].ink > avail) {
                rows.push_back({first, i, x - (pieces[i - 1].w - pieces[i - 1].ink)});
                first = i;
                x = 0;
            }
            x += pieces[i].w;
        }
        if (!pieces.empty()) rows.push_back({first, pieces.size(), x - (pieces.back().w - pieces.back().ink)});
        // How much of each syllable has been sung.
        std::vector<float> sungW(l.syllables.size(), 0.0f);
        if (active)
            for (int si = 0; si < int(l.syllables.size()); si++) {
                const LyricSyllable& s = l.syllables[size_t(si)];
                if (now < s.timeUs) continue;
                int64_t end = sylEnd(li, si);
                float frac = end > s.timeUs ? std::clamp(float(double(now - s.timeUs) / double(end - s.timeUs)), 0.0f, 1.0f) : 1.0f;
                sungW[size_t(si)] = frac * sylW[size_t(si)];
            }
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = pos.y;
        for (const Row& r : rows) {
            float px = pos.x + std::max(0.0f, (avail - r.ink) * 0.5f);
            for (size_t i = r.first; i < r.last; i++) {
                const Piece& p = pieces[i];
                const ImVec2 tp(px, y);
                if (!active) {
                    dl->AddText(tp, li < cur ? dim : unsung, p.text.c_str());
                } else {
                    float done = sungW[size_t(p.syl)] - p.at;
                    dl->AddText(ImVec2(tp.x + 2, tp.y + 2), IM_COL32(0, 0, 0, 150), p.text.c_str());
                    dl->AddText(tp, unsung, p.text.c_str());
                    if (done > 0.0f) {
                        dl->PushClipRect(tp, ImVec2(tp.x + std::min(done, p.w), tp.y + lh), true);
                        dl->AddText(tp, sung, p.text.c_str());
                        dl->PopClipRect();
                    }
                }
                px += p.w;
            }
            y += lh;
        }
        ImGui::Dummy(ImVec2(avail, std::max(lh, y - pos.y)));
        ImGui::PopFont();
    };

    if (lyricsShowAll_) {
        ImGui::BeginChild("##alllyrics");
        const float scale = std::max(1.0f, lyricsFontScale_ * 0.6f);
        for (int i = 0; i < int(lines.size()); i++) {
            if (lines[size_t(i)].paragraphStart && i) ImGui::Dummy(ImVec2(0, ImGui::GetStyle().FontSizeBase * scale * 0.8f));  // between verses
            drawLine(i, i == cur, scale);
            if (i == cur && snap_->playing) ImGui::SetScrollHereY(0.4f);
        }
        ImGui::EndChild();
        return;
    }

    // Karaoke page: previous line, current line, and the upcoming lines.
    ImGui::BeginChild("##karaoke", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    float lineH = ImGui::GetStyle().FontSizeBase * lyricsFontScale_ * 1.35f;
    float top = std::max(0.0f, (ImGui::GetContentRegionAvail().y - lineH * 5) * 0.35f);
    ImGui::Dummy(ImVec2(0, top));
    int first = std::max(0, cur - 1);
    if (cur < 0) first = 0;
    for (int i = first; i < std::min<int>(int(lines.size()), first + 5); i++) {
        if (i > first && lines[size_t(i)].paragraphStart) ImGui::Dummy(ImVec2(0, lineH * 0.4f));  // a new verse
        bool active = (i == cur) || (cur < 0 && i == 0);
        float scale = active ? lyricsFontScale_ : lyricsFontScale_ * 0.8f;
        drawLine(i, active, scale);
        ImGui::Dummy(ImVec2(0, 6));
    }
    ImGui::EndChild();
}

} // namespace immidi

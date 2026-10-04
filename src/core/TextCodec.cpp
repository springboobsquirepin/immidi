#include "TextCodec.h"

namespace immidi {

#include "SjisTable.inc"

const char* textEncodingName(TextEncoding e) {
    switch (e) {
    case TextEncoding::Auto: return "Auto";
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::ShiftJis: return "Shift-JIS (CP932)";
    case TextEncoding::Latin1: return "Latin-1 (CP1252)";
    }
    return "?";
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

bool isValidUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
        i += len;
    }
    return true;
}

static int sjisLeadIndex(uint8_t b) {
    if (b >= 0x81 && b <= 0x9F) return b - 0x81;
    if (b >= 0xE0 && b <= 0xFC) return b - 0xE0 + 31;
    return -1;
}

static uint32_t sjisLookup(uint8_t lead, uint8_t trail) {
    int li = sjisLeadIndex(lead);
    if (li < 0 || trail < 0x40 || trail > 0xFC) return 0;
    return kSjisTable[li * 189 + (trail - 0x40)];
}

bool looksLikeShiftJis(const uint8_t* p, size_t n) {
    // Requires at least one double-byte character: text whose only high bytes are
    // isolated A1-DF bytes is far more likely Latin-1 (e.g. a lone "\xA9" copyright sign).
    bool doubleByte = false;
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        if (c >= 0xA1 && c <= 0xDF) { i++; continue; }
        if (sjisLeadIndex(c) >= 0) {
            if (i + 1 >= n) return false;
            if (!sjisLookup(c, p[i + 1])) return false;
            doubleByte = true;
            i += 2;
            continue;
        }
        return false;
    }
    return doubleByte;
}

TextEncoding detectEncoding(const std::vector<std::string>& rawTexts) {
    bool allUtf8 = true, allSjis = true, anyHigh = false, anySjisSpecific = false;
    for (const std::string& s : rawTexts) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
        bool high = false;
        for (unsigned char c : s) if (c >= 0x80) { high = true; break; }
        if (!high) continue;
        anyHigh = true;
        if (!isValidUtf8(p, s.size())) allUtf8 = false;
        if (looksLikeShiftJis(p, s.size())) anySjisSpecific = true;
        else allSjis = false;
    }
    if (!anyHigh) return TextEncoding::Utf8;
    if (allUtf8) return TextEncoding::Utf8;
    if (allSjis && anySjisSpecific) return TextEncoding::ShiftJis;
    // Mixed content: if most strings decode as Shift-JIS, prefer it.
    if (anySjisSpecific) return TextEncoding::ShiftJis;
    return TextEncoding::Latin1;
}

static const uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

static std::string decodeLatin1(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n + n / 4);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c < 0x80) out += char(c);
        else if (c < 0xA0) appendUtf8(out, kCp1252High[c - 0x80]);
        else appendUtf8(out, c);
    }
    return out;
}

static std::string decodeSjis(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n * 2);
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { out += char(c); i++; continue; }
        if (c >= 0xA1 && c <= 0xDF) { appendUtf8(out, 0xFF61 + (c - 0xA1)); i++; continue; }
        if (sjisLeadIndex(c) >= 0 && i + 1 < n) {
            uint32_t u = sjisLookup(c, p[i + 1]);
            if (u) { appendUtf8(out, u); i += 2; continue; }
        }
        appendUtf8(out, 0xFFFD);
        i++;
    }
    return out;
}

static std::string sanitizeUtf8(const uint8_t* p, size_t n) {
    return std::string(reinterpret_cast<const char*>(p), n);
}

std::string decodeText(const uint8_t* p, size_t n, TextEncoding enc) {
    // Strip trailing NULs that some sequencers write.
    while (n > 0 && p[n - 1] == 0) n--;
    switch (enc) {
    case TextEncoding::Utf8:
        if (isValidUtf8(p, n)) return sanitizeUtf8(p, n);
        return decodeLatin1(p, n);
    case TextEncoding::ShiftJis:
        return decodeSjis(p, n);
    case TextEncoding::Latin1:
        return decodeLatin1(p, n);
    case TextEncoding::Auto:
    default:
        if (isValidUtf8(p, n)) return sanitizeUtf8(p, n);
        if (looksLikeShiftJis(p, n)) return decodeSjis(p, n);
        return decodeLatin1(p, n);
    }
}

} // namespace immidi

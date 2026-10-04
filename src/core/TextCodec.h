#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace immidi {

enum class TextEncoding { Auto = 0, Utf8, ShiftJis, Latin1 };

const char* textEncodingName(TextEncoding e);

bool isValidUtf8(const uint8_t* p, size_t n);
// True when every byte sequence is valid CP932 and at least one double-byte or
// half-width katakana character is present.
bool looksLikeShiftJis(const uint8_t* p, size_t n);

// Picks one encoding for a whole file from all of its text chunks.
TextEncoding detectEncoding(const std::vector<std::string>& rawTexts);

// Converts raw bytes to UTF-8. Auto decides per string.
std::string decodeText(const uint8_t* p, size_t n, TextEncoding enc);
inline std::string decodeText(const std::string& s, TextEncoding enc) {
    return decodeText(reinterpret_cast<const uint8_t*>(s.data()), s.size(), enc);
}

void appendUtf8(std::string& out, uint32_t cp);

} // namespace immidi

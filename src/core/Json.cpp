#include "Json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace immidi {

const JsonValue& JsonValue::null() {
    static const JsonValue v;
    return v;
}

const JsonValue& JsonValue::operator[](const char* key) const {
    if (type_ == Type::Object)
        for (auto& [k, v] : items_)
            if (k == key) return v;
    return null();
}

bool JsonValue::has(const char* key) const {
    if (type_ == Type::Object)
        for (auto& [k, v] : items_)
            if (k == key) return true;
    return false;
}

JsonValue JsonValue::makeBool(bool b) {
    JsonValue v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

JsonValue JsonValue::makeNumber(double n) {
    JsonValue v;
    v.type_ = Type::Number;
    v.number_ = n;
    return v;
}

JsonValue JsonValue::makeString(std::string s) {
    JsonValue v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}

JsonValue JsonValue::makeArray() {
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}

JsonValue JsonValue::makeObject() {
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

JsonValue* JsonValue::find(const std::string& key) {
    if (type_ == Type::Object)
        for (auto& [k, v] : items_)
            if (k == key) return &v;
    return nullptr;
}

JsonValue& JsonValue::set(const std::string& key, JsonValue v, std::initializer_list<const char*> order) {
    if (type_ != Type::Object) {
        *this = makeObject();
    }
    if (JsonValue* e = find(key)) {
        *e = std::move(v);
        return *e;
    }
    // Before the first member that comes after `key` in `order`.
    size_t at = items_.size();
    bool after = false;
    for (const char* o : order) {
        if (after) {
            for (size_t i = 0; i < items_.size(); i++)
                if (items_[i].first == o && i < at) at = i;
        }
        if (key == o) after = true;
    }
    items_.insert(items_.begin() + long(at), {key, std::move(v)});
    return items_[at].second;
}

bool JsonValue::remove(const std::string& key) {
    for (size_t i = 0; i < items_.size(); i++)
        if (items_[i].first == key) {
            items_.erase(items_.begin() + long(i));
            return true;
        }
    return false;
}

namespace {

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}

void writeValue(std::string& out, const JsonValue& v) {
    switch (v.type()) {
    case JsonValue::Type::Null: out += "null"; break;
    case JsonValue::Type::Bool: out += v.boolean() ? "true" : "false"; break;
    case JsonValue::Type::Number: {
        double d = v.number();
        char b[40];
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) snprintf(b, sizeof b, "%.0f", d);
        else snprintf(b, sizeof b, "%.17g", d);
        out += b;
        break;
    }
    case JsonValue::Type::String: writeString(out, v.string()); break;
    case JsonValue::Type::Array:
        out += '[';
        for (size_t i = 0; i < v.size(); i++) {
            if (i) out += ", ";
            writeValue(out, v[i]);
        }
        out += ']';
        break;
    case JsonValue::Type::Object: {
        out += '{';
        bool first = true;
        for (auto& [k, e] : v.items()) {
            if (!first) out += ", ";
            first = false;
            writeString(out, k);
            out += ": ";
            writeValue(out, e);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

std::string jsonText(const JsonValue& v) {
    std::string out;
    writeValue(out, v);
    return out;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : s_(t.data()), end_(t.data() + t.size()), begin_(t.data()) {}

    bool document(JsonValue& out, std::string& error) {
        if (end_ - s_ >= 3 && std::memcmp(s_, "\xEF\xBB\xBF", 3) == 0) s_ += 3;  // UTF-8 byte order mark
        bool ok = value(out, 0);
        if (ok) {
            space();
            if (s_ != end_) ok = fail("unexpected text after the value");
        }
        if (!ok) {
            int line = 1;
            for (const char* p = begin_; p < errorAt_ && p < end_; p++)
                if (*p == '\n') line++;
            error = "line " + std::to_string(line) + ": " + error_;
        }
        return ok;
    }

private:
    const char* s_;
    const char* end_;
    const char* begin_;
    const char* errorAt_ = nullptr;
    std::string error_;

    bool fail(const char* what) {
        if (!errorAt_) {
            errorAt_ = s_;
            error_ = what;
        }
        return false;
    }

    void space() {
        while (s_ < end_ && (*s_ == ' ' || *s_ == '\t' || *s_ == '\n' || *s_ == '\r')) s_++;
    }

    bool literal(const char* word) {
        size_t n = std::strlen(word);
        if (size_t(end_ - s_) < n || std::memcmp(s_, word, n) != 0) return fail("invalid value");
        s_ += n;
        return true;
    }

    bool value(JsonValue& v, int depth) {
        if (depth > 200) return fail("nested too deeply");
        space();
        if (s_ == end_) return fail("unexpected end of text");
        switch (*s_) {
        case '{': return object(v, depth);
        case '[': return array(v, depth);
        case '"':
            v.type_ = JsonValue::Type::String;
            return string(v.string_);
        case 't':
            v.type_ = JsonValue::Type::Bool;
            v.bool_ = true;
            return literal("true");
        case 'f':
            v.type_ = JsonValue::Type::Bool;
            v.bool_ = false;
            return literal("false");
        case 'n':
            v.type_ = JsonValue::Type::Null;
            return literal("null");
        default: return number(v);
        }
    }

    bool object(JsonValue& v, int depth) {
        v.type_ = JsonValue::Type::Object;
        s_++;
        space();
        if (s_ < end_ && *s_ == '}') {
            s_++;
            return true;
        }
        for (;;) {
            space();
            if (s_ == end_ || *s_ != '"') return fail("expected a member name");
            v.items_.emplace_back();
            if (!string(v.items_.back().first)) return false;
            space();
            if (s_ == end_ || *s_ != ':') return fail("expected ':'");
            s_++;
            if (!value(v.items_.back().second, depth + 1)) return false;
            space();
            if (s_ < end_ && *s_ == ',') {
                s_++;
                continue;
            }
            if (s_ < end_ && *s_ == '}') {
                s_++;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(JsonValue& v, int depth) {
        v.type_ = JsonValue::Type::Array;
        s_++;
        space();
        if (s_ < end_ && *s_ == ']') {
            s_++;
            return true;
        }
        for (;;) {
            v.items_.emplace_back();
            if (!value(v.items_.back().second, depth + 1)) return false;
            space();
            if (s_ < end_ && *s_ == ',') {
                s_++;
                continue;
            }
            if (s_ < end_ && *s_ == ']') {
                s_++;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    // Locale-independent (strtod would follow the C locale's decimal point).
    bool number(JsonValue& v) {
        const char* p = s_;
        bool neg = false;
        if (p < end_ && *p == '-') {
            neg = true;
            p++;
        }
        if (p == end_ || *p < '0' || *p > '9') return fail("invalid value");
        double x = 0;
        while (p < end_ && *p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
        if (p < end_ && *p == '.') {
            p++;
            if (p == end_ || *p < '0' || *p > '9') return fail("invalid number");
            double f = 0.1;
            while (p < end_ && *p >= '0' && *p <= '9') {
                x += (*p++ - '0') * f;
                f /= 10;
            }
        }
        if (p < end_ && (*p == 'e' || *p == 'E')) {
            p++;
            bool eneg = false;
            if (p < end_ && (*p == '+' || *p == '-')) eneg = *p++ == '-';
            if (p == end_ || *p < '0' || *p > '9') return fail("invalid number");
            int e = 0;
            for (; p < end_ && *p >= '0' && *p <= '9'; p++)
                if (e < 1000) e = e * 10 + (*p - '0');
            double m = 1;
            for (int i = 0; i < e; i++) m *= 10;
            x = eneg ? x / m : x * m;
        }
        v.type_ = JsonValue::Type::Number;
        v.number_ = neg ? -x : x;
        s_ = p;
        return true;
    }

    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }

    bool hex4(uint32_t& c) {
        if (end_ - s_ < 4) return fail("invalid \\u escape");
        c = 0;
        for (int i = 0; i < 4; i++) {
            char h = *s_++;
            c <<= 4;
            if (h >= '0' && h <= '9') c |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') c |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') c |= uint32_t(h - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        s_++;  // opening quote
        for (;;) {
            if (s_ == end_) return fail("unterminated string");
            char ch = *s_++;
            if (ch == '"') return true;
            if (uint8_t(ch) < 0x20) return fail("control character in a string");
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (s_ == end_) return fail("unterminated string");
            switch (*s_++) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t c;
                if (!hex4(c)) return false;
                if (c >= 0xD800 && c <= 0xDBFF && end_ - s_ >= 6 && s_[0] == '\\' && s_[1] == 'u') {
                    s_ += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    else c = 0xFFFD;
                } else if (c >= 0xD800 && c <= 0xDFFF) {
                    c = 0xFFFD;
                }
                utf8(out, c);
                break;
            }
            default: return fail("invalid escape in a string");
            }
        }
    }
};

bool parseJson(const std::string& text, JsonValue& out, std::string& error) {
    out = JsonValue();
    JsonParser p(text);
    if (p.document(out, error)) return true;
    out = JsonValue();
    return false;
}

} // namespace immidi

#include "ConversionDoc.h"
#include "Util.h"

#include <cstring>
#include <vector>

namespace immidi {

namespace {

std::string quoted(const std::string& s) { return jsonText(JsonValue::makeString(s)); }

bool isSection(const std::string& k) { return k == "voices" || k == "drumKits" || k == "noteMaps" || k == "rules"; }

// Elements one per line with `indent`, joined by ",\n" (nothing between the brackets' lines when empty).
std::string lines(const JsonValue& list, const char* indent) {
    std::string out;
    for (size_t i = 0; i < list.size(); i++) {
        if (i) out += ",\n";
        out += indent + jsonText(list[i]);
    }
    return out;
}

} // namespace

bool ConversionDoc::parse(const std::string& text, std::string& error) {
    JsonValue v;
    if (!parseJson(text, v, error)) return false;
    if (!v.isObject()) {
        error = "not a JSON object";
        return false;
    }
    root_ = std::move(v);
    return true;
}

bool ConversionDoc::load(const std::string& path, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) {
        error = "cannot read the file";
        return false;
    }
    return parse(std::string(bytes.begin(), bytes.end()), error);
}

// The layout of the tables in conversion/: the header members one per line, then the voices and drum
// kits one per line, the note maps with one note entry per line, and the rule sets with one rule per line.
std::string ConversionDoc::serialize() const {
    std::string out = "{\n";
    for (auto& [k, v] : root_.items())
        if (!isSection(k)) out += "  " + quoted(k) + ": " + jsonText(v) + ",\n";
    for (const char* sec : {"voices", "drumKits"}) {
        out += std::string("  \"") + sec + "\": [\n";
        out += lines(root_[sec], "    ");
        out += "\n  ],\n";
    }
    out += "  \"noteMaps\": {\n";
    bool first = true;
    for (auto& [name, list] : root_["noteMaps"].items()) {
        if (!first) out += ",\n";
        first = false;
        out += "    " + quoted(name) + ": [\n" + lines(list, "      ") + "\n    ]";
    }
    out += "\n  },\n";
    out += "  \"rules\": {\n";
    first = true;
    for (auto& [name, set] : root_["rules"].items()) {
        if (!first) out += ",\n";
        first = false;
        std::vector<std::string> parts;
        for (const char* k : {"controllers", "nrpn"})
            if (set.has(k)) parts.push_back(std::string("      \"") + k + "\": [\n" + lines(set[k], "        ") + "\n      ]");
        for (auto& [k, v] : set.items())
            if (k != "controllers" && k != "nrpn") parts.push_back("      " + quoted(k) + ": " + jsonText(v));
        out += "    " + quoted(name) + ": {\n";
        for (size_t i = 0; i < parts.size(); i++) out += (i ? ",\n" : "") + parts[i];
        out += "\n    }";
    }
    out += "\n  }\n}\n";
    return out;
}

} // namespace immidi

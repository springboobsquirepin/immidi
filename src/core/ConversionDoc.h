#pragma once
#include "Json.h"

#include <string>

namespace immidi {

// A conversion table file (conversion/*.json) as a document to edit: kept with its members in order and
// written in the layout of ImMidi's tables (one entry per line), so that a file read and written again
// stays the same byte for byte, and an edit changes only its own lines.
class ConversionDoc {
public:
    bool parse(const std::string& text, std::string& error);
    bool load(const std::string& path, std::string& error);
    std::string serialize() const;

    JsonValue& root() { return root_; }
    const JsonValue& root() const { return root_; }

private:
    JsonValue root_;
};

} // namespace immidi

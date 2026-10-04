#pragma once
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace immidi {

// A parsed JSON document (RFC 8259). Reading a missing member or element gives a null value, so
// lookups can be chained without checks: doc["voices"][3]["to"]["pc"].integer().
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double number(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    // The number rounded towards zero; `fallback` for anything else.
    int integer(int fallback = 0) const { return type_ == Type::Number ? int(number_) : fallback; }
    const std::string& string() const { return string_; }  // empty unless a string

    size_t size() const { return items_.size(); }  // array elements or object members
    const JsonValue& operator[](size_t i) const { return i < items_.size() ? items_[i].second : null(); }
    const JsonValue& operator[](const char* key) const;
    bool has(const char* key) const;
    // Arrays: ("", value) pairs; objects: (name, value) pairs in document order.
    const std::vector<std::pair<std::string, JsonValue>>& items() const { return items_; }

    static const JsonValue& null();

    // Building and editing (the conversion table editor). Object members keep their order.
    static JsonValue makeBool(bool b);
    static JsonValue makeNumber(double v);
    static JsonValue makeString(std::string s);
    static JsonValue makeArray();
    static JsonValue makeObject();
    // Objects: the member `key`, or null when there is none.
    JsonValue* find(const std::string& key);
    // Objects: sets the member `key`. A new member goes before the first member that `order` lists
    // after it, else at the end.
    JsonValue& set(const std::string& key, JsonValue v, std::initializer_list<const char*> order = {});
    bool remove(const std::string& key);
    // Arrays and objects: the elements / members, to edit in place.
    std::vector<std::pair<std::string, JsonValue>>& mutableItems() { return items_; }
    JsonValue& at(size_t i) { return items_[i].second; }
    void append(JsonValue v) { items_.emplace_back(std::string(), std::move(v)); }

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<std::pair<std::string, JsonValue>> items_;
};

// Parses UTF-8 JSON text. On failure returns false and describes the problem (with the line) in `error`.
bool parseJson(const std::string& text, JsonValue& out, std::string& error);

// A value as compact JSON text, as Python's json.dumps writes it (", " and ": " between items, UTF-8
// kept, integers without a fraction).
std::string jsonText(const JsonValue& v);

} // namespace immidi

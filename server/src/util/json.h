// Minimal JSON, sized for a control protocol rather than for general use.
//
// Supports the whole syntax (objects, arrays, strings with escapes, numbers,
// true/false/null) but keeps the value model deliberately simple.  Parsing is
// bounded in depth so a hostile client cannot blow the stack with nested
// brackets.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fernsdr {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json make_object() { Json j; j.type_ = Type::Object; return j; }
    static Json make_array() { Json j; j.type_ = Type::Array; return j; }

    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(double v) : type_(Type::Number), number_(v) {}
    Json(int v) : type_(Type::Number), number_(v) {}
    Json(long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Json(const char* v) : type_(Type::String), string_(v) {}
    Json(std::string v) : type_(Type::String), string_(std::move(v)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_bool() const { return type_ == Type::Bool; }

    // Accessors with defaults: a control protocol should treat a missing or
    // wrong-typed field as "not specified", not as a fatal error.
    double number(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    const std::string& string(const std::string& fallback = empty_string()) const {
        return type_ == Type::String ? string_ : fallback;
    }

    // Object member lookup; returns a null Json when absent.
    const Json& operator[](const std::string& key) const;
    // Array element; returns null Json when out of range.
    const Json& operator[](size_t index) const;
    size_t size() const;
    bool has(const std::string& key) const;

    void set(const std::string& key, Json value);
    void push_back(Json value);

    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }
    const std::vector<Json>& elements() const { return elements_; }

    std::string serialize() const;

    // Returns false on malformed input.  Never throws.
    static bool parse(const std::string& text, Json& out);
    // The same, saying why in `reason` when it fails, for input a person
    // wrote: a theme or settings file that fails to load should say what to fix.
    static bool parse(const std::string& text, Json& out, std::string& reason);

private:
    friend struct JsonParser;
    static const std::string& empty_string();
    static const Json& null_value();
    void serialize_into(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> elements_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace fernsdr

#include "json.h"
#include "utf8.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace fernsdr {

namespace {

// Bounded so deeply nested input cannot exhaust the stack.
constexpr int kMaxDepth = 32;
// Members of one object. The duplicate check is a hash set, and a hash can be
// made to collide by someone who picks the keys; bounded, even that costs
// little. Nothing this server reads has objects near this size.
constexpr size_t kMaxObjectMembers = 1024;

}  // namespace

struct JsonParser {
    const std::string& text;
    size_t pos = 0;
    // Set when a failure has a reason worth telling a person; empty for plain
    // syntax errors.
    std::string reason;

    void skip_whitespace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++;
            else break;
        }
    }

    bool literal(const char* word) {
        const size_t length = std::char_traits<char>::length(word);
        if (text.compare(pos, length, word) != 0) return false;
        pos += length;
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos >= text.size() || text[pos] != '"') return false;
        pos++;
        out.clear();
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"') return true;
            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20) return false;
                out += c;
                continue;
            }
            if (pos >= text.size()) return false;
            const char escape = text[pos++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (pos + 4 > text.size()) return false;
                    unsigned code = 0;
                    for (int i = 0; i < 4; i++) {
                        const char h = text[pos++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    // Encode as UTF-8.  Surrogate halves are passed through as
                    // the replacement character rather than rejected.
                    if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parse_value(Json& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip_whitespace();
        if (pos >= text.size()) return false;

        const char c = text[pos];
        if (c == '{') return parse_object(out, depth);
        if (c == '[') return parse_array(out, depth);
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            out = Json(s);
            return true;
        }
        if (literal("true")) { out = Json(true); return true; }
        if (literal("false")) { out = Json(false); return true; }
        if (literal("null")) { out = Json(); return true; }

        // strtod also accepts hexadecimal numbers, leading plus signs and
        // missing fractional digits. Scan JSON's grammar before conversion.
        const size_t start = pos;
        if (text[pos] == '-') ++pos;
        if (pos >= text.size()) return false;
        const auto digit = [&]() { return pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; };
        if (text[pos] == '0') ++pos;
        else {
            if (!digit()) return false;
            while (digit()) ++pos;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            if (!digit()) return false;
            while (digit()) ++pos;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            if (!digit()) return false;
            while (digit()) ++pos;
        }
        const char* begin = text.c_str() + start;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end != text.c_str() + pos) return false;
        if (!std::isfinite(value)) return false;
        out = Json(value);
        return true;
    }

    // A repeated key is refused rather than resolved. Taking the last one is
    // what set() would do, but set() finds the earlier one by walking every
    // member, so an object of n keys cost n squared comparisons: 7,000 keys in
    // one 64 kB listener message held the network thread for 32 ms. And a
    // document whose meaning depends on which duplicate a reader believes is
    // not one worth accepting from anyone.
    bool parse_object(Json& out, int depth) {
        out = Json::make_object();
        pos++;  // '{'
        skip_whitespace();
        if (pos < text.size() && text[pos] == '}') { pos++; return true; }
        std::unordered_set<std::string> keys;
        while (true) {
            skip_whitespace();
            std::string key;
            if (!parse_string(key)) return false;
            skip_whitespace();
            if (pos >= text.size() || text[pos] != ':') return false;
            pos++;
            Json value;
            if (!parse_value(value, depth + 1)) return false;
            if (!keys.insert(key).second) {
                reason = "the key \"" + key + "\" appears twice in one object";
                return false;
            }
            if (keys.size() > kMaxObjectMembers) {
                reason = "an object has more than " + std::to_string(kMaxObjectMembers) + " members";
                return false;
            }
            out.members_.emplace_back(std::move(key), std::move(value));
            skip_whitespace();
            if (pos >= text.size()) return false;
            if (text[pos] == ',') { pos++; continue; }
            if (text[pos] == '}') { pos++; return true; }
            return false;
        }
    }

    bool parse_array(Json& out, int depth) {
        out = Json::make_array();
        pos++;  // '['
        skip_whitespace();
        if (pos < text.size() && text[pos] == ']') { pos++; return true; }
        while (true) {
            Json value;
            if (!parse_value(value, depth + 1)) return false;
            out.push_back(std::move(value));
            skip_whitespace();
            if (pos >= text.size()) return false;
            if (text[pos] == ',') { pos++; continue; }
            if (text[pos] == ']') { pos++; return true; }
            return false;
        }
    }
};

const std::string& Json::empty_string() {
    static const std::string value;
    return value;
}

const Json& Json::null_value() {
    static const Json value;
    return value;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return null_value();
    for (const auto& member : members_) {
        if (member.first == key) return member.second;
    }
    return null_value();
}

const Json& Json::operator[](size_t index) const {
    if (type_ != Type::Array || index >= elements_.size()) return null_value();
    return elements_[index];
}

size_t Json::size() const {
    if (type_ == Type::Array) return elements_.size();
    if (type_ == Type::Object) return members_.size();
    return 0;
}

bool Json::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& member : members_) {
        if (member.first == key) return true;
    }
    return false;
}

void Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) { type_ = Type::Object; members_.clear(); }
    for (auto& member : members_) {
        if (member.first == key) { member.second = std::move(value); return; }
    }
    members_.emplace_back(key, std::move(value));
}

void Json::push_back(Json value) {
    if (type_ != Type::Array) { type_ = Type::Array; elements_.clear(); }
    elements_.push_back(std::move(value));
}

void Json::serialize_into(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(number_)) {
                out += "null";
            } else if (std::fabs(number_) < 1e15 &&
                       number_ == static_cast<double>(static_cast<long long>(number_))) {
                out += std::to_string(static_cast<long long>(number_));
            } else {
                // The shortest form that reads back as the same double, as a
                // browser's JSON.stringify writes it, and in any locale. Ten
                // significant digits, as it was, turned 1/3 into a different
                // number and a second round into yet another.
                char buffer[32];
                const auto written = std::to_chars(buffer, buffer + sizeof(buffer), number_);
                out.append(buffer, written.ptr);
            }
            break;
        }
        case Type::String: {
            out += '"';
            for (unsigned char c : string_) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char buffer[8];
                            snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                            out += buffer;
                        } else {
                            out += static_cast<char>(c);
                        }
                }
            }
            out += '"';
            break;
        }
        case Type::Array: {
            out += '[';
            for (size_t i = 0; i < elements_.size(); i++) {
                if (i) out += ',';
                elements_[i].serialize_into(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            for (size_t i = 0; i < members_.size(); i++) {
                if (i) out += ',';
                Json(members_[i].first).serialize_into(out);
                out += ':';
                members_[i].second.serialize_into(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Json::serialize() const {
    std::string out;
    serialize_into(out);
    return out;
}

bool Json::parse(const std::string& text, Json& out) {
    std::string unused;
    return parse(text, out, unused);
}

bool Json::parse(const std::string& text, Json& out, std::string& reason) {
    reason.clear();
    if (!valid_utf8(text)) {
        reason = "it is not UTF-8";
        return false;
    }
    JsonParser parser{text, 0, {}};
    if (!parser.parse_value(out, 0)) {
        reason = parser.reason.empty() ? "it is not valid JSON" : parser.reason;
        return false;
    }
    parser.skip_whitespace();
    if (parser.pos != text.size()) {  // reject trailing junk
        reason = "it is not valid JSON";
        return false;
    }
    return true;
}

}  // namespace fernsdr

#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace fernsdr {
inline size_t utf8_character_bytes(std::string_view text) {
    if (text.empty()) return 0;
    const auto a = static_cast<unsigned char>(text[0]);
    if (a < 0x80) return 1;
    const size_t size = a >= 0xC2 && a <= 0xDF ? 2 : a >= 0xE0 && a <= 0xEF ? 3 :
                        a >= 0xF0 && a <= 0xF4 ? 4 : 0;
    if (!size || text.size() < size) return 0;
    for (size_t i = 1; i < size; i++) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80 || c > 0xBF) return 0;
    }
    const auto b = static_cast<unsigned char>(text[1]);
    if ((a == 0xE0 && b < 0xA0) || (a == 0xED && b >= 0xA0) ||
        (a == 0xF0 && b < 0x90) || (a == 0xF4 && b > 0x8F)) return 0;
    return size;
}

inline bool valid_utf8(std::string_view text) {
    while (!text.empty()) {
        const size_t size = utf8_character_bytes(text);
        if (!size) return false;
        text.remove_prefix(size);
    }
    return true;
}

// Text that came from outside - a module, a package, GitHub - made safe to put
// in a log line or the admin panel's JSON: at most `limit` bytes, cut at a
// character boundary, broken UTF-8 replaced, control characters that could
// rewrite a terminal or forge a log line replaced, and "..." when shortened.
inline std::string printable(std::string_view text, size_t limit) {
    std::string out;
    bool shortened = false;
    while (!text.empty()) {
        const size_t size = utf8_character_bytes(text);
        const size_t take = size == 0 ? 1 : size;
        if (out.size() + take > limit) {
            shortened = true;
            break;
        }
        const auto first = static_cast<unsigned char>(text[0]);
        if (size == 0) out += '?';
        else if (size == 1 && (first < 0x20 || first == 0x7f)) out += first == '\t' ? ' ' : '?';
        else out.append(text.data(), size);
        text.remove_prefix(take);
    }
    if (shortened) out += "...";
    return out;
}
}

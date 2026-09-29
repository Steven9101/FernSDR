#include "../src/util/utf8.h"
#include "../src/core/chat.h"
#include "test_util.h"

TEST_CASE(utf8_rejects_overlong_surrogate_truncated_and_out_of_range_sequences) {
    for (const std::string text : {"\xC0\xAF", "\xED\xA0\x80", "\xE2\x82", "\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80"})
        CHECK(!fernsdr::valid_utf8(text));
    for (const std::string text : {"", "ASCII", "é", "日本語", "📻"}) CHECK(fernsdr::valid_utf8(text));
}

TEST_CASE(chat_byte_limits_never_cut_a_multibyte_character) {
    for (const size_t limit : {24u, 400u}) for (const std::string end : {"é", "漢", "📻"}) {
        const std::string prefix(limit - 1, 'a');
        CHECK_EQ_STR(fernsdr::clean_chat_text(prefix + end, limit), prefix);
        const std::string exact = std::string(limit - end.size(), 'a') + end;
        CHECK_EQ_STR(fernsdr::clean_chat_text(exact, limit), exact);
        CHECK(fernsdr::valid_utf8(fernsdr::clean_chat_text(prefix + end, limit)));
    }
    CHECK(fernsdr::clean_chat_text("broken\xC3", 400).empty());
}

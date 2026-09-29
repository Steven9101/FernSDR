#include "../src/core/chat.h"
#include "test_util.h"

#include <string>

TEST_CASE(chat_counts_its_rate_per_address_not_per_session) {
    // A session ends with every reload, and one address may hold sixteen at
    // once. Counted per session, each of them had a full allowance.
    fernsdr::ChatMessage out;
    std::string error;
    int single = 0;
    {
        fernsdr::ChatRoom room;
        for (int i = 0; i < 20; i++) single += room.post(1, "198.51.100.1", "G0ABC", "hello", 1000 + i, out, error);
    }
    fernsdr::ChatRoom room;
    int spread = 0;
    for (int i = 0; i < 20; i++) spread += room.post(100 + i, "203.0.113.7", "G0ABC", "hello", 1000 + i, out, error);
    CHECK(single > 0);
    CHECK_EQ(spread, single);
    // Another address has its own allowance, and the first gets its back once
    // the window has passed.
    CHECK(room.post(500, "198.51.100.9", "G0XYZ", "hi", 1100, out, error));
    CHECK(room.post(600, "203.0.113.7", "G0ABC", "later", 1000 + 60000, out, error));
}

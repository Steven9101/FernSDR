#include <cstdint>
#include <vector>

#include "../src/core/row_ring.h"
#include "test_util.h"

using namespace fernsdr;

TEST_CASE(row_ring_gives_rows_back_oldest_first_and_drops_the_oldest_when_full) {
    RowRing ring(3, 4);
    std::vector<uint8_t> out;
    CHECK_EQ(ring.copy_oldest_first(out), 0);
    CHECK(out.empty());

    // Six rows into room for four: rows 2..5 remain, in order.
    for (uint8_t n = 0; n < 6; n++) {
        const uint8_t row[3] = {n, static_cast<uint8_t>(n + 100), static_cast<uint8_t>(n + 200)};
        ring.push(row);
    }
    CHECK_EQ(ring.size(), 4);
    CHECK_EQ(ring.copy_oldest_first(out), 4);
    CHECK_EQ(out.size(), 12);
    for (size_t row = 0; row < 4; row++) {
        CHECK_EQ(out[row * 3], row + 2);
        CHECK_EQ(out[row * 3 + 1], row + 102);
        CHECK_EQ(out[row * 3 + 2], row + 202);
    }
}

TEST_CASE(row_ring_before_it_is_full_holds_only_what_was_pushed) {
    RowRing ring(2, 5);
    const uint8_t first[2] = {7, 8};
    const uint8_t second[2] = {9, 10};
    ring.push(first);
    ring.push(second);
    std::vector<uint8_t> out;
    CHECK_EQ(ring.copy_oldest_first(out), 2);
    CHECK(out == (std::vector<uint8_t>{7, 8, 9, 10}));
}

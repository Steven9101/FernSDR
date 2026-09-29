#include <sys/resource.h>

#include "../src/util/limits.h"
#include "test_util.h"

TEST_CASE(open_file_limit_is_raised_to_the_hard_limit) {
    rlimit before{};
    CHECK(getrlimit(RLIMIT_NOFILE, &before) == 0);
    // Start from the soft limit a service manager usually leaves.
    if (before.rlim_max != RLIM_INFINITY && before.rlim_max > 1024) {
        rlimit low = before;
        low.rlim_cur = 1024;
        CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0);
    }
    const auto result = fernsdr::raise_open_file_limit();
    rlimit after{};
    CHECK(getrlimit(RLIMIT_NOFILE, &after) == 0);
    CHECK_EQ(static_cast<std::uint64_t>(after.rlim_cur), result);
    const rlim_t cap = rlim_t(1) << 20;
    const rlim_t expected = before.rlim_max == RLIM_INFINITY ? cap : std::min(before.rlim_max, cap);
    CHECK_EQ(static_cast<std::uint64_t>(after.rlim_cur), static_cast<std::uint64_t>(expected));
    // Calling it again changes nothing.
    CHECK_EQ(fernsdr::raise_open_file_limit(), result);
}

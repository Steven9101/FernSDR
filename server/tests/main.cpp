#include "../src/util/log.h"
#include "test_util.h"

int main(int argc, char** argv) {
    // The tests exercise code that logs on purpose - a band starting, a theme
    // file missing - and its output interleaves with the results, which makes
    // a failure harder to find than it needs to be.
    ::fernsdr::set_log_level(::fernsdr::LogLevel::None);
    return test::run_all(argc > 1 ? argv[1] : nullptr);
}

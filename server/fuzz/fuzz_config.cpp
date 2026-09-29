// The configuration parser: a file the operator writes by hand, and one the
// admin panel's editor writes from what it is sent.
#include <cstddef>
#include <cstdint>
#include <string>

#include "../src/util/config.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    fernsdr::Config config;
    std::string error;
    if (!config.parse(text, error)) return 0;
    for (const fernsdr::ConfigSection& section : config.sections()) {
        for (const auto& [key, value] : section.values()) {
            (void)section.get_double(key, 0.0);
            (void)section.get_int(key, 0);
            (void)section.get_bool(key, false);
            (void)value;
        }
    }
    (void)config.sections_with_prefix("band:");
    return 0;
}

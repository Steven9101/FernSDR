// The JSON parser, which every listener's messages and the admin panel's go
// through. What it accepts, the serializer writes in a form it accepts again,
// and that form is stable.
#include <cstddef>
#include <cstdint>
#include <string>

#include "../src/util/json.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    fernsdr::Json value;
    std::string reason;
    if (!fernsdr::Json::parse(text, value, reason)) return 0;
    const std::string once = value.serialize();
    fernsdr::Json again;
    if (!fernsdr::Json::parse(once, again)) __builtin_trap();
    if (again.serialize() != once) __builtin_trap();
    return 0;
}

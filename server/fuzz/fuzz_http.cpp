// The HTTP request parser: the first code an unauthenticated peer's bytes
// reach. Several requests back to back, as a kept-alive connection sends them.
#include <cstddef>
#include <cstdint>
#include <string>

#include "../src/net/http.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* text = reinterpret_cast<const char*>(data);
    size_t offset = 0;
    for (int i = 0; i < 8 && offset < size; i++) {
        fernsdr::HttpRequest request;
        size_t consumed = 0;
        if (fernsdr::parse_http_request(text + offset, size - offset, request, consumed, 4096) !=
            fernsdr::HttpParse::Ok) {
            break;
        }
        if (consumed == 0 || consumed > size - offset) __builtin_trap();
        (void)request.header("host");
        (void)request.header_contains("connection", "upgrade");
        (void)request.keep_alive();
        std::string decoded;
        (void)fernsdr::percent_decode(request.target, decoded);
        offset += consumed;
    }
    return 0;
}

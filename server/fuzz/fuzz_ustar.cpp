// A release archive, read before anything in it is unpacked. Whatever the
// reader accepts has to lie inside the input and survive being written out
// again and read back as the same entries.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../src/update/ustar.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string archive(reinterpret_cast<const char*>(data), size);
    std::vector<fernsdr::UstarEntry> entries;
    std::string error;
    if (!fernsdr::read_ustar(archive, entries, error)) return 0;

    std::vector<fernsdr::UstarInput> inputs;
    for (const fernsdr::UstarEntry& entry : entries) {
        if (entry.offset % 512 != 0 || entry.offset > archive.size() || entry.size > archive.size() - entry.offset) {
            __builtin_trap();
        }
        if (entry.directory && entry.size != 0) __builtin_trap();
        inputs.push_back({entry.path, entry.directory, entry.executable, archive.substr(entry.offset, entry.size)});
    }
    std::string again;
    if (!fernsdr::write_ustar(inputs, 0, again, error)) __builtin_trap();
    std::vector<fernsdr::UstarEntry> back;
    if (!fernsdr::read_ustar(again, back, error) || back.size() != entries.size()) __builtin_trap();
    for (size_t i = 0; i < back.size(); i++) {
        if (back[i].path != entries[i].path || back[i].directory != entries[i].directory ||
            back[i].executable != entries[i].executable ||
            again.compare(back[i].offset, back[i].size, inputs[i].contents) != 0) {
            __builtin_trap();
        }
    }
    return 0;
}

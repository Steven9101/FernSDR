// A downloaded module package (.fernmod) and the name it was published
// under: parsed before anything about it is trusted.
#include <cstddef>
#include <cstdint>
#include <string>

#include "../src/core/module_store.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string package(reinterpret_cast<const char*>(data), size);
    fernsdr::ModuleManifest manifest;
    size_t executable = 0;
    std::string error;
    if (fernsdr::parse_module_package(package, manifest, executable, error)) {
        if (executable > package.size()) __builtin_trap();
    }
    fernsdr::PackageName name;
    (void)fernsdr::parse_package_name(package.substr(0, 128), name);
    return 0;
}

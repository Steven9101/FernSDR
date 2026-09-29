// The release this source is. The server reports it with --version and to the
// admin panel; web/package.json and CHANGELOG.md carry the same number, and a
// web test holds the two files to it.
#pragma once

namespace fernsdr {

constexpr const char* kVersion = "0.1.0";

}  // namespace fernsdr

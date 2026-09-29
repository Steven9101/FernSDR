// The keys a release has to be signed with. Each is the public half of a key
// made with `fernsdr-release keygen`: one whose secret CI signs releases
// with, and one kept offline for when the first is lost or leaks. A release
// that changes this list reaches receivers signed by a key they already have.
//
// With the list empty, this build takes no update at all, since it cannot
// tell a release from anything else.
#pragma once
#include <vector>

#include "release.h"

namespace fernsdr {

inline const std::vector<ReleaseKey>& release_keys() {
    static const std::vector<ReleaseKey> keys = {};
    return keys;
}

}  // namespace fernsdr

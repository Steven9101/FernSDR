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
    static const std::vector<ReleaseKey> keys = {
        // The key CI signs releases with (the release environment's
        // FERNSDR_RELEASE_KEY), 59928fb2118b5d95360a86fad202c42a009b323ebcc41632dfd1ae29c02007dd.
        ReleaseKey{
            0x59, 0x92, 0x8f, 0xb2, 0x11, 0x8b, 0x5d, 0x95,
            0x36, 0x0a, 0x86, 0xfa, 0xd2, 0x02, 0xc4, 0x2a,
            0x00, 0x9b, 0x32, 0x3e, 0xbc, 0xc4, 0x16, 0x32,
            0xdf, 0xd1, 0xae, 0x29, 0xc0, 0x20, 0x07, 0xdd,
        },
        // The offline key, for when the first is lost or leaks,
        // c0c7529f3d64898a19864a5f87a82155589d6ff298ab757d0076b5ab50f5e9ab.
        ReleaseKey{
            0xc0, 0xc7, 0x52, 0x9f, 0x3d, 0x64, 0x89, 0x8a,
            0x19, 0x86, 0x4a, 0x5f, 0x87, 0xa8, 0x21, 0x55,
            0x58, 0x9d, 0x6f, 0xf2, 0x98, 0xab, 0x75, 0x7d,
            0x00, 0x76, 0xb5, 0xab, 0x50, 0xf5, 0xe9, 0xab,
        },
    };
    return keys;
}

}  // namespace fernsdr

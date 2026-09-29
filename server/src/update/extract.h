// Unpacking a release archive that read_ustar accepted.
#pragma once
#include <string>
#include <vector>

#include "ustar.h"

namespace fernsdr {

// Writes `entries` of `archive` below `dir`, a directory this process has
// just made and no one else can write to. Directories get 0755, files 0644,
// or 0755 when the archive marks them executable; the archive's own modes,
// owners and times are not used. Every entry is created new, through the
// descriptor of its parent, without following a link, and the parents an
// archive does not list are made as well. Files and directories are synced,
// so the tree is on disk before anything points at it.
bool extract_ustar(const std::string& archive, const std::vector<UstarEntry>& entries, int dir, std::string& error);

}  // namespace fernsdr

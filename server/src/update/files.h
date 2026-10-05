// Files as the updater has to handle them: it runs as root, and part of what
// it reads and writes lies in a directory the receiver's user owns. Whatever
// that user put there, a link where a file was, a second name for somebody
// else's file, has to be refused or replaced, never followed. So every call
// here works relative to a directory descriptor and does not follow a link
// in the name it is given.
#pragma once
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace fernsdr {

// A directory, opened without following a link at the end of `path`; -1 with
// errno on failure. The caller closes it.
int open_directory(const std::string& path);
int open_directory_at(int dir, const std::string& name);

// Whether the open directory `dir` belongs to this process's user and no
// other user can change what is in it. A directory reached through one that
// someone else owns could have been put there by them; this says whether
// the one actually opened is still this user's alone.
bool held_by_this_user(int dir);

// Reads `name` in `dir`: a regular file with one name, of at most `limit`
// bytes, owned by `owner` unless that is -1. `missing` is set, and false
// returned, when there is no such entry at all.
bool read_file_at(int dir, const std::string& name, size_t limit, uid_t owner, std::string& contents, bool& missing,
                  std::string& error);

// Writes `name` in `dir` as a new file renamed over whatever had the name,
// with its mode and owner set before the rename: a reader sees the old
// contents or the new, and a link in its place is replaced, not followed.
// The file and the directory are synced. -1 for `uid` and `gid` leaves them
// this process's.
bool write_file_at(int dir, const std::string& name, const std::string& contents, mode_t mode, uid_t uid, gid_t gid,
                   std::string& error);

// Points the link `name` in `dir` at `target`, by a new link renamed over the
// old, so at every moment the name points at one or the other.
bool replace_link_at(int dir, const std::string& name, const std::string& target, std::string& error);

// Where the link `name` in `dir` points; false when it is not a link.
bool read_link_at(int dir, const std::string& name, std::string& target);

// Removes `name` in `dir` and whatever is below it, following no link: a
// link is removed as a link.
bool remove_tree_at(int dir, const std::string& name, std::string& error);

// Removes `name` in `dir` if it is there; false only on another error.
bool remove_file_at(int dir, const std::string& name, std::string& error);

bool sync_directory(int dir);

// Bytes free to this process below `dir`.
bool free_bytes(int dir, uint64_t& bytes);

}  // namespace fernsdr

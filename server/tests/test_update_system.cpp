// The new version's configuration check, run the way the updater runs it:
// as the receiver's user, root gone. Only a test running as root can change
// user, so elsewhere these return at once.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "../src/update/files.h"
#include "../src/update/system.h"
#include "test_util.h"

namespace {

std::string make_script(const std::string& directory, const std::string& name, const std::string& text) {
    const std::string path = directory + "/" + name;
    FILE* file = std::fopen(path.c_str(), "w");
    if (file) {
        std::fputs(text.c_str(), file);
        std::fclose(file);
    }
    ::chmod(path.c_str(), 0755);
    return path;
}

}  // namespace

TEST_CASE(update_check_runs_as_the_receivers_user_and_nobody_else) {
    if (::geteuid() != 0) return;
    char name[] = "/tmp/fernsdr-check-XXXXXX";
    CHECK(::mkdtemp(name) != nullptr);
    const std::string directory = name;
    ::chmod(name, 0755);  // for the user the check becomes to reach the script

    // Who runs it, with which groups, and what it was asked.
    const std::string who =
        make_script(directory, "who", "#!/bin/sh\necho \"$(id -u) $(id -g) $(id -G) $1 $2 $3 $4\"\n");
    std::string output;
    CHECK(fernsdr::check_configuration_as(who, "/state/fernsdr.conf", "/opt/web", 65534, 65534, output));
    CHECK_EQ_STR(output, "65534 65534 65534 --check /state/fernsdr.conf --root /opt/web");

    // A refusal comes back with what the program said.
    const std::string refuses =
        make_script(directory, "refuses", "#!/bin/sh\necho 'line 3: unknown setting' >&2\nexit 1\n");
    CHECK(!fernsdr::check_configuration_as(refuses, "/c", "/w", 65534, 65534, output));
    CHECK_EQ_STR(output, "line 3: unknown setting");

    // And a program that cannot run at all is a refusal too.
    CHECK(!fernsdr::check_configuration_as(directory + "/missing", "/c", "/w", 65534, 65534, output));
    CHECK(!output.empty());

    const int parent = fernsdr::open_directory("/tmp");
    std::string error;
    fernsdr::remove_tree_at(parent, directory.substr(5), error);
    ::close(parent);
}

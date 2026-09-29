#include "../src/util/config.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

using fernsdr::Config;

TEST_CASE(config_parses_sections_and_values) {
    Config config;
    std::string error;
    CHECK(config.parse(R"(
# a comment
name = Northern Utah WebSDR   ; trailing comment

[server]
port = 8073
bind = 0.0.0.0

[band:40m]
center = 7.1M
sample_rate = 192k
)", error));

    CHECK(config.section("").get("name") == "Northern Utah WebSDR");
    CHECK_EQ(config.section("server").get_int("port", 0), 8073);
    CHECK(config.section("server").get("bind") == "0.0.0.0");
    CHECK_NEAR(config.section("band:40m").get_double("center", 0), 7.1e6, 1e-6);
    CHECK_NEAR(config.section("band:40m").get_double("sample_rate", 0), 192000.0, 1e-6);
}

TEST_CASE(config_reports_the_offending_line) {
    Config config;
    std::string error;
    CHECK(!config.parse("[ok]\nvalid = 1\nthis line has no equals sign\n", error));
    CHECK(error.find("line 3") != std::string::npos);

    CHECK(!config.parse("[unterminated\n", error));
    CHECK(error.find("line 1") != std::string::npos);
}

TEST_CASE(config_missing_values_fall_back) {
    Config config;
    std::string error;
    CHECK(config.parse("[a]\nx = 1\n", error));
    CHECK(config.section("nope").get("anything", "default") == "default");
    CHECK_EQ(config.section("a").get_int("missing", 42), 42);
    CHECK(config.section("a").get_bool("missing", true));
}

TEST_CASE(config_collects_repeated_prefixed_sections_in_order) {
    Config config;
    std::string error;
    CHECK(config.parse("[band:80m]\nc=3.7M\n[other]\nx=1\n[band:40m]\nc=7.1M\n", error));
    auto bands = config.sections_with_prefix("band");
    CHECK_EQ(static_cast<long long>(bands.size()), 2);
    CHECK(bands[0]->name() == "band:80m");
    CHECK(bands[1]->name() == "band:40m");
}

TEST_CASE(config_parses_frequency_suffixes) {
    double hz = 0;
    CHECK(fernsdr::parse_frequency("7.1M", hz));      CHECK_NEAR(hz, 7.1e6, 1e-9);
    CHECK(fernsdr::parse_frequency("14074k", hz));    CHECK_NEAR(hz, 14074000.0, 1e-9);
    CHECK(fernsdr::parse_frequency("3690000", hz));   CHECK_NEAR(hz, 3690000.0, 1e-9);
    CHECK(fernsdr::parse_frequency(" 21.2 MHz ", hz)); CHECK_NEAR(hz, 21.2e6, 1e-9);
    CHECK(fernsdr::parse_frequency("500Hz", hz));     CHECK_NEAR(hz, 500.0, 1e-9);
    CHECK(!fernsdr::parse_frequency("banana", hz));
    CHECK(!fernsdr::parse_frequency("", hz));
    CHECK(!fernsdr::parse_frequency("7.1X", hz));
}

TEST_CASE(config_booleans_accept_the_usual_spellings) {
    Config config;
    std::string error;
    CHECK(config.parse("[a]\np=yes\nq=off\nr=TRUE\ns=0\n", error));
    const auto& a = config.section("a");
    CHECK(a.get_bool("p", false));
    CHECK(!a.get_bool("q", true));
    CHECK(a.get_bool("r", false));
    CHECK(!a.get_bool("s", true));
}

TEST_CASE(config_keeps_hashes_inside_quoted_values) {
    Config config;
    std::string error;
    CHECK(config.parse("[a]\nlabel = \"net #1\"\n", error));
    CHECK(config.section("a").get("label") == "net #1");
}

namespace {

std::string scratch_directory() {
    char pattern[] = "/tmp/fernsdr-config-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    return made ? made : "";
}

unsigned file_mode(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 ? info.st_mode & 0777 : 0;
}

}  // namespace

TEST_CASE(config_files_holding_secrets_are_written_for_their_owner_only) {
    const std::string directory = scratch_directory();
    CHECK(!directory.empty());
    const std::string path = directory + "/fernsdr.conf";
    std::string error;

    // A permissive umask, and a temporary left behind by an interrupted save
    // with a mode that would otherwise survive into the saved file.
    const mode_t previous = ::umask(0);
    CHECK(fernsdr::write_text_file(path + ".tmp", "stale", error));
    CHECK(::chmod((path + ".tmp").c_str(), 0666) == 0);
    CHECK(fernsdr::write_text_file(path, "[admin]\npassword_hash = x\n", error, fernsdr::FileAccess::OwnerOnly));
    CHECK_EQ(file_mode(path), 0600u);
    std::string text;
    CHECK(fernsdr::read_text_file(path, text));
    CHECK(text == "[admin]\npassword_hash = x\n");

    // Anything else is left to the umask, as before.
    CHECK(fernsdr::write_text_file(directory + "/theme.json", "{}", error));
    CHECK_EQ(file_mode(directory + "/theme.json"), 0644u);
    ::umask(previous);

    // A temporary that is a link is refused rather than followed.
    CHECK(::symlink((directory + "/elsewhere").c_str(), (directory + "/linked.tmp").c_str()) == 0);
    CHECK(!fernsdr::write_text_file(directory + "/linked", "secret", error, fernsdr::FileAccess::OwnerOnly));
    CHECK(::access((directory + "/elsewhere").c_str(), F_OK) != 0);

    for (const char* name : {"fernsdr.conf", "theme.json", "linked.tmp"}) ::unlink((directory + "/" + name).c_str());
    ::rmdir(directory.c_str());
}

TEST_CASE(config_whole_numbers_no_long_can_hold_fall_back) {
    // Found by fuzzing: 4e126 was cast to long as it was, which C++ leaves
    // undefined.
    fernsdr::Config config;
    std::string error;
    CHECK(config.parse("[a]\nhuge = 4e126\nsmall = -4e126\nfine = 2147483647\nlow = -2147483648\n"
                       "past32 = 2147483648\nbig = 9e18\n",
                       error));
    const fernsdr::ConfigSection& a = config.section("a");
    CHECK_EQ(a.get_int("huge", 7), 7);
    CHECK_EQ(a.get_int("small", 7), 7);
    CHECK_EQ(a.get_int("fine", 7), 2147483647);
    CHECK_EQ(a.get_int("low", 7), -2147483648LL);
    // Beyond 32 bits: a long of 64 holds it, the 32-bit one of armhf does
    // not, and falls back rather than saturate as ARM's conversion does.
    const bool wide = sizeof(long) == 8;
    CHECK_EQ(a.get_int("past32", 7), wide ? 2147483648LL : 7LL);
    CHECK_EQ(a.get_int("big", 7), wide ? 9000000000000000000LL : 7LL);
}

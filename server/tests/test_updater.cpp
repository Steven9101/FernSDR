// The update from request to commit or rollback, in a directory that stands
// in for /opt/fernsdr, /var/lib/fernsdr and /var/lib/fernsdr-update, with
// the network, the service manager and the clock played by the test.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../src/update/files.h"
#include "../src/update/release.h"
#include "../src/update/updater.h"
#include "../src/update/ustar.h"
#include "../src/util/ed25519.h"
#include "../src/util/json.h"
#include "../src/util/password.h"
#include "test_util.h"

using fernsdr::UpdateOutcome;

namespace {

using Result = UpdateOutcome::Result;

std::string sha256_hex(const std::string& data) {
    fernsdr::Sha256 hash;
    hash.update(data);
    uint8_t digest[32];
    hash.finish(digest);
    return fernsdr::to_hex(digest, 32);
}

void put(const std::string& path, const std::string& text) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    if (::write(fd, text.data(), text.size()) < 0) {}
    ::close(fd);
}

std::string contents_of(const std::string& path) {
    std::string out;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return "<missing>";
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) out.append(buffer, static_cast<size_t>(n));
    ::close(fd);
    return out;
}

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

std::string link_of(const std::string& path) {
    char buffer[256];
    const ssize_t n = ::readlink(path.c_str(), buffer, sizeof(buffer));
    return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : "";
}

// An install with 0.1.0 current and trusted, a configuration and settings,
// and a release server that knows whatever the test publishes.
struct Install {
    std::string root;
    fernsdr::UpdateLayout layout;
    fernsdr::UpdateEnvironment env;
    std::map<std::string, std::string> server;
    uint8_t seed[32];
    int64_t clock_ms = 1'000'000;
    int restarts_asked = 0;
    // What the new version does once started: say it works after a while,
    // crash over and over, or nothing.
    enum class Behaviour { Works, Crashes, Silent, WrongVersion } behaviour = Behaviour::Works;
    int service_restarts = 0;
    bool check_passes = true;
    std::string during_trial_config;  // written to fernsdr.conf while on trial

    Install() {
        char name[] = "/tmp/fernsdr-updater-XXXXXX";
        root = ::mkdtemp(name) ? name : "";
        layout = {root + "/opt", root + "/state", root + "/update"};
        for (const std::string& dir : {layout.install, layout.install + "/releases", layout.install + "/releases/0.1.0",
                                       layout.install + "/releases/0.1.0/web", layout.state, layout.update}) {
            ::mkdir(dir.c_str(), 0755);
        }
        put(layout.install + "/releases/0.1.0/fernsdr", "old program");
        put(layout.install + "/releases/0.1.0/web/index.html", "old page");
        CHECK(::symlink("releases/0.1.0", (layout.install + "/current").c_str()) == 0);
        CHECK(::symlink("releases/0.1.0", (layout.install + "/trusted").c_str()) == 0);
        put(layout.state + "/fernsdr.conf", "[site]\nname = before\n");
        put(layout.state + "/fernsdr-settings.json", "{\"before\":true}");

        for (int i = 0; i < 32; i++) seed[i] = static_cast<uint8_t>(40 + i);
        fernsdr::ReleaseKey key;
        fernsdr::ed25519_public_key(seed, key.data());
        env.keys = {key};
        env.platform = "linux-x86_64";
        env.running_version = "0.1.0";
        env.base_url = "https://releases.test/";
        env.trial_ms = 5 * 60 * 1000;
        env.state_may_be_roots = ::geteuid() == 0;
        env.fetch = [this](const std::string& url, size_t limit, std::string& body, std::string& error) {
            const auto found = server.find(url);
            if (found == server.end()) {
                error = "404 for " + url;
                return false;
            }
            if (found->second.size() > limit) {
                error = "larger than " + std::to_string(limit);
                return false;
            }
            body = found->second;
            return true;
        };
        env.check = [this](const std::string& program, const std::string&, const std::string&, uid_t, gid_t,
                           std::string& output) {
            output = check_passes ? "" : "unknown setting";
            return check_passes && exists(program);
        };
        env.restart = [this](std::string&) {
            restarts_asked++;
            // The receiver now running is whatever `current` names.
            const std::string running = link_of(layout.install + "/current");
            if (running == "releases/0.1.0") return true;
            if (!during_trial_config.empty()) put(layout.state + "/fernsdr.conf", during_trial_config);
            if (behaviour == Behaviour::Works) put(layout.state + "/update-commit", running.substr(9) + "\n");
            if (behaviour == Behaviour::WrongVersion) put(layout.state + "/update-commit", "9.9.9\n");
            return true;
        };
        env.service = [this](int& restarts, bool& failed) {
            if (behaviour == Behaviour::Crashes) service_restarts++;
            restarts = service_restarts;
            failed = false;
            return true;
        };
        env.now_ms = [this] { return clock_ms; };
        env.sleep_ms = [this](int64_t ms) { clock_ms += ms; };
    }

    ~Install() {
        const int top = fernsdr::open_directory("/tmp");
        std::string error;
        if (top >= 0) fernsdr::remove_tree_at(top, root.substr(5), error);
        if (top >= 0) ::close(top);
    }

    // Publishes `version` for this platform, signed with the install's key.
    void publish(const std::string& version, const std::string& channel = "stable",
                 const std::string& platform = "linux-x86_64") {
        std::string archive, error;
        CHECK(fernsdr::write_ustar({{"fernsdr", false, true, "program " + version},
                                    {"web", true, false, ""},
                                    {"web/index.html", false, false, "page " + version}},
                                   0, archive, error));
        fernsdr::ReleaseManifest manifest;
        manifest.version = version;
        manifest.date = "2026-10-01";
        manifest.channel = channel;
        manifest.notes = "Notes for " + version;
        const std::string file = "fernsdr-" + version + "-" + platform + ".tar";
        manifest.assets.push_back({platform, file, archive.size(), sha256_hex(archive)});
        std::string text;
        CHECK(fernsdr::format_release_manifest(manifest, text, error));
        const std::string message = fernsdr::release_signed_message(text);
        uint8_t signature[64];
        fernsdr::ed25519_sign(seed, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature);
        server[env.base_url + fernsdr::kReleaseManifestName] = text;
        server[env.base_url + fernsdr::kReleaseSignatureName] = std::string(reinterpret_cast<const char*>(signature), 64);
        server[env.base_url + file] = archive;
    }

    void request(const std::string& version) {
        std::string error;
        CHECK(fernsdr::request_update(layout.state, version, {"20m"}, error));
    }

    UpdateOutcome run() { return fernsdr::run_update(layout, env); }

    std::string current() const { return link_of(layout.install + "/current"); }
    std::string trusted() const { return link_of(layout.install + "/trusted"); }
    std::string status_state() const {
        fernsdr::Json status;
        fernsdr::Json::parse(contents_of(layout.update + "/status.json"), status);
        return status["state"].string();
    }
};

}  // namespace

TEST_CASE(updater_installs_a_signed_release_that_says_it_works) {
    Install install;
    install.publish("0.1.1");
    install.request("0.1.1");
    const UpdateOutcome outcome = install.run();
    CHECK(outcome.result == Result::Updated);
    CHECK_EQ_STR(outcome.message, "Updated to 0.1.1.");
    CHECK_EQ_STR(install.current(), "releases/0.1.1");
    CHECK_EQ_STR(install.trusted(), "releases/0.1.1");
    CHECK_EQ_STR(contents_of(install.layout.install + "/releases/0.1.1/fernsdr"), "program 0.1.1");
    // The version before stays, for going back by hand; the request, the
    // trial and the snapshot are gone.
    CHECK(exists(install.layout.install + "/releases/0.1.0/fernsdr"));
    CHECK(!exists(install.layout.state + "/update-request"));
    CHECK(!exists(install.layout.update + "/trial"));
    CHECK(!exists(install.layout.update + "/snapshot"));
    CHECK_EQ(install.restarts_asked, 1);
    CHECK_EQ_STR(install.status_state(), "updated");
    struct stat info {};
    CHECK(::stat((install.layout.install + "/releases/0.1.1/fernsdr").c_str(), &info) == 0 &&
          (info.st_mode & 0777) == 0755);
}

TEST_CASE(updater_refuses_a_state_directory_of_roots) {
    // Only a test running as root can make one to try it with.
    if (::geteuid() != 0) return;
    Install install;
    install.env.state_may_be_roots = false;
    install.publish("0.1.1");
    install.request("0.1.1");
    const UpdateOutcome outcome = install.run();
    CHECK(outcome.result == Result::Failed);
    CHECK(outcome.message.find("belongs to root") != std::string::npos);
    CHECK_EQ_STR(install.current(), "releases/0.1.0");
}

TEST_CASE(updater_does_nothing_without_a_request) {
    Install install;
    install.publish("0.1.1");
    CHECK(install.run().result == Result::Nothing);
    CHECK_EQ_STR(install.current(), "releases/0.1.0");
    CHECK_EQ(install.restarts_asked, 0);
}

TEST_CASE(updater_refuses_requests_it_cannot_trust) {
    // Not newer, not a version, and a request that is a link to some file.
    for (const char* version : {"0.1.0", "0.0.9"}) {
        Install install;
        install.publish(version);
        install.request(version);
        const UpdateOutcome outcome = install.run();
        CHECK(outcome.result == Result::Refused);
        CHECK(outcome.message.find("not older") != std::string::npos);
    }
    Install words;
    put(words.layout.state + "/update-request", "latest\n");
    CHECK(words.run().result == Result::Refused);
    CHECK(!exists(words.layout.state + "/update-request"));
    Install linked;
    linked.publish("0.1.1");
    put(linked.root + "/elsewhere", "0.1.1\n");
    CHECK(::symlink((linked.root + "/elsewhere").c_str(), (linked.layout.state + "/update-request").c_str()) == 0);
    const UpdateOutcome outcome = linked.run();
    CHECK(outcome.result == Result::Refused);
    CHECK(outcome.message.find("link") != std::string::npos);
    CHECK_EQ_STR(linked.current(), "releases/0.1.0");
}

TEST_CASE(updater_refuses_releases_that_are_not_what_they_claim) {
    const auto refused = [](const std::function<void(Install&)>& spoil, const std::string& reason) {
        Install install;
        install.publish("0.1.1");
        spoil(install);
        install.request("0.1.1");
        const UpdateOutcome outcome = install.run();
        const bool ok = outcome.result == Result::Refused && outcome.message.find(reason) != std::string::npos &&
                        install.current() == "releases/0.1.0" &&
                        !exists(install.layout.install + "/releases/0.1.1") && install.restarts_asked == 0;
        if (!ok) fprintf(stderr, "    got \"%s\"\n", outcome.message.c_str());
        return ok;
    };
    // A manifest changed after signing, a signature by another key, an
    // archive that is not the one the manifest names.
    CHECK(refused([](Install& i) { i.server[i.env.base_url + "fernsdr-release-v1.txt"] += "note changed\n"; },
                  "not signed by a key"));
    CHECK(refused([](Install& i) { i.env.keys[0][0] ^= 1; }, "refused"));
    CHECK(refused([](Install& i) { i.server[i.env.base_url + "fernsdr-0.1.1-linux-x86_64.tar"][600] ^= 1; },
                  "not the archive"));
    // Signed, but another version, a testing release, none for this machine.
    CHECK(refused([](Install& i) { i.publish("0.1.2"); }, "is 0.1.2, not 0.1.1"));
    CHECK(refused([](Install& i) { i.publish("0.1.1", "testing"); }, "testing"));
    CHECK(refused([](Install& i) { i.env.platform = "linux-armhf"; }, "no archive for linux-armhf"));
    // A release that unpacks, but refuses the receiver's configuration.
    CHECK(refused([](Install& i) { i.check_passes = false; }, "refuses this receiver's configuration"));
}

TEST_CASE(updater_goes_back_when_the_new_version_never_says_it_works) {
    Install install;
    install.publish("0.1.1");
    install.behaviour = Install::Behaviour::Silent;
    // The new version rewrote the configuration and made a theme file.
    install.during_trial_config = "[site]\nname = rewritten by 0.1.1\n";
    const std::string original = contents_of(install.layout.state + "/fernsdr.conf");
    install.request("0.1.1");
    const UpdateOutcome outcome = install.run();
    CHECK(outcome.result == Result::RolledBack);
    CHECK(outcome.message.find("did not say it works within 5 minutes") != std::string::npos);
    CHECK_EQ_STR(install.current(), "releases/0.1.0");
    CHECK_EQ_STR(install.trusted(), "releases/0.1.0");
    CHECK_EQ_STR(contents_of(install.layout.state + "/fernsdr.conf"), original);
    CHECK_EQ_STR(contents_of(install.layout.state + "/fernsdr-settings.json"), "{\"before\":true}");
    CHECK(!exists(install.layout.install + "/releases/0.1.1"));
    CHECK(!exists(install.layout.update + "/trial"));
    CHECK_EQ(install.restarts_asked, 2);  // into the new version, and back
    CHECK_EQ_STR(install.status_state(), "rolled-back");
    // Five minutes and not long after.
    CHECK(install.clock_ms - 1'000'000 >= 5 * 60 * 1000);
    CHECK(install.clock_ms - 1'000'000 <= 5 * 60 * 1000 + 2000);
}

TEST_CASE(updater_goes_back_at_once_from_a_crash_loop_or_the_wrong_answer) {
    Install crashing;
    crashing.publish("0.1.1");
    crashing.behaviour = Install::Behaviour::Crashes;
    crashing.request("0.1.1");
    const UpdateOutcome outcome = crashing.run();
    CHECK(outcome.result == Result::RolledBack);
    CHECK(outcome.message.find("kept stopping") != std::string::npos);
    CHECK(crashing.clock_ms - 1'000'000 < 10 * 1000);
    CHECK_EQ_STR(crashing.current(), "releases/0.1.0");

    // A commit file naming another version is not this version's word.
    Install wrong;
    wrong.publish("0.1.1");
    wrong.behaviour = Install::Behaviour::WrongVersion;
    wrong.request("0.1.1");
    CHECK(wrong.run().result == Result::RolledBack);
    CHECK_EQ_STR(wrong.trusted(), "releases/0.1.0");
}

TEST_CASE(updater_finishes_a_rollback_whatever_the_receiver_put_in_the_way) {
    // During the trial the receiver's user puts directories where the
    // settings were and where the theme would be. Restoring over them must
    // not stop the rollback half way, which left updates wedged for good.
    Install install;
    install.publish("0.1.1");
    install.behaviour = Install::Behaviour::Silent;
    const std::string settings = contents_of(install.layout.state + "/fernsdr-settings.json");
    const auto plain_restart = install.env.restart;
    install.env.restart = [&](std::string& why) {
        if (link_of(install.layout.install + "/current") == "releases/0.1.1") {
            ::unlink((install.layout.state + "/fernsdr-settings.json").c_str());
            ::mkdir((install.layout.state + "/fernsdr-settings.json").c_str(), 0755);
            put(install.layout.state + "/fernsdr-settings.json/inside", "x");
            ::mkdir((install.layout.state + "/fernsdr-theme.json").c_str(), 0755);
        }
        return plain_restart(why);
    };
    install.request("0.1.1");
    const UpdateOutcome outcome = install.run();
    CHECK(outcome.result == Result::RolledBack);
    CHECK_EQ_STR(install.current(), "releases/0.1.0");
    CHECK(!exists(install.layout.update + "/trial"));
    CHECK_EQ_STR(contents_of(install.layout.state + "/fernsdr-settings.json"), settings);
    CHECK(!exists(install.layout.state + "/fernsdr-theme.json"));
    CHECK_EQ(install.restarts_asked, 2);
    // And the next update goes through.
    install.env.restart = plain_restart;
    install.behaviour = Install::Behaviour::Works;
    install.request("0.1.1");
    CHECK(install.run().result == Result::Updated);
    CHECK_EQ_STR(install.trusted(), "releases/0.1.1");
}

TEST_CASE(updater_keeps_a_version_that_said_it_works_before_the_updater_stopped) {
    // The new version wrote its commit, then the updater was stopped before
    // it read it (systemd's timeout, or the machine going down): the version
    // is kept, as supervision would have kept it.
    Install install;
    install.publish("0.1.1");
    bool crash = true;
    install.env.restart = [&](std::string&) {
        install.restarts_asked++;
        if (crash) {
            put(install.layout.state + "/update-commit", "0.1.1\n");
            throw 1;
        }
        return true;
    };
    install.request("0.1.1");
    try {
        install.run();
    } catch (int) {
    }
    crash = false;
    const UpdateOutcome outcome = fernsdr::run_boot_check(install.layout, install.env);
    CHECK(outcome.result == Result::Updated);
    CHECK_EQ_STR(install.current(), "releases/0.1.1");
    CHECK_EQ_STR(install.trusted(), "releases/0.1.1");
    CHECK(!exists(install.layout.update + "/trial"));
}

TEST_CASE(updater_takes_the_request_away_even_when_it_cannot_go_on) {
    // A trial record it cannot end, here for want of a trusted version: the
    // request is gone all the same, or its path unit would start the updater
    // again and again.
    Install install;
    install.publish("0.1.1");
    std::string error;
    const int update = fernsdr::open_directory(install.layout.update);
    CHECK(fernsdr::write_file_at(update, "trial", "old 0.1.0\nnew 0.1.1\nstarted 5\npresent\n", 0644,
                                 static_cast<uid_t>(-1), static_cast<gid_t>(-1), error));
    ::close(update);
    ::unlink((install.layout.install + "/trusted").c_str());
    install.request("0.1.1");
    CHECK(install.run().result == Result::Failed);
    CHECK(!exists(install.layout.state + "/update-request"));
}

TEST_CASE(updater_goes_by_the_trusted_link_not_its_own_idea_of_itself) {
    // A build whose version was not raised: going back to releases/0.0.9
    // would point the receiver at nothing, and a commit would prune the real
    // previous release.
    Install install;
    install.env.running_version = "0.0.9";
    install.publish("0.1.1");
    install.request("0.1.1");
    const UpdateOutcome outcome = install.run();
    CHECK(outcome.result == Result::Refused);
    CHECK(outcome.message.find("trusted version is 0.1.0") != std::string::npos);
    CHECK_EQ_STR(install.current(), "releases/0.1.0");
}

TEST_CASE(updater_will_not_copy_a_link_as_root) {
    // The receiver's user put a link where the configuration was: taken into
    // the snapshot, a rollback would have copied the file it points at back
    // under the receiver's name.
    Install install;
    install.publish("0.1.1");
    put(install.root + "/secret", "not the receiver's");
    ::unlink((install.layout.state + "/fernsdr-settings.json").c_str());
    CHECK(::symlink((install.root + "/secret").c_str(), (install.layout.state + "/fernsdr-settings.json").c_str()) == 0);
    install.request("0.1.1");
    const UpdateOutcome outcome = install.run();
    CHECK(outcome.result == Result::Failed);
    CHECK(outcome.message.find("will not copy fernsdr-settings.json") != std::string::npos);
    CHECK_EQ_STR(install.current(), "releases/0.1.0");
    CHECK(!exists(install.layout.install + "/releases/0.1.1"));
    CHECK_EQ(install.restarts_asked, 0);
}

TEST_CASE(updater_ends_an_interrupted_trial_before_anything_else) {
    // A trial the machine was restarted in: the switch and the record are
    // there, and the new version's changes to the configuration.
    Install install;
    install.publish("0.1.1");
    install.behaviour = Install::Behaviour::Silent;
    install.env.trial_ms = 0;  // not reached: the restart hook plays the crash
    const std::string original = contents_of(install.layout.state + "/fernsdr.conf");
    bool crash = true;
    install.env.restart = [&](std::string&) {
        install.restarts_asked++;
        if (crash) {
            put(install.layout.state + "/fernsdr.conf", "rewritten");
            put(install.layout.state + "/fernsdr-theme.json", "{}");
            throw 1;  // the updater stops here, mid-trial
        }
        return true;
    };
    install.request("0.1.1");
    bool thrown = false;
    try {
        install.run();
    } catch (int) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK_EQ_STR(install.current(), "releases/0.1.1");
    CHECK(exists(install.layout.update + "/trial"));

    // At boot the trusted version is put back, with the configuration as it
    // was; nothing is restarted, since the receiver has not started yet.
    crash = false;
    install.restarts_asked = 0;
    const UpdateOutcome outcome = fernsdr::run_boot_check(install.layout, install.env);
    CHECK(outcome.result == Result::RolledBack);
    CHECK(outcome.message.find("interrupted") != std::string::npos);
    CHECK_EQ_STR(install.current(), "releases/0.1.0");
    CHECK_EQ_STR(contents_of(install.layout.state + "/fernsdr.conf"), original);
    CHECK(!exists(install.layout.state + "/fernsdr-theme.json"));
    CHECK(!exists(install.layout.update + "/trial"));
    CHECK_EQ(install.restarts_asked, 0);
    // With nothing left to do, the boot check does nothing.
    CHECK(fernsdr::run_boot_check(install.layout, install.env).result == Result::Nothing);
}

TEST_CASE(updater_finishes_a_commit_that_was_interrupted) {
    // Trusted was already switched to the new version when the updater
    // stopped: the record says trial, but the commit is what is finished.
    Install install;
    install.publish("0.1.1");
    install.request("0.1.1");
    CHECK(install.run().result == Result::Updated);
    std::string error;
    const int update = fernsdr::open_directory(install.layout.update);
    CHECK(fernsdr::write_file_at(update, "trial", "old 0.1.0\nnew 0.1.1\nstarted 5\npresent fernsdr.conf\n", 0644,
                                 static_cast<uid_t>(-1), static_cast<gid_t>(-1), error));
    ::close(update);
    const UpdateOutcome outcome = fernsdr::run_boot_check(install.layout, install.env);
    CHECK(outcome.result == Result::Updated);
    CHECK_EQ_STR(install.current(), "releases/0.1.1");
    CHECK_EQ_STR(install.trusted(), "releases/0.1.1");
    CHECK(!exists(install.layout.update + "/trial"));
}

TEST_CASE(update_trial_commits_once_the_new_version_has_served_with_its_bands) {
    Install install;
    std::string error;
    CHECK(fernsdr::request_update(install.layout.state, "0.1.1", {"20m", "40m"}, error));
    CHECK_EQ_STR(contents_of(install.layout.state + "/update-bands"), "20m\n40m\n");
    CHECK_EQ_STR(contents_of(install.layout.state + "/update-request"), "0.1.1\n");
    const int update = fernsdr::open_directory(install.layout.update);
    CHECK(fernsdr::write_file_at(update, "trial", "old 0.1.0\nnew 0.1.1\nstarted 5\npresent\n", 0644,
                                 static_cast<uid_t>(-1), static_cast<gid_t>(-1), error));
    ::close(update);

    // Another version is not on trial.
    CHECK(!fernsdr::UpdateTrial(install.layout.state, install.layout.update, "0.1.2").on_trial());
    fernsdr::UpdateTrial trial(install.layout.state, install.layout.update, "0.1.1");
    CHECK(trial.on_trial());
    CHECK_EQ(trial.bands().size(), 2u);
    // Not before a minute of serving, not while a band is missing.
    CHECK(!trial.tick(0, {"20m", "40m"}));
    CHECK(!trial.tick(59'000, {"20m", "40m"}));
    CHECK(!trial.tick(61'000, {"20m"}));
    CHECK(!exists(install.layout.state + "/update-commit"));
    CHECK(trial.tick(62'000, {"40m", "20m", "80m"}));
    CHECK_EQ_STR(contents_of(install.layout.state + "/update-commit"), "0.1.1\n");
    CHECK(trial.tick(63'000, {}));
}

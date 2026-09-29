// The Updates page's service: when a receiver can update itself, what a look
// at the published release finds, and when it may ask the updater.
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>

#include "../src/update/files.h"
#include "../src/update/release.h"
#include "../src/update/service.h"
#include "../src/update/updater.h"
#include "../src/util/ed25519.h"
#include "test_util.h"

namespace {

struct Setup {
    std::string root;
    uint8_t seed[32];
    std::map<std::string, std::string> server;
    fernsdr::UpdateServiceOptions options;

    Setup() {
        char name[] = "/tmp/fernsdr-update-service-XXXXXX";
        root = ::mkdtemp(name) ? name : "";
        ::mkdir((root + "/state").c_str(), 0700);
        ::mkdir((root + "/update").c_str(), 0755);
        for (int i = 0; i < 32; i++) seed[i] = static_cast<uint8_t>(90 + i);
        fernsdr::ReleaseKey key;
        fernsdr::ed25519_public_key(seed, key.data());
        options.state = root + "/state";
        options.update = root + "/update";
        options.install = "/opt/fernsdr";
        options.executable = "/opt/fernsdr/releases/0.1.0/fernsdr";
        options.running_version = "0.1.0";
        options.platform = "linux-x86_64";
        options.keys = {key};
        options.base_url = "https://releases.test/";
        options.fetch = [this](const std::string& url, size_t limit, std::string& body, std::string& error) {
            const auto found = server.find(url);
            if (found == server.end() || found->second.size() > limit) {
                error = "404";
                return false;
            }
            body = found->second;
            return true;
        };
    }

    ~Setup() {
        const int top = fernsdr::open_directory("/tmp");
        std::string error;
        if (top >= 0) fernsdr::remove_tree_at(top, root.substr(5), error);
        if (top >= 0) ::close(top);
    }

    void publish(const std::string& version, const std::string& channel = "stable",
                 const std::string& platform = "linux-x86_64") {
        fernsdr::ReleaseManifest manifest;
        manifest.version = version;
        manifest.date = "2026-10-01";
        manifest.channel = channel;
        manifest.notes = "What changed in " + version + ".\n\nMore.";
        manifest.assets.push_back({platform, "fernsdr-" + version + "-" + platform + ".tar", 1000, std::string(64, 'a')});
        std::string text, error;
        CHECK(fernsdr::format_release_manifest(manifest, text, error));
        const std::string message = fernsdr::release_signed_message(text);
        uint8_t signature[64];
        fernsdr::ed25519_sign(seed, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature);
        server[options.base_url + fernsdr::kReleaseManifestName] = text;
        server[options.base_url + fernsdr::kReleaseSignatureName] =
            std::string(reinterpret_cast<const char*>(signature), 64);
    }

    void status(const std::string& json) {
        const int dir = fernsdr::open_directory(root + "/update");
        std::string error;
        CHECK(fernsdr::write_file_at(dir, "status.json", json, 0644, static_cast<uid_t>(-1), static_cast<gid_t>(-1),
                                     error));
        ::close(dir);
    }

    bool exists(const std::string& name) const { return ::access((root + "/state/" + name).c_str(), F_OK) == 0; }
};

fernsdr::Json looked(fernsdr::UpdateService& service) {
    std::string error;
    CHECK(service.check(error));
    service.wait();
    return service.snapshot();
}

}  // namespace

TEST_CASE(update_service_says_why_a_receiver_cannot_update_itself) {
    const auto reason = [](const std::function<void(fernsdr::UpdateServiceOptions&)>& change) {
        Setup setup;
        change(setup.options);
        fernsdr::UpdateService service(setup.options);
        return service.unavailable();
    };
    CHECK_EQ_STR(reason([](fernsdr::UpdateServiceOptions&) {}), "");
    CHECK(reason([](fernsdr::UpdateServiceOptions& o) { o.keys.clear(); }).find("no release key") != std::string::npos);
    CHECK(reason([](fernsdr::UpdateServiceOptions& o) { o.platform.clear(); }).find("No releases") != std::string::npos);
    CHECK(reason([](fernsdr::UpdateServiceOptions& o) { o.executable = "/usr/local/bin/fernsdr"; })
              .find("not installed from a release") != std::string::npos);
    // In the image, the way to update is a new image.
    ::setenv("FERNSDR_CONTAINER", "1", 1);
    CHECK(reason([](fernsdr::UpdateServiceOptions& o) { o.executable = "/opt/fernsdr/fernsdr"; })
              .find("pulling the new image") != std::string::npos);
    ::unsetenv("FERNSDR_CONTAINER");
    CHECK(reason([](fernsdr::UpdateServiceOptions& o) { o.update += "/missing"; }).find("not set up") !=
          std::string::npos);
    // Not available, a look is refused with the reason, and so is a start.
    Setup setup;
    setup.options.keys.clear();
    fernsdr::UpdateService service(setup.options);
    std::string error;
    CHECK(!service.check(error));
    CHECK(error.find("no release key") != std::string::npos);
    CHECK(!service.snapshot()["available"].boolean(true));
}

TEST_CASE(update_service_finds_a_newer_release_and_asks_for_it) {
    Setup setup;
    setup.publish("0.1.1");
    fernsdr::UpdateService service(setup.options);
    std::string error;
    // Nothing looked at yet: nothing to start.
    CHECK(!service.start("0.1.1", {"20m"}, error));
    const fernsdr::Json view = looked(service);
    CHECK_EQ_STR(view["check"]["state"].string(), "done");
    CHECK_EQ_STR(view["check"]["version"].string(), "0.1.1");
    CHECK(view["check"]["newer"].boolean());
    CHECK_EQ_STR(view["check"]["notes"].string(), "What changed in 0.1.1.\n\nMore.");
    CHECK(view["check"]["checked"].number() > 0);
    // Only the version found, and not while the updater is at work.
    CHECK(!service.start("0.1.2", {"20m"}, error));
    setup.status("{\"state\":\"downloading\",\"version\":\"0.1.1\",\"message\":\"x\",\"time\":5}");
    CHECK(!service.start("0.1.1", {"20m"}, error));
    CHECK(error.find("already under way") != std::string::npos);
    CHECK_EQ_STR(service.snapshot()["status"]["state"].string(), "downloading");
    setup.status("{\"state\":\"rolled-back\",\"version\":\"0.1.1\",\"message\":\"x\",\"time\":5}");
    CHECK(service.start("0.1.1", {"20m", "40m"}, error));
    CHECK(setup.exists("update-request"));
    CHECK(setup.exists("update-bands"));
}

TEST_CASE(update_service_reports_what_it_cannot_use) {
    const auto failure = [](const std::function<void(Setup&)>& spoil) {
        Setup setup;
        setup.publish("0.1.1");
        spoil(setup);
        fernsdr::UpdateService service(setup.options);
        const fernsdr::Json view = looked(service);
        return view["check"]["state"].string() == "failed" ? view["check"]["error"].string() : std::string();
    };
    CHECK(failure([](Setup& s) { s.server.clear(); }).find("could not be fetched") != std::string::npos);
    CHECK(failure([](Setup& s) { s.options.keys[0][3] ^= 1; }).find("refused") != std::string::npos);
    CHECK(failure([](Setup& s) { s.publish("0.1.1", "testing"); }).find("testing") != std::string::npos);
    CHECK(failure([](Setup& s) { s.publish("0.1.1", "stable", "linux-armhf"); }).find("no archive") !=
          std::string::npos);
    // The same version or an older one is a look that found nothing to do.
    Setup same;
    same.publish("0.1.0");
    fernsdr::UpdateService service(same.options);
    const fernsdr::Json view = looked(service);
    CHECK_EQ_STR(view["check"]["state"].string(), "done");
    CHECK(!view["check"]["newer"].boolean(true));
    std::string error;
    CHECK(!service.start("0.1.0", {}, error));
}

// Modules end to end: the package format, the store, the process boundary
// and a band that runs, loses and recovers a real program. The program is
// tests/support/fake_module.cpp, built beside this test binary.
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../src/core/band.h"
#include "../src/core/module_store.h"
#include "../src/core/modules.h"
#include "../src/core/radio.h"
#include "../src/source/source_module.h"
#include "../src/util/config.h"
#include "../src/util/log.h"
#include "../src/util/password.h"
#include "../src/util/subprocess.h"
#include "test_util.h"

namespace {

using namespace fernsdr;
using std::chrono::milliseconds;

std::string fake_module_path() {
    char self[4096];
    const ssize_t length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) return "";
    self[length] = '\0';
    std::string path(self);
    return path.substr(0, path.find_last_of('/')) + "/fake-module";
}

std::string read_binary(const std::string& path) {
    std::string out;
    read_text_file(path, out);
    return out;
}

std::string sha256_hex(const std::string& data) {
    Sha256 hash;
    hash.update(data);
    uint8_t digest[32];
    hash.finish(digest);
    return to_hex(digest, 32);
}

const char* kSettings =
    R"([{"key":"behaviour","type":"choice","label":"Behaviour","choices":["normal","no-ready","invalid","no-device",)"
    R"("early","huge","ignore-stop","bad-rate","bad-center","near-center","clipping","fds","flood","stall",)"
    R"("crash","exit0","applied-flood",)"
    R"("helper","helper-exit"],"default":"normal"},)"
    R"({"key":"gain","type":"number","label":"Gain","min":0,"max":50,"unit":"dB","live":true},)"
    R"({"key":"label","type":"string","label":"Label"},)"
    R"({"key":"flag","type":"boolean","label":"Flag"}])";

std::string manifest_for(const std::string& program, const std::string& version, const std::string& platform = "",
                         const std::string& id = "fake", const std::string& settings = kSettings) {
    return std::string(R"({"schema":1,"id":")") + id + R"(","name":"Fake","version":")" + version +
           R"(","kind":"input","api":1,"platform":")" + (platform.empty() ? module_platform() : platform) +
           R"(","size":)" + std::to_string(program.size()) + R"(,"sha256":")" + sha256_hex(program) +
           R"(","license":"AGPL-3.0","source":"tests","description":"test double","settings":)" + settings + "}";
}

std::string package_of(const std::string& manifest, const std::string& program) {
    return "FERNMOD1\n" + std::to_string(manifest.size()) + "\n" + manifest + program;
}

struct TempDir {
    std::string path;
    TempDir() {
        char pattern[] = "/tmp/fernsdr-modules-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        path = made ? made : "";
    }
    ~TempDir() {
        if (path.empty()) return;
        const int ignored = std::system(("rm -rf '" + path + "'").c_str());
        (void)ignored;
    }
};

// A store holding the fake module, installed from a package the way a
// download would be.
std::shared_ptr<ModuleStore> store_with_fake(const TempDir& directory, const std::string& version = "1.0.0") {
    auto store = std::make_shared<ModuleStore>(directory.path + "/modules");
    const std::string program = read_binary(fake_module_path());
    ModuleManifest installed;
    std::string error;
    if (!store->install(package_of(manifest_for(program, version), program), "file", nullptr, installed, error)) {
        std::fprintf(stderr, "installing the fake module failed: %s\n", error.c_str());
        return nullptr;
    }
    return store;
}

ModuleTiming quick_timing() {
    ModuleTiming timing;
    timing.hello = milliseconds(2000);
    timing.ready = milliseconds(1000);
    timing.stall = milliseconds(400);
    timing.grace = milliseconds(300);
    timing.kill_wait = milliseconds(1000);
    return timing;
}

ConfigSection band_section(const std::string& behaviour, const std::string& extra_key = "",
                           const std::string& extra_value = "") {
    ConfigSection section("band:mod");
    section.set("source", "module");
    section.set("module", "fake");
    section.set("sample_rate", "48000");
    section.set("center", "7100000");
    section.set("module.behaviour", behaviour);
    section.set("module.label", "first");
    if (!extra_key.empty()) section.set(extra_key, extra_value);
    return section;
}

std::unique_ptr<Band> module_band(const std::shared_ptr<ModuleStore>& store, const ConfigSection& section) {
    std::string error;
    auto source = make_module_source(section, store, error, quick_timing());
    if (!source) {
        std::fprintf(stderr, "make_module_source: %s\n", error.c_str());
        return nullptr;
    }
    return std::make_unique<Band>("mod", "Module band", std::move(source), section);
}

bool wait_until(const std::function<bool()>& condition, milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(milliseconds(10));
    }
    return condition();
}

bool log_contains(const Band& band, const std::string& text) {
    const Json details = band.source_details();
    for (const Json& line : details["log"].elements()) {
        if (line.string().find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE(module_package_round_trips_and_rejects_damage) {
    const std::string program = "#!/bin/sh\nexit 0\n";
    const std::string manifest = manifest_for(program, "1.2.3");
    ModuleManifest parsed;
    size_t start = 0;
    std::string error;
    CHECK(parse_module_package(package_of(manifest, program), parsed, start, error));
    CHECK_EQ_STR(parsed.id, "fake");
    CHECK_EQ_STR(parsed.version, "1.2.3");
    CHECK_EQ(static_cast<long long>(parsed.size), static_cast<long long>(program.size()));
    CHECK_EQ(parsed.settings.size(), 4);
    CHECK(parsed.setting("gain") && parsed.setting("gain")->live);

    std::string damaged = package_of(manifest, program);
    damaged.back() ^= 1;
    CHECK(!parse_module_package(damaged, parsed, start, error));
    CHECK(error.find("SHA-256") != std::string::npos);
    CHECK(!parse_module_package(package_of(manifest, program).substr(0, 40), parsed, start, error));
    CHECK(!parse_module_package(package_of(manifest, program + "x"), parsed, start, error));
    CHECK(!parse_module_package("FERNMOD2\n2\n{}", parsed, start, error));
    CHECK(!parse_module_package("FERNMOD1\n99999999\n{}", parsed, start, error));

    // Fields that become paths, and fields the receiver acts on.
    const char* bad[] = {
        R"({"schema":1,"id":"../evil","name":"x","version":"1.0.0","kind":"input","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0","kind":"input","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[]})",
        R"({"schema":1,"id":"ok","name":"x","version":"01.0.0","kind":"input","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0.0","kind":"decoder","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0.0","kind":"input","api":2,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0.0","kind":"input","api":1,"platform":"windows","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0.0","kind":"input","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[{"key":"center","type":"number"}]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0.0","kind":"input","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[{"key":"a","type":"number","min":5,"max":1}]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0.0","kind":"input","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","settings":[{"key":"a","type":"choice"}]})",
        R"({"schema":1,"id":"ok","name":"x","version":"1.0.0","kind":"input","api":1,"platform":"linux-x86_64","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"})",
    };
    for (const char* text : bad) CHECK(!parse_module_manifest(text, parsed, error));
    CHECK(!parse_module_manifest(std::string(20000, ' '), parsed, error));

    // What the operator must install first: a few lines, shown as they are.
    const std::string base = manifest.substr(0, manifest.size() - 1);
    CHECK(parse_module_manifest(base + R"(,"requires":["SDRplay API 3.14 or 3.15 from sdrplay.com, with its service running"]})",
                                parsed, error));
    CHECK_EQ(parsed.requires_.size(), 1u);
    CHECK(parsed.to_json()["requires"][0].string().find("sdrplay.com") != std::string::npos);
    CHECK(parse_module_manifest(manifest, parsed, error));
    CHECK(parsed.requires_.empty());
    CHECK(!parsed.to_json().has("requires"));
    for (const std::string& needs : {std::string(R"("x")"), std::string(R"(["a","b","c","d","e"])"), std::string(R"([""])"),
                                     std::string(R"([1])"), "[\"" + std::string(201, 'x') + "\"]", std::string(R"(["line\nbreak"])"),
                                     std::string("[\"\xff\"]")}) {
        CHECK(!parse_module_manifest(base + R"(,"requires":)" + needs + "}", parsed, error));
    }

    // What the radio can be set to, for the panel's suggestions.
    CHECK(parse_module_manifest(base + R"(,"tuning":{"ranges":[[500000,1766000000]],"rates":[2400000,2048000],"signal":"iq"}})",
                                parsed, error));
    CHECK_EQ(parsed.tuning.ranges.size(), 1u);
    CHECK_EQ(parsed.tuning.rates.size(), 2u);
    const fernsdr::Json tuning = parsed.to_json()["tuning"];
    CHECK_EQ(tuning["ranges"][0][1].number(), 1766000000.0);
    CHECK_EQ(tuning["rates"][0].number(), 2400000.0);
    CHECK_EQ_STR(tuning["signal"].string(), "iq");
    CHECK(parse_module_manifest(manifest, parsed, error));
    CHECK(!parsed.to_json().has("tuning"));
    for (const std::string& tuning : {std::string(R"("x")"), std::string(R"({"ranges":[],"rates":[1000],"signal":"iq"})"),
                                      std::string(R"({"ranges":[[5,1]],"rates":[1000],"signal":"iq"})"),
                                      std::string(R"({"ranges":[[0,1]],"rates":[10],"signal":"iq"})"),
                                      std::string(R"({"ranges":[[0,1]],"rates":[1000],"signal":"both"})"),
                                      std::string(R"({"ranges":[[0,1.5]],"rates":[1000],"signal":"iq"})"),
                                      std::string(R"({"ranges":[[0,1]],"rates":[],"signal":"real"})")}) {
        CHECK(!parse_module_manifest(base + R"(,"tuning":)" + tuning + "}", parsed, error));
    }

    PackageName name;
    CHECK(parse_package_name("rtl-sdr-0.10.2-linux-aarch64.fernmod", name));
    CHECK_EQ_STR(name.id, "rtl-sdr");
    CHECK_EQ_STR(name.version, "0.10.2");
    CHECK_EQ_STR(name.platform, "linux-aarch64");
    CHECK(!parse_package_name("rtlsdr-0.1.0-linux-x86_64.tar.gz", name));
    CHECK(!parse_package_name("../x-0.1.0-linux-x86_64.fernmod", name));
    CHECK(compare_module_versions("0.10.0", "0.9.3") > 0);
    CHECK(compare_module_versions("1.0.0", "1.0.0") == 0);
}

TEST_CASE(module_settings_are_typed_by_what_the_module_declares) {
    ModuleManifest manifest;
    std::string error;
    const std::string program = "x";
    CHECK(parse_module_manifest(manifest_for(program, "1.0.0"), manifest, error));
    Json value;
    CHECK(type_module_setting(*manifest.setting("gain"), "38.6", value, error));
    CHECK_NEAR(value.number(), 38.6, 1e-9);
    CHECK(!type_module_setting(*manifest.setting("gain"), "1k", value, error));
    CHECK(!type_module_setting(*manifest.setting("gain"), "70", value, error));
    CHECK(!type_module_setting(*manifest.setting("gain"), "", value, error));
    CHECK(type_module_setting(*manifest.setting("flag"), "yes", value, error) && value.boolean());
    CHECK(type_module_setting(*manifest.setting("flag"), "Off", value, error) && !value.boolean(true));
    CHECK(!type_module_setting(*manifest.setting("flag"), "maybe", value, error));
    CHECK(type_module_setting(*manifest.setting("behaviour"), "stall", value, error));
    CHECK(!type_module_setting(*manifest.setting("behaviour"), "dance", value, error));
    // A serial number stays exactly as written.
    CHECK(type_module_setting(*manifest.setting("label"), "00000001", value, error));
    CHECK_EQ_STR(value.string(), "00000001");
    CHECK(!check_module_setting(*manifest.setting("gain"), Json("38"), value, error));
    CHECK(check_module_setting(*manifest.setting("gain"), Json(38.0), value, error));
}

TEST_CASE(module_store_installs_activates_and_removes_versions_safely) {
    TempDir directory;
    ModuleStore store(directory.path + "/modules");
    const std::string one = "program one";
    const std::string two = "program two";
    ModuleManifest installed;
    std::string error;
    CHECK(store.install(package_of(manifest_for(one, "1.0.0"), one), "someone/modules", nullptr, installed, error));
    CHECK(store.install(package_of(manifest_for(two, "1.1.0"), two), "someone/modules", nullptr, installed, error));
    ModuleStore::Module module;
    CHECK(store.find("fake", module));
    // The first version is what bands use; a later one waits to be activated.
    CHECK_EQ_STR(module.active, "1.0.0");
    CHECK_EQ(module.versions.size(), 2);
    CHECK_EQ_STR(module.versions[0].version, "1.1.0");
    CHECK_EQ_STR(module.origin, "someone/modules");

    // Same version, same bytes: nothing to do. Same version, other bytes: no.
    CHECK(store.install(package_of(manifest_for(one, "1.0.0"), one), "someone/modules", nullptr, installed, error));
    CHECK(!store.install(package_of(manifest_for(two, "1.0.0"), two), "someone/modules", nullptr, installed, error));
    // Another repository cannot take the id over.
    const std::string three = "program three";
    CHECK(!store.install(package_of(manifest_for(three, "2.0.0"), three), "other/modules", nullptr, installed, error));
    CHECK(error.find("someone/modules") != std::string::npos);
    // A published name that does not match the manifest is refused.
    PackageName promised{"fake", "9.9.9", module_platform()};
    CHECK(!store.install(package_of(manifest_for(three, "2.0.0"), three), "someone/modules", &promised, installed, error));
    // A package for another platform is refused.
    const std::string elsewhere = std::string(module_platform()) == "linux-aarch64" ? "linux-x86_64" : "linux-aarch64";
    CHECK(!store.install(package_of(manifest_for(three, "2.0.0", elsewhere), three), "someone/modules", nullptr,
                         installed, error));

    CHECK(store.activate("fake", "1.1.0", error));
    CHECK(store.find("fake", module) && module.active == "1.1.0");
    CHECK(!store.activate("fake", "3.0.0", error));
    CHECK(!store.activate("../fake", "1.0.0", error));

    // A running version cannot be removed, nor the version configured bands use.
    ModuleStore::Launch launch;
    CHECK(store.acquire("fake", launch, error) == ModuleStore::Resolve::Ok);
    CHECK_EQ_STR(launch.version, "1.1.0");
    CHECK(!store.remove("fake", "1.1.0", false, error));
    store.release("fake", "1.1.0");
    CHECK(!store.remove("fake", "1.1.0", true, error));

    // Removal follows no link planted inside a version directory.
    const std::string outside = directory.path + "/precious";
    { std::ofstream(outside) << "keep me"; }
    CHECK(::symlink(outside.c_str(), (directory.path + "/modules/fake/1.0.0/link").c_str()) == 0);
    CHECK(store.remove("fake", "1.0.0", true, error));
    struct stat info {};
    CHECK(::stat(outside.c_str(), &info) == 0);

    CHECK(store.set_enabled("fake", false, error));
    CHECK(store.acquire("fake", launch, error) == ModuleStore::Resolve::Disabled);
    CHECK(store.set_enabled("fake", true, error));
    CHECK(store.remove("fake", "1.1.0", false, error));
    CHECK(!store.find("fake", module));
    CHECK(store.acquire("fake", launch, error) == ModuleStore::Resolve::NotInstalled);
    // With the module gone, another repository may install the id.
    CHECK(store.install(package_of(manifest_for(three, "2.0.0"), three), "other/modules", nullptr, installed, error));
}

TEST_CASE(subprocess_run_gives_a_program_its_input_on_stdin) {
    using fernsdr::Subprocess;
    std::string output, errors, error;
    Subprocess::Exit status;
    const std::string small = "{\"hello\":1}";
    CHECK(Subprocess::run("/bin/cat", {}, {}, std::chrono::seconds(5), 1 << 20, output, errors, status, error,
                          nullptr, &small));
    CHECK_EQ_STR(output, small);
    // More than a pipe holds, to a program that answers while it reads: the
    // input and the output have to move together or both sides wait.
    const std::string large(200 * 1024, 'x');
    CHECK(Subprocess::run("/bin/cat", {}, {}, std::chrono::seconds(5), 1 << 20, output, errors, status, error,
                          nullptr, &large));
    CHECK_EQ(output.size(), large.size());
    CHECK(status.exited && status.code == 0);
    // A program that exits without reading: the rest of the input is dropped,
    // which is neither a failure nor a SIGPIPE for the receiver.
    CHECK(Subprocess::run("/bin/true", {}, {}, std::chrono::seconds(5), 1 << 20, output, errors, status, error,
                          nullptr, &large));
    CHECK(status.exited && status.code == 0);
    // What the caller's error string held before says nothing about this run:
    // the updater once passed one that still named a missing file, and a
    // systemctl that had worked counted as failed.
    error = "left over from something else";
    CHECK(Subprocess::run("/bin/true", {}, {}, std::chrono::seconds(5), 1 << 20, output, errors, status, error));
    CHECK(error.empty());
    // One that never reads is ended at its time limit like any other.
    CHECK(!Subprocess::run("/bin/sleep", {"30"}, {}, std::chrono::seconds(1), 1 << 20, output, errors, status, error,
                           nullptr, &large));
    CHECK(error.find("did not finish within 1 s") != std::string::npos);
}

TEST_CASE(module_processes_hold_only_the_descriptors_they_are_given) {
    const std::string fake = fake_module_path();
    // A descriptor opened without close-on-exec, as a library might.
    const int leaked = ::open("/dev/null", O_RDONLY);
    CHECK(leaked >= 0);
    for (bool listing : {false, true}) {
        Subprocess::force_descriptor_listing(listing);
        std::string output, errors, error;
        Subprocess::Exit status;
        CHECK(Subprocess::run(fake, {"--fds"}, module_environment(), milliseconds(5000), 4096, output, errors, status,
                              error));
        CHECK(status.exited && status.code == 0);
        CHECK_EQ_STR(output, "0 1 2\n");
    }
    Subprocess::force_descriptor_listing(false);
    ::close(leaked);
}

TEST_CASE(module_process_limits_are_enforced) {
    const std::string fake = fake_module_path();
    std::string output, errors, error;
    Subprocess::Exit status;
    CHECK(!Subprocess::run(fake, {"--list-devices"}, module_environment(), milliseconds(5000), 20, output, errors,
                           status, error));
    CHECK(error.find("more than") != std::string::npos);
    const auto started = std::chrono::steady_clock::now();
    CHECK(!Subprocess::run(fake, {"--ignore-term"}, module_environment(), milliseconds(300), 4096, output, errors,
                           status, error));
    CHECK(error.find("did not finish") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    CHECK(!Subprocess::run("fake-module", {}, {}, milliseconds(300), 10, output, errors, status, error));

    // SIGTERM ignored: the stop escalates to SIGKILL and still reaps it.
    Subprocess child;
    Subprocess::Options options;
    options.path = fake;
    options.arguments = {"--ignore-term"};
    options.environment = module_environment();
    options.streams = {Subprocess::Stream::Null, Subprocess::Stream::FromChild, Subprocess::Stream::Null};
    CHECK(child.start(options, error));
    const pid_t pid = child.pid();
    std::this_thread::sleep_for(milliseconds(100));
    const Subprocess::Exit ended = child.terminate({}, milliseconds(100), milliseconds(2000));
    CHECK(!ended.exited && ended.signal == SIGKILL);
    CHECK(::kill(pid, 0) != 0);
}

TEST_CASE(module_band_streams_takes_live_settings_and_stops_cleanly) {
    TempDir directory;
    auto store = store_with_fake(directory);
    CHECK(store != nullptr);
    if (!store) return;
    auto band = module_band(store, band_section("normal"));
    CHECK(band != nullptr);
    if (!band) return;
    std::string error;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
    if (!band->online()) std::fprintf(stderr, "  normal: %s\n", band->status().c_str());
    CHECK(band->status().empty());
    CHECK(band->healthy());
    const Json details = band->source_details();
    CHECK_EQ_STR(details["state"].string(), "streaming");
    CHECK_EQ_STR(details["version"].string(), "1.0.0");
    CHECK_EQ_STR(details["device"]["serial"].string(), "F00D");
    CHECK_EQ_STR(details["settings"]["label"].string(), "first");
    CHECK_EQ(details["live"].size(), 1);

    Json settings = Json::make_object();
    settings.set("gain", 21.0);
    Json sent;
    CHECK(band->apply_source_settings(settings, sent, error));
    CHECK(wait_until([&] { return band->source_details()["last_set"]["ok"].boolean(); }, milliseconds(2000)));
    CHECK_NEAR(band->source_details()["settings"]["gain"].number(), 21.0, 0);
    CHECK_NEAR(band->source_details()["last_set"]["id"].number(), sent["id"].number(), 0);
    Json not_live = Json::make_object();
    not_live.set("label", "second");
    CHECK(!band->apply_source_settings(not_live, sent, error));
    Json out_of_range = Json::make_object();
    out_of_range.set("gain", 99.0);
    CHECK(!band->apply_source_settings(out_of_range, sent, error));

    CHECK(wait_until([&] { return band->source_details()["samples"].number() > 20000; }, milliseconds(3000)));
    const auto started = std::chrono::steady_clock::now();
    band->stop();
    CHECK(std::chrono::steady_clock::now() - started < milliseconds(1500));
    CHECK_EQ_STR(band->source_details()["exit"].string(), "stopped as asked");
}

TEST_CASE(module_band_recovers_from_a_crash_and_a_stall) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    for (const char* behaviour : {"crash", "stall", "exit0"}) {
        auto band = module_band(store, band_section(behaviour));
        if (!band) return;
        std::string error;
        CHECK(band->start(error));
        CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
        // It fails, the band says why, and a second before the next attempt.
        CHECK(wait_until([&] { return band->status().find("offline:") == 0; }, milliseconds(3000)));
        CHECK(!band->online());
        CHECK(!band->healthy());
        if (std::string(behaviour) == "stall") CHECK(band->status().find("stopped delivering") != std::string::npos);
        if (std::string(behaviour) == "crash") CHECK(band->status().find("signal") != std::string::npos);
        CHECK(band->status().find("Trying again in 1 s") != std::string::npos);
        CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
        band->stop();
    }
}

TEST_CASE(module_band_takes_a_centre_a_few_hertz_beside) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    auto band = module_band(store, band_section("near-center"));
    if (!band) return;
    std::string error;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
    CHECK_NEAR(band->source_details()["center"].number(), 7100003.25, 1e-6);
    band->stop();
}

TEST_CASE(module_band_shows_what_clips_and_the_gain_the_module_chose) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    auto band = module_band(store, band_section("clipping"));
    if (!band) return;
    std::string error;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->source_details()["clipping"].number() > 0; }, milliseconds(4000)));
    const Json details = band->source_details();
    CHECK_NEAR(details["clipping"].number(), 0.1, 1e-9);
    CHECK_NEAR(details["gain_now"].number(), 22.9, 1e-9);
    band->stop();
    // A module that counts nothing says nothing.
    auto quiet = module_band(store, band_section("normal"));
    if (!quiet) return;
    CHECK(quiet->start(error));
    CHECK(wait_until([&] { return quiet->source_details()["samples"].number() > 20000; }, milliseconds(3000)));
    CHECK(!quiet->source_details().has("clipping"));
    CHECK(!quiet->source_details().has("gain_now"));
    quiet->stop();
}

TEST_CASE(module_band_waits_for_the_operator_and_stops_or_restarts_at_once) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    struct Case {
        const char* behaviour;
        const char* expect;
    };
    const Case cases[] = {
        {"invalid", "that gain is not possible here"},
        {"bad-rate", "runs at 48480 Hz"},
        {"bad-center", "is tuned to 7100050.0 Hz"},
        {"early", "wrote samples before it was ready"},
        {"huge", "longer than 64 KiB"},
    };
    for (const Case& c : cases) {
        auto band = module_band(store, band_section(c.behaviour));
        if (!band) return;
        std::string error;
        CHECK(band->start(error));
        CHECK(wait_until([&] { return band->status().find("waiting for the operator") == 0; }, milliseconds(4000)));
        if (band->status().find(c.expect) == std::string::npos) {
            std::fprintf(stderr, "  %s: %s\n", c.behaviour, band->status().c_str());
        }
        CHECK(band->status().find(c.expect) != std::string::npos);
        // A restart must not wait on a wait that has no end.
        auto started = std::chrono::steady_clock::now();
        CHECK(band->restart());
        CHECK(wait_until([&] { return !band->restarting(); }, milliseconds(3000)));
        CHECK(std::chrono::steady_clock::now() - started < milliseconds(3000));
        started = std::chrono::steady_clock::now();
        band->stop();
        CHECK(std::chrono::steady_clock::now() - started < milliseconds(1500));
    }
}

TEST_CASE(module_band_retries_a_missing_device_and_stops_during_the_wait) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    auto band = module_band(store, band_section("no-device"));
    if (!band) return;
    std::string error;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->status().find("offline: no fake radio is plugged in") == 0; },
                     milliseconds(3000)));
    // After two quick failures the wait is 4 s; stopping must not sit it out.
    CHECK(wait_until([&] { return band->status().find("Trying again in 4 s") != std::string::npos; },
                     milliseconds(6000)));
    const auto started = std::chrono::steady_clock::now();
    band->stop();
    CHECK(std::chrono::steady_clock::now() - started < milliseconds(500));
    CHECK(!band->running());
}

TEST_CASE(module_band_retrying_the_same_failure_logs_it_once) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    auto band = module_band(store, band_section("no-device"));
    if (!band) return;
    // The receiver's log, not the band's: that is what an unplugged dongle
    // was filling. The ring is shared by the whole process, so only what
    // this test adds is counted; the other tests log at level none.
    set_log_level(LogLevel::Info);
    const auto count = [](const std::string& text) {
        int found = 0;
        for (const std::string& line : LogRing::instance().snapshot()) {
            if (line.find(text) != std::string::npos) found++;
        }
        return found;
    };
    const int started_before = count("[band] mod: started fake");
    const int opened_before = count("[module] mod fake: open: no-device");
    const int warned_before = count("[band] mod is offline");
    std::string error;
    CHECK(band->start(error));
    // Three attempts: the first, then after 1 s and 2 s.
    CHECK(wait_until([&] { return band->status().find("Trying again in 4 s") != std::string::npos; },
                     milliseconds(6000)));
    band->stop();
    set_log_level(LogLevel::None);
    CHECK_EQ(count("[band] mod: started fake") - started_before, 1);
    CHECK_EQ(count("[module] mod fake: open: no-device") - opened_before, 1);
    CHECK_EQ(count("[band] mod is offline") - warned_before, 1);
    // Nothing is lost for the operator: the band keeps every attempt's lines.
    int kept = 0;
    const Json details = band->source_details();
    for (const Json& line : details["log"].elements()) {
        if (line.string().find("open: no-device") != std::string::npos) kept++;
    }
    CHECK(kept >= 3);
}

TEST_CASE(module_band_that_ignores_stop_is_killed) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    auto band = module_band(store, band_section("ignore-stop"));
    if (!band) return;
    std::string error;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
    if (!band->online()) std::fprintf(stderr, "  ignore-stop: %s / %s\n", band->status().c_str(), band->source_details().serialize().c_str());
    const int pid = static_cast<int>(band->source_details()["pid"].number());
    CHECK(pid > 0);
    const auto started = std::chrono::steady_clock::now();
    band->stop();
    // 300 ms for stop, 300 ms after SIGTERM, then SIGKILL.
    CHECK(std::chrono::steady_clock::now() - started < milliseconds(2500));
    CHECK(pid > 0 && ::kill(pid, 0) != 0);
    CHECK_EQ_STR(band->source_details()["exit"].string(), "did not stop when asked, and was killed");
}

TEST_CASE(module_band_reports_disabled_and_missing_modules) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    std::string error;
    CHECK(store->set_enabled("fake", false, error));
    auto band = module_band(store, band_section("normal"));
    if (!band) return;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->status().find("switched off") == 0; }, milliseconds(3000)));
    // Deliberately off is not a fault to page anyone about.
    CHECK(band->healthy());
    CHECK(!band->online());
    CHECK(store->set_enabled("fake", true, error));
    CHECK(band->restart());
    CHECK(wait_until([&] { return band->online(); }, milliseconds(4000)));
    band->stop();

    auto empty = std::make_shared<ModuleStore>(directory.path + "/nothing");
    auto orphan = module_band(empty, band_section("normal"));
    if (!orphan) return;
    CHECK(orphan->start(error));
    CHECK(wait_until([&] { return orphan->status().find("waiting for the operator: module fake is not installed") == 0; },
                     milliseconds(3000)));
    CHECK(!orphan->healthy());
    orphan->stop();
}

TEST_CASE(module_band_hands_over_only_its_pipes_and_caps_the_log) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    for (bool listing : {false, true}) {
        Subprocess::force_descriptor_listing(listing);
        auto band = module_band(store, band_section("fds"));
        if (!band) return;
        std::string error;
        CHECK(band->start(error));
        CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
        CHECK(wait_until([&] { return log_contains(*band, "fds: 0 1 2 3"); }, milliseconds(2000)));
        band->stop();
    }
    Subprocess::force_descriptor_listing(false);

    auto flood = module_band(store, band_section("flood"));
    if (!flood) return;
    std::string error;
    CHECK(flood->start(error));
    CHECK(wait_until([&] { return log_contains(*flood, "flood line 4999"); }, milliseconds(4000)));
    // The samples come after the flood, and still arrive.
    CHECK(wait_until([&] { return flood->online(); }, milliseconds(2000)));
    CHECK_EQ(flood->source_details()["log"].size(), 200);
    flood->stop();
}

TEST_CASE(module_band_takes_new_settings_on_restart) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    auto band = module_band(store, band_section("normal"));
    if (!band) return;
    std::string error;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
    ConfigSection changed = band_section("normal");
    changed.set("module.label", "second");
    CHECK(band->reconfigure_source(changed, error));
    CHECK_EQ_STR(band->configured_values().at("module.label"), "second");
    CHECK(band->restart());
    CHECK(wait_until([&] { return band->source_details()["settings"]["label"].string() == "second"; },
                     milliseconds(4000)));
    ConfigSection reserved = band_section("normal");
    reserved.set("module.center", "1");
    CHECK(!band->reconfigure_source(reserved, error));
    band->stop();
}

TEST_CASE(module_sections_are_checked_against_the_installed_module) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    std::string error;
    CHECK(make_module_source(band_section("normal", "module.gain", "20"), store, error) != nullptr);
    CHECK(make_module_source(band_section("normal", "module.gian", "20"), store, error) == nullptr);
    CHECK(error.find("no setting 'gian'") != std::string::npos);
    CHECK(make_module_source(band_section("normal", "module.gain", "loud"), store, error) == nullptr);
    CHECK(make_module_source(band_section("normal", "module.sample_rate", "1"), store, error) == nullptr);
    CHECK(make_module_source(band_section("normal", "format", "u8"), store, error) == nullptr);
    ConfigSection bad_id = band_section("normal");
    bad_id.set("module", "../../bin/sh");
    CHECK(make_module_source(bad_id, store, error) == nullptr);
    // Not installed yet: accepted, and checked when the band starts.
    auto empty = std::make_shared<ModuleStore>(directory.path + "/nothing");
    CHECK(make_module_source(band_section("normal", "module.anything", "1"), empty, error) != nullptr);
}

TEST_CASE(module_manager_runs_jobs_and_publishes_what_the_panel_shows) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    std::vector<std::string> changed;
    ModuleManager manager(store, {"someone/modules", "not a repository"}, false);
    manager.set_bands_callback([](const std::string& id) {
        return id == "fake" ? std::vector<std::string>{"mod"} : std::vector<std::string>{};
    });
    manager.set_changed_callback([&](const std::string& id) { changed.push_back(id); });
    manager.publish();
    CHECK_EQ(manager.catalog().size(), 1);

    Json view;
    CHECK(Json::parse(manager.snapshot(), view));
    CHECK_EQ(view["installed"].size(), 1);
    CHECK_EQ_STR(view["installed"][0]["id"].string(), "fake");
    CHECK_EQ_STR(view["installed"][0]["bands"][0].string(), "mod");

    std::string error;
    CHECK(manager.list_devices("fake", error));
    CHECK(manager.set_enabled("fake", false, error));
    CHECK(!manager.install("elsewhere/modules", "v1", "fake-1.0.0-linux-x86_64.fernmod", false, error));
    CHECK(!manager.install("someone/modules", "../v1", "fake-1.0.0-linux-x86_64.fernmod", false, error));
    CHECK(!manager.activate("fake", "../1.0.0", error));
    manager.run_pending();
    CHECK_EQ(changed.size(), 1);
    CHECK(Json::parse(manager.snapshot(), view));
    CHECK(!view["installed"][0]["enabled"].boolean(true));
    const Json& devices = view["devices"]["fake"]["devices"];
    CHECK_EQ(devices.size(), 2);
    CHECK_EQ_STR(devices[0]["serial"].string(), "F00D");
    // Only the documented fields reach the panel.
    CHECK(!devices[0].has("secret"));
    CHECK(!devices[1]["usable"].boolean(true));
    CHECK_EQ(view["jobs"].size(), 2);
    CHECK_EQ_STR(view["jobs"][0]["state"].string(), "done");

    // The version bands use cannot be removed while bands are configured with it.
    CHECK(manager.remove("fake", "1.0.0", error));
    manager.run_pending();
    CHECK(Json::parse(manager.snapshot(), view));
    CHECK_EQ_STR(view["jobs"][0]["state"].string(), "failed");
    CHECK_EQ(view["installed"].size(), 1);
}

TEST_CASE(github_release_lists_are_reduced_to_what_can_be_installed) {
    const std::string platform = module_platform();
    const std::string body = R"([
      {"tag_name":"v0.2.0","name":"Second","draft":false,"prerelease":false,"published_at":"2026-09-01T00:00:00Z",
       "assets":[{"name":"rtlsdr-0.2.0-linux-armhf.fernmod","size":1000},
                 {"name":"rtlsdr-0.2.0-)" + platform + R"(.fernmod","size":2000,"browser_download_url":"http://evil/x"}]},
      {"tag_name":"v0.3.0-rc1","draft":true,"assets":[]},
      {"tag_name":"../../x","draft":false,"assets":[]},
      {"tag_name":"v0.1.0","prerelease":true,"assets":[{"name":"notes.txt","size":10}]}
    ])";
    std::vector<CatalogRelease> releases;
    std::string error;
    CHECK(parse_github_releases(body, releases, error));
    CHECK_EQ(releases.size(), 2);
    CHECK_EQ_STR(releases[0].tag, "v0.2.0");
    if (!platform.empty()) {
        CHECK_EQ_STR(releases[0].asset, "rtlsdr-0.2.0-" + platform + ".fernmod");
        CHECK_EQ_STR(releases[0].package.version, "0.2.0");
    }
    CHECK(releases[1].prerelease);
    CHECK(releases[1].asset.empty());
    CHECK(!parse_github_releases("{\"message\":\"API rate limit exceeded\"}", releases, error));
    CHECK(valid_catalog_repository("Steven9101/Fern-RTLSDR"));
    CHECK(!valid_catalog_repository("Steven9101/../x"));
    CHECK(!valid_catalog_repository("https://github.com/x/y"));
    CHECK(!valid_catalog_repository("a/b/c"));
}

TEST_CASE(module_store_works_from_a_relative_directory) {
    // `fernsdr fernsdr.conf` run in the config's own directory leaves the
    // modules directory relative; the module must still start by an
    // absolute path.
    TempDir directory;
    char previous[4096];
    CHECK(::getcwd(previous, sizeof(previous)) != nullptr);
    CHECK(::chdir(directory.path.c_str()) == 0);
    ModuleStore store("modules/");
    CHECK_EQ_STR(store.directory(), directory.path + "/modules");
    const std::string program = read_binary(fake_module_path());
    ModuleManifest installed;
    std::string error;
    CHECK(store.install(package_of(manifest_for(program, "1.0.0"), program), "file", nullptr, installed, error));
    ModuleStore::Launch launch;
    CHECK(store.acquire("fake", launch, error) == ModuleStore::Resolve::Ok);
    CHECK(!launch.executable.empty() && launch.executable[0] == '/');
    store.release("fake", launch.version);
    CHECK(::chdir(previous) == 0);
}

TEST_CASE(module_reports_cannot_grow_without_bound) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    auto band = module_band(store, band_section("applied-flood"));
    if (!band) return;
    std::string error;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->online(); }, milliseconds(3000)));
    std::this_thread::sleep_for(milliseconds(500));
    const Json settings = band->source_details()["settings"];
    CHECK(settings.size() <= 64);
    CHECK_EQ_STR(settings["label"].string(), "first");
    band->stop();
}

namespace {

long helper_pid(const Band& band) {
    const Json details = band.source_details();
    for (const Json& line : details["log"].elements()) {
        const std::string text = line.string();
        if (text.rfind("helper: ", 0) == 0) return std::atol(text.c_str() + 8);
    }
    return 0;
}

}  // namespace

TEST_CASE(module_helpers_end_with_the_module) {
    TempDir directory;
    auto store = store_with_fake(directory);
    if (!store) return;
    // Stopped by the band, and ending on its own: either way nothing it
    // started may keep the device.
    for (const char* behaviour : {"helper", "helper-exit"}) {
        auto band = module_band(store, band_section(behaviour));
        if (!band) return;
        std::string error;
        CHECK(band->start(error));
        CHECK(wait_until([&] { return helper_pid(*band) > 0; }, milliseconds(3000)));
        const long helper = helper_pid(*band);
        CHECK(::kill(static_cast<pid_t>(helper), 0) == 0);
        if (std::string(behaviour) == "helper") {
            band->stop();
        } else {
            CHECK(wait_until([&] { return band->status().find("offline:") == 0; }, milliseconds(3000)));
            band->stop();
        }
        CHECK(wait_until([&] { return ::kill(static_cast<pid_t>(helper), 0) != 0; }, milliseconds(2000)));
    }
}

TEST_CASE(module_that_cannot_be_executed_waits_for_the_operator) {
    // qemu-user, which `make release-test` runs ARM builds under, turns the
    // vfork inside glibc's posix_spawn into a fork, so a failed exec cannot
    // reach the parent: the spawn succeeds and the child exits 127. Linux
    // itself reports the error, on ARM as anywhere.
    if (std::getenv("FERNSDR_TEST_UNDER_QEMU")) {
        std::fprintf(stderr, "  skipped under qemu-user, which cannot report a failed exec from posix_spawn\n");
        return;
    }
    TempDir directory;
    auto store = std::make_shared<ModuleStore>(directory.path + "/modules");
    const std::string garbage(4096, 'x');
    ModuleManifest installed;
    std::string error;
    CHECK(store->install(package_of(manifest_for(garbage, "1.0.0"), garbage), "file", nullptr, installed, error));
    auto band = module_band(store, band_section("normal"));
    if (!band) return;
    CHECK(band->start(error));
    CHECK(wait_until([&] { return band->status().find("waiting for the operator: cannot run") == 0; },
                     milliseconds(3000)));
    band->stop();
}

TEST_CASE(two_bands_cannot_take_one_module_device) {
    const auto configure = [](const std::string& bands, std::string& error) {
        fernsdr::Config config;
        CHECK(config.parse("[modules]\ndirectory = /nonexistent-fernsdr-modules\n" + bands, error));
        fernsdr::Radio radio;
        return radio.configure(config, error);
    };
    const auto band = [](const std::string& id, const std::string& device) {
        return "[band:" + id + "]\nsource = module\nmodule = rtlsdr\nsample_rate = 2048k\ncenter = 14.1M\n" +
               (device.empty() ? "" : "module.device = " + device + "\n");
    };
    std::string error;
    CHECK(!configure(band("a", "") + band("b", ""), error));
    CHECK(error.find("both take the first rtlsdr device") != std::string::npos);
    CHECK(!configure(band("a", "serial:00000001") + band("b", " SERIAL:00000001 "), error));
    CHECK(error.find("both name rtlsdr device 'serial:00000001'") != std::string::npos);
    // Two devices, or one band with the first device and one with a named one, are fine as far
    // as the configuration can tell.
    error.clear();
    configure(band("a", "serial:00000001") + band("b", "serial:00000002"), error);
    CHECK(error.find("both") == std::string::npos);
    error.clear();
    configure(band("a", "") + band("b", "serial:00000002"), error);
    CHECK(error.find("both") == std::string::npos);
}

namespace {

// A store with the fake module whose manifest says what it tunes.
std::shared_ptr<ModuleStore> store_with_tuning(const TempDir& directory, const std::string& tuning) {
    auto store = std::make_shared<ModuleStore>(directory.path + "/modules");
    const std::string program = read_binary(fake_module_path());
    std::string manifest = manifest_for(program, "1.0.0");
    manifest = manifest.substr(0, manifest.size() - 1) + R"(,"tuning":)" + tuning + "}";
    ModuleManifest installed;
    std::string error;
    if (!store->install(package_of(manifest, program), "file", nullptr, installed, error)) {
        std::fprintf(stderr, "installing the fake module failed: %s\n", error.c_str());
        return nullptr;
    }
    return store;
}

ConfigSection plain_module_band() {
    ConfigSection section("band:mod");
    section.set("source", "module");
    section.set("module", "fake");
    return section;
}

}  // namespace

TEST_CASE(module_band_takes_signal_and_rate_from_what_the_module_says) {
    // An RX-888-like module: real, 64.8 Msps first. A band that says only
    // which module gets a real band at that rate, from 0 Hz.
    TempDir directory;
    auto store = store_with_tuning(directory, R"({"ranges":[[0,64800000]],"rates":[64800000,129600000],"signal":"real"})");
    CHECK(store != nullptr);
    if (!store) return;
    std::string error;
    auto source = make_module_source(plain_module_band(), store, error, quick_timing());
    CHECK(source != nullptr);
    if (!source) return;
    CHECK(source->kind() == fernsdr::SignalKind::Real);
    CHECK_EQ(source->sample_rate(), 64800000.0);
}

TEST_CASE(module_band_that_contradicts_the_module_is_refused_with_the_fix) {
    TempDir directory;
    auto real = store_with_tuning(directory, R"({"ranges":[[0,64800000]],"rates":[64800000],"signal":"real"})");
    CHECK(real != nullptr);
    if (!real) return;
    std::string error;
    ConfigSection iq = plain_module_band();
    iq.set("signal", "iq");
    CHECK(make_module_source(iq, real, error, quick_timing()) == nullptr);
    CHECK(error.find("set signal = real") != std::string::npos);
    ConfigSection centred = plain_module_band();
    centred.set("center", "7100000");
    CHECK(make_module_source(centred, real, error, quick_timing()) == nullptr);
    CHECK(error.find("center is 0") != std::string::npos);

    TempDir other;
    auto tuner = store_with_tuning(other, R"({"ranges":[[500000,1766000000]],"rates":[2400000],"signal":"iq"})");
    CHECK(tuner != nullptr);
    if (!tuner) return;
    ConfigSection low = plain_module_band();
    low.set("center", "100000");
    CHECK(make_module_source(low, tuner, error, quick_timing()) == nullptr);
    CHECK(error.find("0.500 to 1766.000 MHz") != std::string::npos);
    ConfigSection fine = plain_module_band();
    fine.set("center", "7100000");
    auto source = make_module_source(fine, tuner, error, quick_timing());
    CHECK(source != nullptr);
    if (source) CHECK_EQ(source->sample_rate(), 2400000.0);
}

TEST_CASE(band_values_set_by_hand_are_refused_when_saved_and_replaced_at_a_start) {
    // fft_size 2, spectrum_bins not a power of two, a range past what the
    // input covers: the panel refuses each with the reason; a receiver that
    // starts with the same file runs the band on its defaults.
    const char* cases[] = {"fft_size = 2\n", "spectrum_bins = 100000\n", "low = 7000000\nhigh = 9000000\n",
                           "low = 7200000\nhigh = 7100000\n"};
    const char* reasons[] = {"fft_size is a power of two", "spectrum_bins is a power of two",
                             "must lie in what the input covers", "low must be below high"};
    for (size_t i = 0; i < 4; i++) {
        fernsdr::Config config;
        std::string error;
        CHECK(config.parse(std::string("[band:t]\nsource = test\nsample_rate = 192000\ncenter = 7100000\n") + cases[i],
                           error));
        fernsdr::Radio saved;
        CHECK(!saved.configure(config, error, true));
        CHECK(error.find(reasons[i]) != std::string::npos);
        fernsdr::Radio started;
        CHECK(started.configure(config, error));
    }
}

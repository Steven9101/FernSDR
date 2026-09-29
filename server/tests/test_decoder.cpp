// The decoder runner against tests/support/fake_decoder.cpp, a real program
// that plays a good decoder and every kind of bad one.
#include "../src/core/band.h"
#include "../src/core/decoder.h"
#include "../src/source/source.h"
#include "../src/util/config.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace fernsdr;
using namespace std::chrono_literals;

namespace {

std::string beside_tests(const char* name) {
    char path[4096];
    const ssize_t size = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    const std::string self(path, size > 0 ? static_cast<size_t>(size) : 0);
    return self.substr(0, self.find_last_of('/')) + "/" + name;
}

struct Rig {
    ConfigSection section{"band:40m"};
    std::unique_ptr<Band> band;
    DecodeStore store;
    std::unique_ptr<Decoder> decoder;

    explicit Rig(const std::string& behaviour, bool sandboxed = false) {
        section.set("source", "test");
        section.set("sample_rate", "48000");
        section.set("center", "7074000");
        section.set("fft_size", "4096");
        section.set("realtime", "true");
        std::string error;
        band = std::make_unique<Band>("40m", "40 m", make_source(section, error), section);
        DecoderConfig config;
        config.id = "ft8";
        config.module = "fake";
        config.settings.set("behaviour", behaviour);
        DecoderChannelConfig channel;
        channel.band = "40m";
        channel.dial_hz = 7074000.0 - 5000.0;
        config.channels.push_back(channel);
        decoder = std::make_unique<Decoder>(
            config,
            [sandboxed](const std::string&, Decoder::Program& out, std::string&) {
                out.executable = beside_tests("fake-decoder");
                if (sandboxed) out.launcher = beside_tests("fernsdr");
                return true;
            },
            [this](const std::string& id) { return id == "40m" ? band.get() : nullptr; }, store);
        CHECK(band->start(error));
        CHECK(decoder->start(error));
    }
    ~Rig() {
        decoder->stop();
        band->stop();
    }
    template <typename Done>
    bool wait(Done done, std::chrono::milliseconds limit = 8000ms) {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            if (done()) return true;
            std::this_thread::sleep_for(20ms);
        }
        return done();
    }
};

// The sandbox is entered through the receiver's own program, which qemu-user
// (under which `make release-test` runs ARM builds) hands to the host kernel
// to exec: an ARM program it cannot run, so the child exits 127. The
// sandbox itself is exercised by these tests on every native build.
bool sandbox_unavailable_under_qemu() {
    if (!std::getenv("FERNSDR_TEST_UNDER_QEMU")) return false;
    std::fprintf(stderr, "  skipped under qemu-user, which cannot exec the ARM program that enters the sandbox\n");
    return true;
}

}  // namespace

TEST_CASE(a_decoder_gets_its_channel_and_its_decodes_are_kept) {
    Rig rig("good");
    CHECK(rig.wait([&] { return rig.store.size() >= 2; }));
    const auto decodes = rig.store.since(0, 10);
    CHECK(!decodes.empty());
    CHECK_EQ_STR(decodes[0].decoder, "ft8");
    CHECK_EQ_STR(decodes[0].band, "40m");
    CHECK_EQ_STR(decodes[0].call, "DL1ABC");
    CHECK_EQ_STR(decodes[0].grid, "JO31");
    CHECK_EQ(decodes[0].time_ms % 15000, 0);
    const Json status = rig.decoder->status();
    CHECK_EQ_STR(status["state"].string(), "running");
    CHECK_EQ_STR(status["version"].string(), "1.2.3");
    CHECK(status["channels"][0]["frames_sent"].number() >= 16);
    // The fake stops with an error if frames were not contiguous from 0.
    CHECK_EQ_STR(status["message"].string(), "");
}

TEST_CASE(a_decoder_cannot_slip_a_bad_decode_through) {
    Rig rig("bad");
    CHECK(rig.wait([&] { return rig.decoder->status()["rejected"].number() >= 4; }));
    CHECK_EQ(rig.store.size(), 0u);
}

TEST_CASE(a_decoder_gets_at_most_two_hundred_decodes_a_slot) {
    Rig rig("flood");
    CHECK(rig.wait([&] { return rig.decoder->status()["rejected"].number() >= 100; }));
    CHECK_EQ(rig.store.size(), 200u);
}

TEST_CASE(a_decoder_that_crashes_is_started_again) {
    Rig rig("crash");
    CHECK(rig.wait([&] { return rig.decoder->status()["restarts"].number() >= 1; }, 6000ms));
    CHECK(rig.decoder->status()["state"].string() != std::string("stopped"));
}

TEST_CASE(a_restart_asked_for_between_attempts_starts_the_module_at_once) {
    Rig rig("crash");
    // Crashing at once, it waits 1, 2, then 4 seconds: catch it in that wait.
    CHECK(rig.wait([&] { return rig.decoder->status()["message"].string().find("again in 4 s") != std::string::npos; },
                   8000ms));
    const double before = rig.decoder->status()["restarts"].number();
    rig.decoder->restart();
    CHECK(rig.wait([&] { return rig.decoder->status()["restarts"].number() > before; }, 1500ms));
}

TEST_CASE(a_slow_decoder_loses_whole_frames_and_is_told) {
    Rig rig("slow");
    CHECK(rig.wait([&] { return rig.decoder->status()["channels"][0]["frames_dropped"].number() >= 1; }, 9000ms));
    // Reading again, it sees a frame that says samples were lost before it.
    CHECK(rig.wait([&] {
        const Json stats = rig.decoder->status()["module_stats"];
        return stats.size() > 0 && stats[0]["late"].number() >= 1;
    }, 9000ms));
}

TEST_CASE(a_decoder_in_its_sandbox_can_open_no_file) {
    if (sandbox_unavailable_under_qemu()) return;
    Rig rig("probe", true);
    CHECK(rig.wait([&] { return rig.decoder->status()["message"].string().find("file open") != std::string::npos; }));
    CHECK_EQ_STR(rig.decoder->status()["message"].string(), "file open denied");
}

#include "../src/core/module_store.h"
#include "../src/core/radio.h"
#include "../src/util/password.h"

#include <cstdlib>

namespace {

std::string read_all(const std::string& path) {
    std::string out;
    read_text_file(path, out);
    return out;
}

std::string sha256_of(const std::string& data) {
    Sha256 hash;
    hash.update(data);
    uint8_t digest[32];
    hash.finish(digest);
    return to_hex(digest, 32);
}

struct Scratch {
    std::string path;
    Scratch() {
        char pattern[] = "/tmp/fernsdr-decoders-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        path = made ? made : "";
    }
    ~Scratch() {
        if (!path.empty()) (void)!std::system(("rm -rf '" + path + "'").c_str());
    }
};

// The fake decoder installed as a package, the way a download would be.
bool install_fake_decoder(const std::string& modules, const std::string& kind = "decoder", int api = 2) {
    ModuleStore store(modules);
    const std::string program = read_all(beside_tests("fake-decoder"));
    const std::string manifest = std::string(R"({"schema":1,"id":"fake","name":"Fake","version":"1.2.3","kind":")") + kind +
        R"(","api":)" + std::to_string(api) + R"(,"platform":")" + module_platform() + R"(","size":)" +
        std::to_string(program.size()) + R"(,"sha256":")" + sha256_of(program) +
        R"(","license":"AGPL-3.0","source":"tests","description":"test double",)" +
        R"("settings":[{"key":"behaviour","type":"string","label":"Behaviour"}]})";
    ModuleManifest installed;
    std::string error;
    const bool ok = store.install("FERNMOD1\n" + std::to_string(manifest.size()) + "\n" + manifest + program, "file",
                                  nullptr, installed, error);
    if (!ok) std::fprintf(stderr, "installing the fake decoder failed: %s\n", error.c_str());
    return ok;
}

std::string receiver_config(const std::string& modules, const std::string& decoder) {
    return "[modules]\ndirectory = " + modules +
           "\n[band:40m]\nsource = test\nsample_rate = 48000\ncenter = 7074000\nfft_size = 4096\nrealtime = true\n" +
           decoder;
}

}  // namespace

TEST_CASE(decoder_sections_are_checked_before_anything_runs) {
    Scratch scratch;
    const auto refused = [&](const std::string& decoder, const char* expect) {
        Config config;
        std::string error;
        CHECK(config.parse(receiver_config(scratch.path + "/modules", decoder), error));
        Radio radio;
        const bool configured = radio.configure(config, error);
        CHECK(!configured);
        if (error.find(expect) == std::string::npos) std::fprintf(stderr, "  said: %s\n", error.c_str());
        CHECK(error.find(expect) != std::string::npos);
    };
    refused("[decoder:ft8]\nchannels = 40m:7069000\n", "module =");
    refused("[decoder:ft8]\nmodule = fake\n", "channels =");
    refused("[decoder:ft8]\nmodule = fake\nchannels = 20m:14074000\n", "configured band");
    refused("[decoder:ft8]\nmodule = fake\nchannels = 40m:14074000\n", "reaches outside");
    refused("[decoder:ft8]\nmodule = fake\nchannels = 40m:7069000 40m:7.069M\n", "twice");
    refused("[decoder:FT8!]\nmodule = fake\nchannels = 40m:7069000\n", "lowercase");
    refused("[decoder:ft8]\nmodule = fake\nchannels = 40m:7069000\npublic = maybe\n", "public must be yes or no");
    refused("[decoder:ft8]\nmodule = fake\nchannels = 40m:7069000\n[decoder:ft8]\nmodule = fake\nchannels = 40m:7070000\n",
            "appears twice");

    Config config;
    std::string error;
    CHECK(config.parse(receiver_config(scratch.path + "/modules",
                                       "[decoder:ft8]\nmodule = fake\nchannels = 40m:7.069M\nmodule.behaviour = good\n"), error));
    Radio radio;
    CHECK(radio.configure(config, error));
    CHECK_EQ(radio.decoder_configs().size(), 1u);
    CHECK_EQ(radio.decoder_configs()[0].channels[0].dial_hz, 7069000.0);
    CHECK_EQ_STR(radio.decoder_configs()[0].channels[0].id(), "40m-ft8-7069");
    CHECK(!radio.decoder_configs()[0].listed);
    CHECK_EQ(radio.listed_decoders_json().size(), 0u);
}

TEST_CASE(a_configured_decoder_runs_from_its_package_in_its_sandbox) {
    if (sandbox_unavailable_under_qemu()) return;
    Scratch scratch;
    const std::string modules = scratch.path + "/modules";
    CHECK(install_fake_decoder(modules));
    Config config;
    std::string error;
    CHECK(config.parse(receiver_config(modules, "[decoder:ft8]\nmodule = fake\nchannels = 40m:7069000\nmodule.behaviour = good\n"),
                       error));
    Radio radio;
    radio.set_decoder_launcher(beside_tests("fernsdr"));
    CHECK(radio.configure(config, error));
    CHECK(radio.start(error));
    bool decoded = false;
    for (int i = 0; i < 400 && !decoded; i++) {
        decoded = radio.decodes().size() > 0;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(decoded);
    CHECK_EQ(radio.decoders_status().size(), 1u);
    CHECK_EQ_STR(radio.decoders_status()[0]["version"].string(), "1.2.3");
    // Not reporting: spots would name only the receiver.
    CHECK(radio.spot_software().find(" / ") == std::string::npos);
    radio.stop();
}

TEST_CASE(an_input_module_is_not_taken_for_a_decoder) {
    Scratch scratch;
    const std::string modules = scratch.path + "/modules";
    CHECK(install_fake_decoder(modules, "input", 1));
    Config config;
    std::string error;
    CHECK(config.parse(receiver_config(modules, "[decoder:ft8]\nmodule = fake\nchannels = 40m:7069000\n"), error));
    Radio radio;
    radio.set_decoder_launcher(beside_tests("fernsdr"));
    CHECK(radio.configure(config, error));
    CHECK(radio.start(error));
    bool refused = false;
    for (int i = 0; i < 200 && !refused; i++) {
        refused = radio.decoders_status()[0]["message"].string().find("not a decoder module") != std::string::npos;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(refused);
    radio.stop();
}

TEST_CASE(a_page_reads_the_newest_decodes_then_follows_along) {
    DecodeStore store(100);
    for (int i = 0; i < 10; i++) {
        Decode decode;
        decode.decoder = i % 2 ? "odd" : "even";
        decode.message = "M" + std::to_string(i);
        store.add(decode, 1000);
    }
    const DecodeStore::Filter even = [](const Decode& d) { return d.decoder == "even"; };
    uint64_t through = 0;
    auto newest = store.latest(2, even, through);
    CHECK_EQ(newest.size(), 2u);
    CHECK_EQ_STR(newest[0].message, "M6");
    CHECK_EQ_STR(newest[1].message, "M8");
    CHECK_EQ(through, 10u);

    // Stopping at the limit leaves `through` just before the first decode
    // not handed out, so the next request starts with it.
    auto first = store.since(0, 2, even, through);
    CHECK_EQ(first.size(), 2u);
    CHECK_EQ_STR(first[1].message, "M2");
    CHECK_EQ(through, 4u);
    auto rest = store.since(through, 10, even, through);
    CHECK_EQ(rest.size(), 3u);
    CHECK_EQ_STR(rest[0].message, "M4");
    CHECK_EQ(through, 10u);
    CHECK_EQ(store.since(through, 10, even, through).size(), 0u);
    CHECK_EQ(through, 10u);
    // A page that read a receiver since restarted asks after a number this
    // store never reached: it hears where this one stands, and its epoch.
    CHECK_EQ(store.since(5000, 10, even, through).size(), 0u);
    CHECK_EQ(through, 10u);
    CHECK_EQ(store.epoch().size(), 16u);
    CHECK(store.epoch() != DecodeStore().epoch());
}

TEST_CASE(decoder_sections_apply_to_a_running_receiver_restarting_only_what_changed) {
    Scratch scratch;
    const std::string modules = scratch.path + "/modules";
    CHECK(install_fake_decoder(modules));
    const auto config_with = [&](const std::string& decoders) {
        Config config;
        std::string error;
        CHECK(config.parse(receiver_config(modules, decoders), error));
        return config;
    };
    const std::string a = "[decoder:a]\nmodule = fake\nchannels = 40m:7069000\nmodule.behaviour = good\n";
    const std::string b = "[decoder:b]\nmodule = fake\nchannels = 40m:7070000\nmodule.behaviour = good\n";
    Radio radio;
    radio.set_decoder_launcher(beside_tests("fernsdr"));
    std::string error;
    CHECK(radio.configure(config_with(a), error));
    CHECK(radio.start(error));
    const auto ids = [&] {
        std::string out;
        const Json list = radio.decoders_status();
        for (size_t i = 0; i < list.size(); i++) out += list[i]["id"].string() + " ";
        return out;
    };
    const auto joined = [](const std::vector<std::string>& list) {
        std::string out;
        for (const auto& id : list) out += id + " ";
        return out;
    };
    CHECK_EQ_STR(ids(), "a ");

    CHECK_EQ_STR(joined(radio.apply_decoders(config_with(a + b)).changed), "b ");
    CHECK_EQ_STR(ids(), "a b ");
    // Made public: nothing restarts, and listeners now see it.
    CHECK_EQ_STR(joined(radio.apply_decoders(config_with(a + b + "public = yes\n")).changed), "");
    CHECK(radio.decoder_listed("b"));
    CHECK(!radio.decoder_listed("a"));
    CHECK_EQ(radio.decoders_status()[1]["public"].boolean(), true);
    // A new channel restarts that one only.
    const std::string a2 = "[decoder:a]\nmodule = fake\nchannels = 40m:7069000 40m:7072000\nmodule.behaviour = good\n";
    CHECK_EQ_STR(joined(radio.apply_decoders(config_with(a2 + b)).changed), "a ");
    CHECK_EQ(radio.decoders_status().size(), 2u);
    CHECK(!radio.decoder_listed("b"));
    // A save that adds a band, which needs a restart, and a decoder on it:
    // the new decoder waits for the restart, and the rest still applies.
    const std::string band20 = "[band:20m]\nsource = test\nsample_rate = 48000\ncenter = 14074000\nrealtime = true\n";
    const std::string c = "[decoder:c]\nmodule = fake\nchannels = 20m:14074000\nmodule.behaviour = good\n";
    const auto waiting = radio.apply_decoders(config_with(a2 + b + "public = yes\n" + band20 + c));
    CHECK_EQ_STR(joined(waiting.changed), "");
    CHECK_EQ(waiting.waiting.count("c"), 1u);
    CHECK(waiting.waiting.at("c").find("20m") != std::string::npos);
    CHECK(radio.decoder_listed("b"));
    CHECK_EQ(radio.decoders_status().size(), 2u);
    // Moved onto that band, a running decoder keeps its present channels,
    // and being made private still takes effect at once.
    const std::string b_moved = "[decoder:b]\nmodule = fake\nchannels = 20m:14074000\nmodule.behaviour = good\npublic = no\n";
    const auto moved = radio.apply_decoders(config_with(a2 + b_moved + band20));
    CHECK_EQ(moved.waiting.count("b"), 1u);
    CHECK(!radio.decoder_listed("b"));
    CHECK_EQ(radio.decoders_status().size(), 2u);
    CHECK_EQ_STR(joined(radio.apply_decoders(config_with("")).changed), "b a ");
    CHECK_EQ(radio.decoders_status().size(), 0u);

    CHECK(!radio.restart_decoder("a"));
    // Saved over and over, the threads that stop the old decoders do not
    // pile up: those finished are joined as the next one starts.
    for (int i = 0; i < 6; i++) {
        radio.apply_decoders(config_with(i % 2 ? a : a2));
        std::this_thread::sleep_for(300ms);
    }
    CHECK(radio.decoder_stoppers() <= 2u);
    radio.stop();
}

TEST_CASE(a_reporting_decoder_sends_its_spots_to_psk_reporter_under_the_station) {
    // PSK Reporter played by a UDP socket on loopback.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    socklen_t size = sizeof address;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    const int port = ntohs(address.sin_port);
    timeval wait{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);

    Scratch scratch;
    const auto radio_with = [&](const std::string& site, const std::string& report) {
        auto radio = std::make_unique<Radio>();
        Config config;
        std::string error;
        CHECK(config.parse("[site]\n" + site + "spot_server = 127.0.0.1:" + std::to_string(port) + "\n" +
                               receiver_config(scratch.path + "/modules",
                                               "[decoder:ft8]\nmodule = fake\nchannels = 40m:7074000\n" + report),
                           error));
        CHECK(radio->configure(config, error));
        return radio;
    };
    Decode heard;
    heard.decoder = "ft8";
    heard.band = "40m";
    heard.mode = "ft8";
    heard.call = "K1ABC";
    heard.grid = "FN42";
    heard.message = "CQ K1ABC FN42";
    heard.quality = "bp";
    heard.dial_hz = 7074000;
    heard.audio_hz = 1200;
    heard.time_ms = 1790604000000;
    uint8_t datagram[1500];

    {
        // Not asked to report: nothing goes out.
        auto radio = radio_with("operator = DL1ABC\ngrid = JO62qm\n", "");
        radio->decodes().add(heard, 1000);
        radio->spot_reporter().flush();
        CHECK(::recv(fd, datagram, sizeof datagram, MSG_DONTWAIT) < 0);
        CHECK(!radio->spots_status()["enabled"].boolean());
    }
    {
        // Asked, but the station has no locator: nothing, and it says why.
        auto radio = radio_with("operator = DL1ABC\n", "report = pskreporter\n");
        radio->decodes().add(heard, 1000);
        radio->spot_reporter().flush();
        CHECK(::recv(fd, datagram, sizeof datagram, MSG_DONTWAIT) < 0);
        CHECK(radio->spots_status()["problem"].string().find("locator") != std::string::npos);
    }
    auto radio = radio_with("operator = dl1abc\ngrid = JO62qm\nantenna = 80 m loop\n", "report = pskreporter\n");
    radio->decodes().add(heard, 1000);
    radio->spot_reporter().flush();
    const ssize_t n = ::recv(fd, datagram, sizeof datagram, 0);
    CHECK(n > 16);
    const std::string text(reinterpret_cast<const char*>(datagram), n > 0 ? static_cast<size_t>(n) : 0);
    CHECK(text.find("DL1ABC") != std::string::npos);
    CHECK(text.find("JO62qm") != std::string::npos);
    CHECK(text.find("80 m loop") != std::string::npos);
    CHECK(text.find("K1ABC") != std::string::npos);
    CHECK(text.find("FernSDR ") != std::string::npos);
    CHECK_EQ(radio->spots_status()["sent"].number(), 1.0);
    ::close(fd);
}

TEST_CASE(reported_spots_name_the_receiver_and_the_decoder_module_that_runs) {
    if (sandbox_unavailable_under_qemu()) return;
    Scratch scratch;
    const std::string modules = scratch.path + "/modules";
    CHECK(install_fake_decoder(modules));
    Config config;
    std::string error;
    CHECK(config.parse("[site]\noperator = DL1ABC\ngrid = JO62qm\nspot_server = 127.0.0.1:9\n" +
                           receiver_config(modules, "[decoder:ft8]\nmodule = fake\nchannels = 40m:7069000\n"
                                                    "module.behaviour = good\nreport = pskreporter\n"),
                       error));
    Radio radio;
    radio.set_decoder_launcher(beside_tests("fernsdr"));
    CHECK(radio.configure(config, error));
    CHECK(radio.start(error));
    bool running = false;
    for (int i = 0; i < 400 && !running; i++) {
        running = radio.decoders_status()[0]["version"].string() == "1.2.3";
        std::this_thread::sleep_for(20ms);
    }
    CHECK(running);
    // The fake package's manifest calls it "Fake".
    const std::string software = radio.spot_software();
    CHECK(software.rfind("FernSDR ", 0) == 0);
    CHECK(software.find(" / Fake 1.2.3") != std::string::npos);
    radio.stop();
}

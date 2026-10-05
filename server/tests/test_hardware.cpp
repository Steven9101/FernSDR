#include "../src/core/hardware.h"
#include "../src/util/config.h"
#include "test_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

struct FakeSysfs {
    std::string root;
    FakeSysfs() {
        char pattern[] = "/tmp/fernsdr-usb-XXXXXX";
        root = ::mkdtemp(pattern);
    }
    ~FakeSysfs() {
        if (std::system(("rm -rf '" + root + "'").c_str()) != 0) std::fprintf(stderr, "could not remove %s\n", root.c_str());
    }
    void device(const std::string& name, const std::string& vendor, const std::string& product,
                const std::string& serial = "") {
        const std::string path = root + "/" + name;
        ::mkdir(path.c_str(), 0755);
        std::ofstream(path + "/idVendor") << vendor << "\n";
        std::ofstream(path + "/idProduct") << product << "\n";
        if (!serial.empty()) std::ofstream(path + "/serial") << serial << "\n";
    }
};

}  // namespace

TEST_CASE(hardware_names_the_radios_on_usb_and_their_modules) {
    FakeSysfs sysfs;
    sysfs.device("1-1.2", "0bda", "2838", "00000001");
    sysfs.device("1-1.3", "04b4", "00f3");
    sysfs.device("2-1", "1df7", "3000", "2305078C35");
    sysfs.device("2-2", "1df7", "3060");
    sysfs.device("1-1.4", "1d50", "6089");
    sysfs.device("1-1.5", "03eb", "800c");
    // Not radios: a keyboard, an interface of a radio, a root hub.
    sysfs.device("1-1.1", "046d", "c31c");
    sysfs.device("1-1.2:1.0", "0bda", "2838");
    sysfs.device("usb1", "1d6b", "0002");
    const auto radios = fernsdr::find_usb_radios(sysfs.root);
    CHECK_EQ(radios.size(), 6u);
    if (radios.size() != 6) return;
    CHECK(radios[0].port == "1-1.2" && radios[0].module == "rtlsdr" && radios[0].serial == "00000001");
    CHECK(radios[1].port == "1-1.3" && radios[1].module == "rx888" && radios[1].name.find("firmware") != std::string::npos);
    // Known, but with no module to run it.
    CHECK(radios[2].port == "1-1.4" && radios[2].module.empty() && radios[2].name == "HackRF One");
    CHECK(radios[3].port == "1-1.5" && radios[3].module == "airspy" && radios[3].name == "Airspy HF+");
    CHECK(radios[4].name == "SDRplay RSP1A" && radios[4].module == "sdrplay");
    // A newer RSP no public list names yet is still an SDRplay.
    CHECK(radios[5].name == "SDRplay RSP" && radios[5].module == "sdrplay");
    const fernsdr::Json json = fernsdr::usb_radios_json(radios);
    CHECK(json[0]["usb"].string() == "0bda:2838");
    CHECK(json[1].has("serial") == false);
}

TEST_CASE(hardware_finds_nothing_where_there_is_no_usb) {
    CHECK(fernsdr::find_usb_radios("/nonexistent-fernsdr-sysfs").empty());
}

TEST_CASE(hardware_names_the_tv_drivers_that_hold_a_radio) {
    FakeSysfs modules;
    ::mkdir((modules.root + "/msi2500").c_str(), 0755);
    ::mkdir((modules.root + "/snd_usb_audio").c_str(), 0755);
    const auto found = fernsdr::drivers_in_the_way(modules.root);
    CHECK_EQ(found.size(), 1u);
    if (!found.empty()) CHECK(found[0].driver == "msi2500" && found[0].module == "sdrplay");
}

TEST_CASE(hardware_every_recognised_radio_is_opened_by_the_receiver_after_install) {
    // Recognised but left to root, a stick would be found by the setup flow
    // and then refused to its module: the installers' device rules give
    // every id the receiver knows to the receiver's group.
    std::string install, source;
    CHECK(fernsdr::read_text_file("../tools/install.sh", install));
    CHECK(fernsdr::read_text_file("../tools/source-install.sh", source));
    for (const char* module : {"rtlsdr", "rx888", "airspy"}) {
        for (const auto& [vendor, product] : fernsdr::usb_ids_of(module)) {
            char udev[96], mdev[40];
            std::snprintf(udev, sizeof udev, "ATTRS{idVendor}==\"%04x\", ATTRS{idProduct}==\"%04x\"", vendor, product);
            std::snprintf(mdev, sizeof mdev, "PRODUCT=%x/%x/", vendor, product);
            const bool in_mdev = install.find(mdev) != std::string::npos ||
                                 (vendor == 0x0bda && install.find("PRODUCT=bda/283[82]/") != std::string::npos) ||
                                 (vendor == 0x04b4 && install.find("PRODUCT=4b4/f[13]/") != std::string::npos);
            if (install.find(udev) == std::string::npos || source.find(udev) == std::string::npos || !in_mdev) {
                std::fprintf(stderr, "  %04x:%04x of %s has no device rule\n", vendor, product, module);
                CHECK(false);
            }
        }
    }
}

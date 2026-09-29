#include "hardware.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace fernsdr {

namespace {

struct KnownRadio {
    uint16_t vendor;
    uint16_t product;  // 0 for every product of the vendor
    const char* name;
    const char* module;
};

// Radios by their USB identity. The RTL2832U list is the common part of
// librtlsdr's own: the stick sold for SDR and the DVB-T sticks people buy
// for it. An RX-888 shows as a Cypress FX3, 00f3 before its firmware is
// loaded and 00f1 after. SDRplay uses its own vendor number for every RSP.
// Radios with no module yet are named, so the panel can say so plainly
// rather than find nothing.
constexpr KnownRadio kRadios[] = {
    {0x0bda, 0x2838, "RTL-SDR (RTL2832U)", "rtlsdr"},
    {0x0bda, 0x2832, "RTL-SDR (RTL2832U)", "rtlsdr"},
    {0x0ccd, 0x00a9, "Terratec Cinergy T Stick (RTL2832U)", "rtlsdr"},
    {0x0ccd, 0x00b3, "Terratec NOXON DAB stick (RTL2832U)", "rtlsdr"},
    {0x0ccd, 0x00d3, "Terratec Cinergy T Stick RC (RTL2832U)", "rtlsdr"},
    {0x0ccd, 0x00e0, "Terratec NOXON DAB stick (RTL2832U)", "rtlsdr"},
    {0x185b, 0x0620, "Compro Videomate U620F (RTL2832U)", "rtlsdr"},
    {0x185b, 0x0650, "Compro Videomate U650F (RTL2832U)", "rtlsdr"},
    {0x1b80, 0xd393, "GIGABYTE GT-U7300 (RTL2832U)", "rtlsdr"},
    {0x1b80, 0xd394, "DIKOM USB-DVBT HD (RTL2832U)", "rtlsdr"},
    {0x1b80, 0xd395, "Peak 102569AGPK (RTL2832U)", "rtlsdr"},
    {0x1b80, 0xd39d, "SVEON STV20 (RTL2832U)", "rtlsdr"},
    {0x1d19, 0x1101, "Dexatek DVB-T dongle (RTL2832U)", "rtlsdr"},
    {0x1d19, 0x1102, "Dexatek DVB-T dongle (RTL2832U)", "rtlsdr"},
    {0x1d19, 0x1103, "Dexatek DK 5217 (RTL2832U)", "rtlsdr"},
    {0x1d19, 0x1104, "MSI DigiVox Micro HD (RTL2832U)", "rtlsdr"},
    {0x1f4d, 0xb803, "GTek T803 (RTL2832U)", "rtlsdr"},
    {0x1f4d, 0xc803, "Lifeview LV5TDeluxe (RTL2832U)", "rtlsdr"},
    {0x1f4d, 0xd286, "MyGica TD312 (RTL2832U)", "rtlsdr"},
    {0x1f4d, 0xd803, "PROlectrix DV107669 (RTL2832U)", "rtlsdr"},
    {0x04b4, 0x00f3, "RX-888 (firmware not loaded yet)", "rx888"},
    {0x04b4, 0x00f1, "RX-888", "rx888"},
    {0x1df7, 0x2500, "SDRplay RSP1", "sdrplay"},
    {0x1df7, 0x3000, "SDRplay RSP1A", "sdrplay"},
    {0x1df7, 0x3010, "SDRplay RSP2", "sdrplay"},
    {0x1df7, 0x3020, "SDRplay RSPduo", "sdrplay"},
    {0x1df7, 0x3030, "SDRplay RSPdx", "sdrplay"},
    // The newer RSPs (3050, 3060) are in no public list by name; the
    // module says which model it is once installed.
    {0x1df7, 0, "SDRplay RSP", "sdrplay"},
    {0x1d50, 0x60a1, "Airspy", ""},
    {0x03eb, 0x800c, "Airspy HF+", ""},
    {0x1d50, 0x6089, "HackRF One", ""},
    {0x1d50, 0xcc15, "HackRF One (rad1o)", ""},
    {0x04d8, 0xfb56, "FUNcube Dongle Pro", ""},
    {0x04d8, 0xfb31, "FUNcube Dongle Pro+", ""},
    {0x1d50, 0x6108, "LimeSDR", ""},
    {0x0403, 0x601f, "LimeSDR Mini", ""},
    {0x0456, 0xb673, "ADALM-Pluto", ""},
};

// The first line of a sysfs attribute, without its newline; empty when it
// cannot be read.
std::string attribute(const std::string& directory, const char* name) {
    std::ifstream in(directory + "/" + name);
    std::string line;
    if (!std::getline(in, line)) return "";
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

bool hex16(const std::string& text, uint16_t& out) {
    if (text.empty() || text.size() > 4) return false;
    unsigned value = 0;
    for (const char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        value = value * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                                                                                                : std::tolower(c) - 'a' + 10);
    }
    out = static_cast<uint16_t>(value);
    return true;
}

// Printable ASCII only, and not too long: the string comes from the device.
std::string plain(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c >= 0x20 && c < 0x7f) out += c;
        if (out.size() >= 64) break;
    }
    return out;
}

}  // namespace

std::vector<UsbRadio> find_usb_radios(const std::string& sysfs) {
    std::vector<UsbRadio> radios;
    DIR* directory = ::opendir(sysfs.c_str());
    if (!directory) return radios;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        // Devices are "1-1.2"; interfaces ("1-1.2:1.0") and root hubs
        // ("usb1") are not.
        if (name.empty() || name[0] == '.' || name.find(':') != std::string::npos || name.rfind("usb", 0) == 0) continue;
        const std::string path = sysfs + "/" + name;
        UsbRadio radio;
        if (!hex16(attribute(path, "idVendor"), radio.vendor) || !hex16(attribute(path, "idProduct"), radio.product)) continue;
        const KnownRadio* known = nullptr;
        for (const KnownRadio& candidate : kRadios) {
            if (candidate.vendor == radio.vendor && (candidate.product == radio.product || candidate.product == 0)) {
                known = &candidate;
                break;
            }
        }
        if (!known) continue;
        radio.port = plain(name);
        radio.name = known->name;
        radio.module = known->module;
        radio.serial = plain(attribute(path, "serial"));
        radios.push_back(std::move(radio));
    }
    ::closedir(directory);
    std::sort(radios.begin(), radios.end(), [](const UsbRadio& a, const UsbRadio& b) { return a.port < b.port; });
    return radios;
}

std::vector<DriverInTheWay> drivers_in_the_way(const std::string& modules) {
    static const DriverInTheWay kDrivers[] = {
        {"dvb_usb_rtl28xxu", "RTL-SDR dongles", "rtlsdr"},
        {"msi2500", "the SDRplay RSP1", "sdrplay"},
    };
    std::vector<DriverInTheWay> found;
    for (const DriverInTheWay& driver : kDrivers) {
        struct stat info {};
        if (::stat((modules + "/" + driver.driver).c_str(), &info) == 0 && S_ISDIR(info.st_mode)) found.push_back(driver);
    }
    return found;
}

Json usb_radios_json(const std::vector<UsbRadio>& radios) {
    Json list = Json::make_array();
    for (const UsbRadio& radio : radios) {
        Json entry = Json::make_object();
        char usb[10];
        std::snprintf(usb, sizeof usb, "%04x:%04x", radio.vendor, radio.product);
        entry.set("usb", usb);
        entry.set("port", radio.port);
        entry.set("name", radio.name);
        entry.set("module", radio.module);
        if (!radio.serial.empty()) entry.set("serial", radio.serial);
        list.push_back(entry);
    }
    return list;
}

}  // namespace fernsdr

// The radios plugged into this machine, seen on USB before any module for
// them is installed.
//
// Setting up a receiver starts with the question the operator can answer
// least: which program drives the thing in their hand. The USB vendor and
// product say it for the radios people have, so the admin panel can name the
// radio, say which module it needs, and offer to install it. A module, once
// installed, lists its devices itself with far more detail (serials, tuners,
// whether the device can be opened); this is only the first look.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../util/json.h"

namespace fernsdr {

struct UsbRadio {
    uint16_t vendor = 0;
    uint16_t product = 0;
    std::string port;       // the sysfs name, "1-1.2", which says where it is plugged in
    std::string name;       // what the radio is, "RTL-SDR (RTL2832U)"
    std::string module;     // the module that drives it, empty when there is none yet
    std::string serial;     // as the device reports it, possibly empty
};

// The radios among the USB devices under `sysfs` (normally
// /sys/bus/usb/devices), in the order of their ports. Devices that are not
// radios are left out; an unreadable directory gives an empty list.
std::vector<UsbRadio> find_usb_radios(const std::string& sysfs = "/sys/bus/usb/devices");

Json usb_radios_json(const std::vector<UsbRadio>& radios);

// A kernel driver that takes a radio for itself, with the radio it takes.
struct DriverInTheWay {
    std::string driver;  // "dvb_usb_rtl28xxu"
    std::string radio;   // "RTL-SDR dongles"
    std::string module;  // the module that cannot open the radio while it is loaded
};

// The TV drivers that claim SDRs as tuners and are loaded now, as seen under
// `modules` (normally /sys/module). install.sh keeps them from loading; one
// still here means the machine has not restarted since, or the radio was
// plugged in before.
std::vector<DriverInTheWay> drivers_in_the_way(const std::string& modules = "/sys/module");

}  // namespace fernsdr

// Whether the receiver starts with the computer, and switching it, for the
// admin panel's switch. Reading works as any user, from the files each init
// keeps; switching is root's, done by the updater with the init's own tool
// when the panel asks through the update request (see updater.h).
#pragma once
#include <string>

namespace fernsdr {

struct AutostartState {
    // systemd, openrc, runit, sysv; "container" where the container's
    // runtime decides; "other" for an init the panel cannot switch, such as
    // a line in Slackware's rc.local or a boot script the operator wrote.
    std::string init;
    bool enabled = false;
    bool changeable = false;
    // For an init the panel cannot switch: who decides, in a sentence.
    std::string note;
};

// How this machine starts the receiver at boot, as install.sh set it up.
// `root` is "/" outside tests; `container` is whether FERNSDR_CONTAINER is
// set. It looks for the files install.sh leaves for each init, in the order
// install.sh picks the init in, rather than for the init's programs, which
// the receiver's sandbox may not let it run.
AutostartState read_autostart(const std::string& root, bool container);
AutostartState read_autostart();

// The two requests the panel may leave for the updater, each followed by a
// newline and nothing else.
constexpr const char* kAutostartOn = "autostart on";
constexpr const char* kAutostartOff = "autostart off";

// As root: makes the receiver start with the computer, or not, without
// starting or stopping it now. False, with the reason, when the init's tool
// refused, the state did not change, or this init cannot be switched.
bool set_autostart(bool on, std::string& error);

}  // namespace fernsdr

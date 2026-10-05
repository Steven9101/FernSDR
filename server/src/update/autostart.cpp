#include "autostart.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <vector>

#include "../util/subprocess.h"
#include "files.h"

namespace fernsdr {

namespace {

// `path` below `root`; root is "/" on a real machine.
std::string at(const std::string& root, const std::string& path) {
    if (root.empty() || root == "/") return path;
    return (root.back() == '/' ? root.substr(0, root.size() - 1) : root) + path;
}

bool is_directory(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool is_file(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// The name itself, a link or not, whether or not a link leads anywhere.
bool present(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

std::string head_of(const std::string& path, size_t limit) {
    std::string out;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return out;
    char buffer[4096];
    while (out.size() < limit) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        out.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);
    if (out.size() > limit) out.resize(limit);
    return out;
}

// The init's tools live in the system's own directories; nothing on PATH,
// which a root process should not trust, is looked at.
std::string system_program(const std::string& root, const char* name) {
    for (const char* directory : {"/usr/sbin/", "/sbin/", "/usr/bin/", "/bin/"}) {
        const std::string candidate = at(root, std::string(directory) + name);
        struct stat info {};
        if (::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 0111)) return candidate;
    }
    return "";
}

// A link named S<two digits>fernsdr in one of the runlevels a machine boots
// into.
bool sysv_starts(const std::string& root) {
    for (const char* base : {"/etc/rc", "/etc/rc.d/rc"}) {
        for (char level = '2'; level <= '5'; level++) {
            DIR* listing = ::opendir(at(root, std::string(base) + level + ".d").c_str());
            if (!listing) continue;
            bool found = false;
            while (const dirent* entry = ::readdir(listing)) {
                const std::string name = entry->d_name;
                if (name.size() == 10 && name[0] == 'S' && name[1] >= '0' && name[1] <= '9' && name[2] >= '0' &&
                    name[2] <= '9' && name.compare(3, 7, "fernsdr") == 0) {
                    found = true;
                    break;
                }
            }
            ::closedir(listing);
            if (found) return true;
        }
    }
    return false;
}

// Where runit keeps the services it runs and where install.sh put the
// definition, as install.sh chose them.
std::string runit_service_directory(const std::string& root) {
    for (const char* directory : {"/var/service", "/run/runit/service", "/etc/service"}) {
        if (is_directory(at(root, directory))) return directory;
    }
    return "";
}

std::string runit_definition(const std::string& root) {
    return is_directory(at(root, "/etc/sv")) || !is_directory(at(root, "/etc/runit/sv")) ? "/etc/sv/fernsdr"
                                                                                         : "/etc/runit/sv/fernsdr";
}

bool runs_runit(const std::string& root) {
    std::string comm = head_of(at(root, "/proc/1/comm"), 64);
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == ' ')) comm.pop_back();
    return comm == "runit" || comm == "runit-init";
}

std::string first_line(const std::string& text) {
    const size_t end = text.find('\n');
    std::string line = text.substr(0, end == std::string::npos ? text.size() : end);
    if (line.size() > 200) line.resize(200);
    return line;
}

}  // namespace

AutostartState read_autostart(const std::string& root, bool container) {
    AutostartState state;
    if (container) {
        state.init = "container";
        state.note = "Docker decides: a container started with a restart policy, such as --restart unless-stopped "
                     "as the installer sets, starts again with the computer.";
        return state;
    }
    if (is_directory(at(root, "/run/systemd/system"))) {
        if (is_file(at(root, "/etc/systemd/system/fernsdr.service"))) {
            state.init = "systemd";
            state.enabled = present(at(root, "/etc/systemd/system/multi-user.target.wants/fernsdr.service"));
            state.changeable = true;
            return state;
        }
        // systemd runs, but this receiver is not its fernsdr.service.
        state.init = "other";
        state.note = "This receiver is not the fernsdr.service install.sh sets up: whatever started it decides "
                     "whether it starts with the computer.";
        return state;
    }
    const std::string script = at(root, "/etc/init.d/fernsdr");
    const bool has_script = is_file(script);
    if (has_script && first_line(head_of(script, 256)).find("openrc-run") != std::string::npos) {
        state.init = "openrc";
        state.enabled = present(at(root, "/etc/runlevels/default/fernsdr"));
        state.changeable = !system_program(root, "rc-update").empty();
        if (!state.changeable) {
            state.note = "OpenRC's rc-update is not installed: add fernsdr to the default runlevel, or remove it, "
                         "by hand.";
        }
        return state;
    }
    if (runs_runit(root)) {
        state.init = "runit";
        const std::string services = runit_service_directory(root);
        const std::string definition = runit_definition(root);
        const bool linked = !services.empty() && present(at(root, services + "/fernsdr"));
        state.enabled = linked && !present(at(root, definition + "/down"));
        state.changeable = linked && is_file(at(root, definition + "/run"));
        if (!state.changeable) {
            state.note = "runit does not run " + definition + ": link it into the service directory to have it "
                         "start with the computer.";
        }
        return state;
    }
    if (is_file(at(root, "/etc/slackware-version")) && is_file(at(root, "/etc/rc.d/rc.fernsdr"))) {
        state.init = "other";
        state.enabled = head_of(at(root, "/etc/rc.d/rc.local"), 1 << 20).find("rc.fernsdr start") != std::string::npos;
        state.note = "Slackware starts it from a line in /etc/rc.d/rc.local: remove that line to have it not start "
                     "with the computer, or add it back.";
        return state;
    }
    if (has_script) {
        state.init = "sysv";
        state.enabled = sysv_starts(root);
        state.changeable = !system_program(root, "update-rc.d").empty() || !system_program(root, "chkconfig").empty() ||
                           !system_program(root, "insserv").empty();
        if (!state.changeable) state.note = "None of update-rc.d, chkconfig and insserv is here to switch it with.";
        return state;
    }
    state.init = "other";
    state.note = is_file(at(root, "/usr/local/sbin/fernsdr-service"))
                     ? "It starts with the computer if a line running /usr/local/sbin/fernsdr-service start is in "
                       "what this machine runs at boot: add or remove that line there."
                     : "This receiver was not set up as a service by install.sh: whatever started it decides "
                       "whether it starts with the computer.";
    return state;
}

AutostartState read_autostart() {
    const char* container = std::getenv("FERNSDR_CONTAINER");
    return read_autostart("/", container && *container);
}

namespace {

// Runs an init's tool with a fixed argument list; false with what it said.
bool run_tool(const std::string& program, const std::vector<std::string>& arguments, std::string& error) {
    std::string output, errors;
    Subprocess::Exit status;
    if (!Subprocess::run(program, arguments, {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8"},
                         std::chrono::seconds(60), 64 * 1024, output, errors, status, error)) {
        return false;
    }
    if (status.exited && status.code == 0) return true;
    const std::string said = first_line(errors.empty() ? output : errors);
    const size_t slash = program.rfind('/');
    error = program.substr(slash == std::string::npos ? 0 : slash + 1) + " " + status.describe() +
            (said.empty() ? "" : " (" + said + ")");
    return false;
}

bool switch_runit(bool on, std::string& error) {
    const std::string definition = runit_definition("/");
    const int dir = open_directory(definition);
    if (dir < 0) {
        error = "cannot open " + definition;
        return false;
    }
    // runsv looks for `down` when it starts the service, at boot; the
    // receiver running now is left as it is.
    const bool done = on ? remove_file_at(dir, "down", error)
                         : write_file_at(dir, "down", "", 0644, static_cast<uid_t>(-1), static_cast<gid_t>(-1), error);
    ::close(dir);
    return done;
}

}  // namespace

bool set_autostart(bool on, std::string& error) {
    const AutostartState before = read_autostart();
    if (!before.changeable) {
        error = before.note.empty() ? "this machine's init cannot be switched from the admin panel" : before.note;
        return false;
    }
    if (before.enabled == on) return true;
    bool done = false;
    if (before.init == "systemd") {
        // Only the receiver's own unit, and without --now: the receiver that
        // asked keeps running, and the updater's units stay as they are, so
        // that a receiver started by hand can still be updated.
        const std::string systemctl = system_program("/", "systemctl");
        if (systemctl.empty()) {
            error = "systemctl is not installed";
            return false;
        }
        done = run_tool(systemctl, {on ? "enable" : "disable", "fernsdr.service"}, error);
    } else if (before.init == "openrc") {
        done = run_tool(system_program("/", "rc-update"), {on ? "add" : "del", "fernsdr", "default"}, error);
    } else if (before.init == "runit") {
        done = switch_runit(on, error);
    } else if (before.init == "sysv") {
        if (const std::string tool = system_program("/", "update-rc.d"); !tool.empty()) {
            done = run_tool(tool, {"fernsdr", on ? "enable" : "disable"}, error);
        } else if (const std::string tool = system_program("/", "chkconfig"); !tool.empty()) {
            done = run_tool(tool, {"fernsdr", on ? "on" : "off"}, error);
        } else {
            const std::string insserv = system_program("/", "insserv");
            done = on ? run_tool(insserv, {"fernsdr"}, error) : run_tool(insserv, {"-r", "fernsdr"}, error);
        }
    } else {
        error = "this machine's init cannot be switched from the admin panel";
        return false;
    }
    if (!done) return false;
    // The tool's word is not taken for it: what the panel shows is read
    // from the same files, so they have to say so.
    if (read_autostart().enabled != on) {
        error = std::string("the init's tool ran, but FernSDR still ") +
                (on ? "does not start with the computer" : "starts with the computer");
        return false;
    }
    return true;
}

}  // namespace fernsdr

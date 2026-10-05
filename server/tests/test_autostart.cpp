// Whether the receiver starts with the computer, read from a directory that
// stands in for / with the files each init keeps, as install.sh leaves them.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "../src/update/autostart.h"
#include "../src/update/files.h"
#include "test_util.h"

namespace {

struct Tree {
    std::string root;

    Tree() {
        char name[] = "/tmp/fernsdr-autostart-XXXXXX";
        root = ::mkdtemp(name) ? name : "";
    }
    ~Tree() {
        const int top = fernsdr::open_directory("/tmp");
        std::string error;
        if (top >= 0) fernsdr::remove_tree_at(top, root.substr(5), error);
        if (top >= 0) ::close(top);
    }

    void dir(const std::string& path) {
        std::string at = root;
        size_t from = 1;
        while (from <= path.size()) {
            const size_t slash = path.find('/', from);
            at = root + path.substr(0, slash);
            ::mkdir(at.c_str(), 0755);
            if (slash == std::string::npos) break;
            from = slash + 1;
        }
    }
    void file(const std::string& path, const std::string& text = "", mode_t mode = 0644) {
        dir(path.substr(0, path.rfind('/')));
        const int fd = ::open((root + path).c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
        if (fd < 0) return;
        if (::write(fd, text.data(), text.size()) < 0) {}
        ::close(fd);
        ::chmod((root + path).c_str(), mode);
    }
    void link(const std::string& target, const std::string& path) {
        dir(path.substr(0, path.rfind('/')));
        CHECK(::symlink(target.c_str(), (root + path).c_str()) == 0);
    }
    void remove(const std::string& path) { ::unlink((root + path).c_str()); }

    fernsdr::AutostartState read(bool container = false) const { return fernsdr::read_autostart(root, container); }
};

}  // namespace

TEST_CASE(autostart_in_a_container_is_dockers) {
    Tree tree;
    // Whatever else is there: in a container the runtime decides.
    tree.dir("/run/systemd/system");
    tree.file("/etc/systemd/system/fernsdr.service");
    const fernsdr::AutostartState state = tree.read(true);
    CHECK_EQ_STR(state.init, "container");
    CHECK(!state.changeable);
    CHECK(state.note.find("restart policy") != std::string::npos);
}

TEST_CASE(autostart_reads_systemds_wants_link) {
    Tree tree;
    tree.dir("/run/systemd/system");
    tree.file("/etc/systemd/system/fernsdr.service");
    fernsdr::AutostartState state = tree.read();
    CHECK_EQ_STR(state.init, "systemd");
    CHECK(state.changeable);
    CHECK(!state.enabled);
    // A link to the unit, as systemctl enable makes it.
    tree.link("/etc/systemd/system/fernsdr.service", "/etc/systemd/system/multi-user.target.wants/fernsdr.service");
    CHECK(tree.read().enabled);
    // systemd without FernSDR's unit: a receiver started some other way.
    tree.remove("/etc/systemd/system/fernsdr.service");
    state = tree.read();
    CHECK_EQ_STR(state.init, "other");
    CHECK(!state.changeable);
    CHECK(!state.note.empty());
}

TEST_CASE(autostart_reads_openrcs_default_runlevel) {
    Tree tree;
    tree.file("/etc/init.d/fernsdr", "#!/sbin/openrc-run\n# FernSDR under OpenRC\n", 0755);
    tree.file("/sbin/rc-update", "#!/bin/sh\n", 0755);
    fernsdr::AutostartState state = tree.read();
    CHECK_EQ_STR(state.init, "openrc");
    CHECK(state.changeable);
    CHECK(!state.enabled);
    tree.link("/etc/init.d/fernsdr", "/etc/runlevels/default/fernsdr");
    CHECK(tree.read().enabled);
    // Without rc-update there is nothing to switch it with.
    tree.remove("/sbin/rc-update");
    state = tree.read();
    CHECK(!state.changeable);
    CHECK(!state.note.empty());
}

TEST_CASE(autostart_reads_runits_link_and_down_file) {
    Tree tree;
    tree.file("/proc/1/comm", "runit\n");
    tree.dir("/var/service");
    tree.file("/etc/sv/fernsdr/run", "#!/bin/sh\n", 0755);
    // Not linked: runit does not run it at all, and the panel does not link it.
    fernsdr::AutostartState state = tree.read();
    CHECK_EQ_STR(state.init, "runit");
    CHECK(!state.enabled);
    CHECK(!state.changeable);
    CHECK(state.note.find("/etc/sv/fernsdr") != std::string::npos);
    tree.link("/etc/sv/fernsdr", "/var/service/fernsdr");
    state = tree.read();
    CHECK(state.enabled);
    CHECK(state.changeable);
    tree.file("/etc/sv/fernsdr/down");
    CHECK(!tree.read().enabled);
}

TEST_CASE(autostart_finds_runits_definition_where_install_sh_puts_it) {
    // Artix: /etc/runit/sv, and the service directory under /run.
    Tree tree;
    tree.file("/proc/1/comm", "runit-init\n");
    tree.dir("/run/runit/service");
    tree.file("/etc/runit/sv/fernsdr/run", "#!/bin/sh\n", 0755);
    tree.link("/etc/runit/sv/fernsdr", "/run/runit/service/fernsdr");
    fernsdr::AutostartState state = tree.read();
    CHECK_EQ_STR(state.init, "runit");
    CHECK(state.enabled && state.changeable);
    tree.file("/etc/runit/sv/fernsdr/down");
    CHECK(!tree.read().enabled);
}

TEST_CASE(autostart_reads_sysv_start_links) {
    Tree tree;
    tree.file("/etc/init.d/fernsdr", "#!/bin/sh\n### BEGIN INIT INFO\n", 0755);
    tree.file("/usr/sbin/update-rc.d", "#!/bin/sh\n", 0755);
    tree.dir("/etc/rc2.d");
    tree.link("../init.d/fernsdr", "/etc/rc0.d/K01fernsdr");
    fernsdr::AutostartState state = tree.read();
    CHECK_EQ_STR(state.init, "sysv");
    CHECK(state.changeable);
    CHECK(!state.enabled);
    // What update-rc.d disable leaves is a K link; only an S link starts it.
    tree.link("../init.d/fernsdr", "/etc/rc2.d/K01fernsdr");
    CHECK(!tree.read().enabled);
    tree.link("../init.d/fernsdr", "/etc/rc3.d/S02fernsdr");
    CHECK(tree.read().enabled);
    // Not a name that is FernSDR's.
    tree.remove("/etc/rc3.d/S02fernsdr");
    tree.link("../init.d/x", "/etc/rc3.d/S02fernsdr-feed");
    CHECK(!tree.read().enabled);
}

TEST_CASE(autostart_reads_sysv_links_in_rc_d_and_needs_a_tool) {
    Tree tree;
    tree.file("/etc/init.d/fernsdr", "#!/bin/sh\n", 0755);
    tree.link("../init.d/fernsdr", "/etc/rc.d/rc5.d/S50fernsdr");
    fernsdr::AutostartState state = tree.read();
    CHECK_EQ_STR(state.init, "sysv");
    CHECK(state.enabled);
    CHECK(!state.changeable);
    tree.file("/sbin/chkconfig", "#!/bin/sh\n", 0755);
    CHECK(tree.read().changeable);
    tree.remove("/sbin/chkconfig");
    // Present but not a program.
    tree.file("/sbin/insserv", "", 0644);
    CHECK(!tree.read().changeable);
    tree.file("/sbin/insserv", "#!/bin/sh\n", 0755);
    CHECK(tree.read().changeable);
}

TEST_CASE(autostart_on_slackware_and_without_an_init_is_left_to_the_operator) {
    Tree slackware;
    slackware.file("/etc/slackware-version", "Slackware 15.0\n");
    slackware.file("/etc/rc.d/rc.fernsdr", "#!/bin/sh\n", 0755);
    slackware.file("/etc/rc.d/rc.local", "#!/bin/sh\n[ -x /etc/rc.d/rc.fernsdr ] && /etc/rc.d/rc.fernsdr start\n", 0755);
    fernsdr::AutostartState state = slackware.read();
    CHECK_EQ_STR(state.init, "other");
    CHECK(state.enabled);
    CHECK(!state.changeable);
    CHECK(state.note.find("rc.local") != std::string::npos);

    Tree none;
    none.file("/usr/local/sbin/fernsdr-service", "#!/bin/sh\n", 0755);
    state = none.read();
    CHECK_EQ_STR(state.init, "other");
    CHECK(!state.changeable);
    CHECK(state.note.find("fernsdr-service start") != std::string::npos);

    Tree nothing;
    state = nothing.read();
    CHECK_EQ_STR(state.init, "other");
    CHECK(!state.changeable);
    CHECK(state.note.find("install.sh") != std::string::npos);
}

TEST_CASE(autostart_picks_the_init_in_install_shs_order) {
    // systemd wins over a SysV script left from before, as in install.sh.
    Tree tree;
    tree.file("/etc/init.d/fernsdr", "#!/bin/sh\n", 0755);
    tree.dir("/run/systemd/system");
    tree.file("/etc/systemd/system/fernsdr.service");
    CHECK_EQ_STR(tree.read().init, "systemd");
}

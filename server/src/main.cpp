// Entry point: read the config, bring up the bands, serve.
#include <signal.h>
#include <sys/prctl.h>
#include <termios.h>
#include <grp.h>
#include <unistd.h>

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "core/admin.h"
#include "core/app.h"
#include "core/module_store.h"
#include "core/radio.h"
#include "net/server.h"
#include "update/supervisor.h"
#include "update/system.h"
#include "update/updater.h"
#include "util/config.h"
#include "util/limits.h"
#include "util/log.h"
#include "util/password.h"
#include "util/sandbox.h"
#include "util/subprocess.h"
#include "version.h"

namespace {

fernsdr::Server* g_server = nullptr;
using fernsdr::split_list;

void handle_signal(int signal_number) {
    (void)signal_number;
    // Nothing starts a module after this; systemd has already sent them the
    // same signal, and restarting one would only be killed again.
    fernsdr::request_shutdown();
    if (g_server) g_server->stop();
}

void print_usage(const char* program) {
    fprintf(stderr,
            "FernSDR - a lightweight WebSDR server\n"
            "\n"
            "usage: %s [options] <fernsdr.conf>\n"
            "\n"
            "options:\n"
            "  -p, --port <n>       override the listening port\n"
            "  -r, --root <dir>     override the web document root\n"
            "  -l, --log <level>    trace, debug, info, warn, error, none\n"
            "  -c, --check          validate the config and exit\n"
            "  --update-run         as root: act on an update the receiver asked for\n"
            "  --update-boot        as root: end an update trial a restart interrupted\n"
            "  --supervise [--daemon] [--pidfile <file>]\n"
            "                       as root, where there is no systemd: run the receiver\n"
            "                       and its updates the way the systemd units do\n"
            "  -V, --version        print the version and exit\n"
            "      --allow-root     serve as root, for a container that has no other user\n"
            "      --set-password   set the admin panel's password in the config\n"
            "      --hash-password  hash an admin password for the config\n"
            "      --install-module <file.fernmod>\n"
            "                       install a module package beside the config and exit\n"
            "  -h, --help           this message\n",
            program);
}

}  // namespace

/**
 * Makes `path` absolute by reading it relative to the directory `config_path`
 * lives in. An absolute path is returned unchanged.
 */
std::string resolve_against_config(const std::string& config_path, const std::string& path) {
    if (path.empty() || path[0] == '/') return path;
    const size_t slash = config_path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return config_path.substr(0, slash + 1) + path;
}

// What the updater's systemd units run: as root, never in the receiver's
// own process. See src/update/updater.h.
int update_command(bool boot) {
    if (::geteuid() != 0) {
        fprintf(stderr, "fernsdr: --update-run and --update-boot run as root, from their systemd units or --supervise\n");
        return 2;
    }
    fernsdr::UpdateEnvironment environment;
    std::string error;
    if (!fernsdr::system_update_environment(environment, error)) {
        fprintf(stderr, "fernsdr: %s\n", error.c_str());
        return 1;
    }
    const fernsdr::UpdateLayout layout = fernsdr::UpdateLayout::standard();
    const fernsdr::UpdateOutcome outcome =
        boot ? fernsdr::run_boot_check(layout, environment) : fernsdr::run_update(layout, environment);
    if (!outcome.message.empty()) printf("%s\n", outcome.message.c_str());
    // A request refused or a version rolled back is the updater doing its
    // job, and status.json says which; only an updater that could not do it
    // leaves its unit failed.
    return outcome.result == fernsdr::UpdateOutcome::Result::Failed ? 1 : 0;
}

// One line from standard input, without echoing it when that is a terminal.
bool read_secret(const char* prompt, std::string& out) {
    const bool terminal = ::isatty(STDIN_FILENO) == 1;
    struct termios before {};
    if (terminal) {
        fprintf(stderr, "%s", prompt);
        ::tcgetattr(STDIN_FILENO, &before);
        struct termios quiet = before;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    const bool got = static_cast<bool>(std::getline(std::cin, out));
    if (terminal) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &before);
        fprintf(stderr, "\n");
    }
    return got;
}

// fernsdr --set-password CONFIG: asks for a password, twice on a terminal,
// and writes its hash where the old one was, or into a new [admin] section,
// so that nobody copies a hash by hand. The file keeps its owner, so that
// the receiver can still read it after root ran this. The receiver reads
// the password when it starts.
int set_password_command(const std::string& path) {
    // Run as root on a configuration in a directory another user owns, as
    // `sudo fernsdr --set-password /var/lib/fernsdr/fernsdr.conf` is, this
    // becomes that user first. Otherwise the receiver, which owns the
    // directory, could swap a link to /etc/sudoers in for the file between
    // its writing and its chown, and have root give that to it. As the
    // directory's owner nothing here can reach further than the receiver
    // already does. A directory root owns is safe to work in as root.
    if (::geteuid() == 0) {
        const size_t slash = path.find_last_of('/');
        const std::string directory = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
        struct stat owner {};
        if (::stat(directory.c_str(), &owner) != 0) {
            fprintf(stderr, "cannot read %s: %s\n", directory.c_str(), std::strerror(errno));
            return 1;
        }
        if (owner.st_uid != 0 &&
            (::setgroups(0, nullptr) != 0 || ::setresgid(owner.st_gid, owner.st_gid, owner.st_gid) != 0 ||
             ::setresuid(owner.st_uid, owner.st_uid, owner.st_uid) != 0 || ::setuid(0) == 0)) {
            fprintf(stderr, "cannot become the owner of %s to write it\n", directory.c_str());
            return 1;
        }
    }
    // A FIFO or a device in its place would hang or mislead the read below.
    struct stat file {};
    if (::stat(path.c_str(), &file) != 0) {
        fprintf(stderr, "cannot read %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    if (!S_ISREG(file.st_mode)) {
        fprintf(stderr, "%s is not a configuration file\n", path.c_str());
        return 1;
    }
    std::string text;
    if (!fernsdr::read_text_file(path, text)) {
        fprintf(stderr, "cannot read %s\n", path.c_str());
        return 1;
    }
    std::string password, again;
    if (!read_secret("New password for the admin panel: ", password) || password.empty()) {
        fprintf(stderr, "no password given\n");
        return 2;
    }
    if (password.size() < 12) {
        fprintf(stderr, "That is %zu characters. The admin panel is reachable by anyone who can reach the\n"
                        "receiver, so use at least 12.\n", password.size());
        return 2;
    }
    if (::isatty(STDIN_FILENO) == 1 && (!read_secret("The same again: ", again) || again != password)) {
        fprintf(stderr, "The two differ; nothing was changed.\n");
        return 2;
    }
    const std::string hash = fernsdr::hash_password(password);
    if (hash.empty()) {
        fprintf(stderr, "cannot read randomness from /dev/urandom\n");
        return 1;
    }
    std::string error;
    fernsdr::Config check;
    const std::string changed = fernsdr::with_admin_password(text, hash);
    if (!check.parse(changed, error) || check.section("admin").get("password_hash") != hash) {
        fprintf(stderr, "the password could not be put into %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    if (!fernsdr::write_text_file(path, changed, error, fernsdr::FileAccess::OwnerOnly)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    // Still root only in a directory root owns, where nobody else can put a
    // link in the file's place; the file keeps the owner it had.
    if (::geteuid() == 0 && ::chown(path.c_str(), file.st_uid, file.st_gid) != 0) {
        fprintf(stderr, "the password is set, but %s could not be given back to its owner: %s\n", path.c_str(),
                std::strerror(errno));
        return 1;
    }
    fprintf(stderr, "The admin password is set in %s. Restart the receiver to use it.\n", path.c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::string config_path;
    int port_override = 0;
    std::string root_override;
    bool check_only = false;
    bool allow_root = false;
    bool set_password = false;
    std::string install_module;

    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", name);
                exit(2);
            }
            return argv[++i];
        };

        if (argument == "-h" || argument == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (argument == "-V" || argument == "--version") {
            printf("FernSDR %s\n", fernsdr::kVersion);
            return 0;
        } else if (argument == "-p" || argument == "--port") {
            port_override = std::stoi(next("--port"));
        } else if (argument == "-r" || argument == "--root") {
            root_override = next("--root");
        } else if (argument == "-l" || argument == "--log") {
            fernsdr::LogLevel level;
            const std::string name = next("--log");
            if (!fernsdr::log_level_from_name(name.c_str(), level)) {
                fprintf(stderr, "unknown log level '%s'\n", name.c_str());
                return 2;
            }
            fernsdr::set_log_level(level);
        } else if (argument == "-c" || argument == "--check") {
            check_only = true;
            // The updater runs this as the receiver's user but outside the
            // receiver's unit: a receiver someone has taken over must not
            // write into it through /proc/PID/mem.
            ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
        } else if (argument == "--allow-root") {
            allow_root = true;
        } else if (argument == "--update-run" || argument == "--update-boot") {
            return update_command(argument == "--update-boot");
        } else if (argument == "--sandbox-exec") {
            // How decoder modules are started; see util/sandbox.h.
            return fernsdr::sandbox_exec_command(argc, argv, i + 1);
        } else if (argument == "--supervise") {
            bool daemon = false;
            std::string pidfile;
            for (int j = i + 1; j < argc; ++j) {
                const std::string option = argv[j];
                if (option == "--daemon") {
                    daemon = true;
                } else if (option == "--pidfile" && j + 1 < argc) {
                    pidfile = argv[++j];
                } else {
                    fprintf(stderr, "--supervise takes --daemon and --pidfile <file>, not %s\n", option.c_str());
                    return 2;
                }
            }
            return fernsdr::supervise_command(daemon, pidfile);
        } else if (argument == "--install-module") {
            install_module = next("--install-module");
        } else if (argument == "--set-password") {
            set_password = true;
        } else if (argument == "--hash-password") {
            // Reads from the terminal rather than taking the password as an
            // argument: an argument would be in the shell history and visible
            // in `ps` to every other user on the machine.
            fprintf(stderr, "Password for the admin panel: ");
            std::string password;
            if (!std::getline(std::cin, password) || password.empty()) {
                fprintf(stderr, "\nno password given\n");
                return 2;
            }
            if (password.size() < 12) {
                fprintf(stderr,
                        "\nThat is %zu characters. The admin panel is reachable by anyone who can\n"
                        "reach the receiver, so use at least 12.\n",
                        password.size());
                return 2;
            }
            const std::string hashed = fernsdr::hash_password(password);
            if (hashed.empty()) {
                fprintf(stderr, "\ncannot read randomness from /dev/urandom\n");
                return 2;
            }
            printf("\n[admin]\npassword_hash = %s\n", hashed.c_str());
            fprintf(stderr, "\nPut those two lines in your config.\n");
            return 0;
        } else if (!argument.empty() && argument[0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argument.c_str());
            print_usage(argv[0]);
            return 2;
        } else {
            config_path = argument;
        }
    }

    if (config_path.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    if (set_password) return set_password_command(config_path);

    fernsdr::Config config;
    std::string error;
    if (!config.load(config_path, error)) {
        fprintf(stderr, "config: %s\n", error.c_str());
        return 1;
    }

    // The password hash signs an administrator in by itself, so the file that
    // holds it belongs to its owner alone, as an SSH key does. Saving from the
    // admin panel writes it that way; a file made by hand may not be.
    struct stat config_file {};
    if (!config.section("admin").get("password_hash", "").empty() &&
        ::stat(config_path.c_str(), &config_file) == 0 && (config_file.st_mode & 077) != 0) {
        LOG_WARN("main", "%s holds the admin password hash and other users can read it; run chmod 600 %s",
                 config_path.c_str(), config_path.c_str());
    }

    if (!install_module.empty()) {
        // For an operator with a shell, and for a receiver without internet.
        // The package is checked exactly as a download would be; only the
        // repository it came from is not known, and is recorded as a file.
        std::string directory = config.section("modules").get("directory", "fernsdr-modules");
        directory = resolve_against_config(config_path, directory);
        // Installed as root into a directory the receiver's own user owns, the
        // files would be root's, and the admin panel could not update or
        // remove them afterwards.
        struct stat owner {};
        const size_t slash = config_path.find_last_of('/');
        const std::string config_directory = slash == std::string::npos ? "." : config_path.substr(0, slash);
        if (::stat(::access(directory.c_str(), F_OK) == 0 ? directory.c_str() : config_directory.c_str(), &owner) == 0 &&
            owner.st_uid != ::geteuid()) {
            fprintf(stderr,
                    "%s belongs to another user; run this as the user the receiver runs as, for example\n"
                    "  sudo -u fernsdr %s --install-module %s %s\n",
                    directory.c_str(), argv[0], install_module.c_str(), config_path.c_str());
            return 1;
        }
        struct stat file {};
        if (::stat(install_module.c_str(), &file) != 0 || !S_ISREG(file.st_mode)) {
            fprintf(stderr, "%s is not a file\n", install_module.c_str());
            return 1;
        }
        if (static_cast<uint64_t>(file.st_size) > fernsdr::kMaxModuleBytes + fernsdr::kMaxManifestBytes + 64) {
            fprintf(stderr, "%s is larger than a module package may be\n", install_module.c_str());
            return 1;
        }
        std::string package;
        if (!fernsdr::read_text_file(install_module, package)) {
            fprintf(stderr, "cannot read %s\n", install_module.c_str());
            return 1;
        }
        fernsdr::ModuleStore store(directory);
        fernsdr::ModuleManifest installed;
        if (!store.install(package, "file", nullptr, installed, error)) {
            fprintf(stderr, "%s: %s\n", install_module.c_str(), error.c_str());
            return 1;
        }
        fernsdr::ModuleStore::Module module;
        store.find(installed.id, module);
        printf("installed %s %s in %s\n", installed.id.c_str(), installed.version.c_str(), directory.c_str());
        if (module.active != installed.version) {
            printf("bands still use %s %s; activate %s in the admin panel under Modules\n", installed.id.c_str(),
                   module.active.c_str(), installed.version.c_str());
        } else {
            printf("a running receiver picks it up when its bands restart: restart them from the admin panel\n");
        }
        return 0;
    }

    const fernsdr::ConfigSection& server_section = config.section("server");
    if (server_section.has("log_level")) {
        fernsdr::LogLevel level;
        if (fernsdr::log_level_from_name(server_section.get("log_level").c_str(), level)) {
            fernsdr::set_log_level(level);
        }
    }

    // Every listener is a socket; see util/limits.h for why this is raised.
    const uint64_t open_files = fernsdr::raise_open_file_limit();
    const long max_connections = server_section.get_int("max_connections", 0);
    if (open_files > 0 && max_connections > 0 && static_cast<uint64_t>(max_connections) + 64 > open_files) {
        LOG_WARN("main", "max_connections is %ld but the process may open only %llu files; raise LimitNOFILE",
                 max_connections, static_cast<unsigned long long>(open_files));
    }

    fernsdr::Radio radio;
    if (!radio.configure(config, error)) {
        fprintf(stderr, "config: %s\n", error.c_str());
        return 1;
    }

    fernsdr::ServerConfig server_config;
    server_config.bind_address = server_section.get("bind", "0.0.0.0");
    server_config.port = static_cast<int>(server_section.get_int("port", 8073));
    // A relative document root is resolved against the configuration file, not
    // against wherever the process happens to have been started. `root` is
    // written once, next to the file it belongs with, and an operator who runs
    // the binary from their home directory should not get a receiver that
    // serves its API and a 404 for the page.
    server_config.document_root =
        resolve_against_config(config_path, server_section.get("root", "../web/dist"));
    // Beside the configuration file, with the settings and the theme, rather
    // than inside the document root: that is build output, and `vite build`
    // empties it. Uploads kept there disappeared on the next client build and
    // the pages that referenced them served 404s.
    server_config.uploads_root = resolve_against_config(config_path,
        server_section.get("uploads", "fernsdr-uploads"));
    // Created up front so it can be served: an unresolvable root leaves static
    // serving switched off for that directory. Not by --check, which root
    // runs on a configuration the receiver may have written: a check makes
    // nothing.
    if (!check_only && !server_config.uploads_root.empty()) {
        ::mkdir(server_config.uploads_root.c_str(), 0755);
    }
    server_config.max_connections = static_cast<int>(server_section.get_int("max_connections", 400));
    server_config.max_connections_per_address =
        std::max(0, static_cast<int>(server_section.get_int("max_connections_per_address", 32)));
    server_config.idle_timeout_seconds =
        static_cast<int>(server_section.get_int("idle_timeout", 120));
    server_config.websocket_path = server_section.get("websocket_path", "/ws");
    // Behind the reverse proxy that DEPLOYMENT.md recommends for HTTPS, every
    // listener arrives from the proxy's own address. Loopback is trusted by
    // default because that is where such a proxy lives; "none" turns it off,
    // and a list of CIDRs covers a proxy on another host.
    for (const std::string& entry : split_list(server_section.get("trusted_proxies", "loopback"))) {
        if (entry != "none") server_config.trusted_proxies.push_back(entry);
    }
    if (!fernsdr::parse_frame_ancestors(server_section.get("frame_ancestors", "self"),
                                        server_config.frame_ancestors, error)) {
        fprintf(stderr, "config: %s\n", error.c_str());
        return 1;
    }
    if (port_override) server_config.port = port_override;
    if (!root_override.empty()) server_config.document_root = root_override;

    // What the receiver keeps for itself must not sit in a directory it
    // serves to anyone: the configuration holds the admin password hash, a
    // private archive is for the operator alone, and modules are programs.
    // The panel cannot point these anywhere, but a file edited by hand can,
    // and so can a default that lands in the wrong place because of where
    // the receiver was started.
    {
        const fernsdr::StaticFiles pages(server_config.document_root);
        const fernsdr::StaticFiles uploads(server_config.uploads_root);
        for (const auto& [what, path] : radio.own_files()) {
            for (const fernsdr::StaticFiles* served : {&pages, &uploads}) {
                if (!served->contains(path)) continue;
                fprintf(stderr, "config: %s, %s, is inside %s, which is served to anyone; move one of them\n",
                        what.c_str(), path.c_str(), served->root().c_str());
                return 1;
            }
        }
    }

    if (check_only) {
        printf("config OK: %zu band(s), listening would be on %s:%d\n", radio.bands().size(),
               server_config.bind_address.c_str(), server_config.port);
        return 0;
    }

    // A receiver faces the internet around the clock, and it starts module
    // programs too. As root, a flaw anywhere in it would own the machine.
    if (::geteuid() == 0 && !allow_root) {
        fprintf(stderr,
                "fernsdr: refusing to serve as root. Run it as a user of its own:\n"
                "  install.sh, published with each release, sets one up with a systemd unit,\n"
                "  and so does tools/source-install.sh --service in the source.\n"
                "In a container with no other user, pass --allow-root.\n");
        return 1;
    }

    fernsdr::Application application(radio, config);
    application.set_server_config(server_config);
    fernsdr::Server server(server_config, application);
    application.set_server(&server);
    g_server = &server;

    if (!server.start(error)) {
        fprintf(stderr, "server: %s\n", error.c_str());
        return 1;
    }

    // Bands push; the server wakes and flushes.  Wiring this before starting
    // the bands means no block goes unflushed.
    radio.set_wake_callback([&server] { server.wake(); });

    if (!radio.start(error)) {
        fprintf(stderr, "radio: %s\n", error.c_str());
        return 1;
    }

    struct sigaction action {};
    action.sa_handler = handle_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    LOG_INFO("main", "%s ready on port %d", radio.site().name.c_str(), server_config.port);
    server.run();

    radio.stop();
    LOG_INFO("main", "stopped");
    return 0;
}

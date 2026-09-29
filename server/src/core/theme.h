// The operator's customisation: colours, background, and widgets.
//
// Kept apart from fernsdr.conf on purpose. The config file is the receiver's
// wiring - sources, sample rates, bands - and it is edited by hand and read at
// startup. This is presentation, it is edited from the admin panel by people
// who should never have to see a config file, and every change takes effect
// immediately for everyone already listening. Mixing the two would mean either
// rewriting the wiring file on every colour tweak, or restarting a receiver
// full of listeners to change a background image.
//
// Stored as JSON next to the config, so it is still a plain file an operator
// can copy between receivers, keep in version control, or delete to get the
// defaults back.
#pragma once
#include <mutex>
#include <string>

#include "../util/json.h"

namespace fernsdr {

class ThemeStore {
public:
    // Loads from `path`, falling back to the built-in defaults when the file
    // does not exist or cannot be parsed. Never fails: a receiver with a
    // corrupt theme file should still come up looking like a receiver.
    void load(const std::string& path);

    /** The current theme, as it is sent to clients. */
    Json snapshot() const;

    /**
     * Replaces the theme and writes it out.
     *
     * Returns false with `error` set when the value is not something the
     * client could apply - which is checked here rather than in the browser,
     * because the browser is not the only thing that reads it.
     */
    bool replace(const Json& theme, std::string& error);

    /** Back to the built-in defaults, and write that out too. */
    bool reset(std::string& error);

    /** Bumped on every change, so a client can tell whether it has the latest. */
    uint64_t version() const;

private:
    bool write(std::string& error);

    mutable std::mutex mutex_;
    std::string path_;
    Json theme_;
    uint64_t version_ = 1;
};

/** The built-in look: what a receiver shows before anyone customises it. */
Json default_theme();

/**
 * Checks a theme the admin panel submitted.
 *
 * Deliberately strict about the things that end up in CSS or in a URL, and
 * deliberately permissive about everything else. An operator can put whatever
 * they like in a widget's title; they cannot put `url(javascript:...)` in a
 * colour.
 */
bool validate_theme(const Json& theme, std::string& error);

}  // namespace fernsdr

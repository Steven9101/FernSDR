#include "theme.h"

#include <algorithm>
#include <cctype>

#include "../util/config.h"
#include "../util/log.h"

namespace fernsdr {

namespace {

// The tokens an operator may set. Anything outside this list is dropped rather
// than passed through: these names become CSS custom properties on the page,
// and an open-ended map would let one receiver's theme define properties the
// client never meant to expose.
const char* const kColorTokens[] = {
    "background", "card", "popover", "muted", "elevated",
    "foreground", "mutedForeground", "subtleForeground",
    "border", "borderStrong", "primary", "primaryForeground",
    "signal", "warning", "destructive", "success",
};

const char* const kWidgetTypes[] = {
    "chat", "lightning", "clock", "notice", "links", "image", "embed", "spots", "space", "greyline", "station",
};

bool is_hex_colour(const std::string& value) {
    if (value.size() != 4 && value.size() != 7 && value.size() != 9) return false;
    if (value[0] != '#') return false;
    for (size_t i = 1; i < value.size(); i++) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

// Only http(s) and data:image. A background image is written into a CSS `url()`
// and an <img> src; `javascript:` in either is a scripting hole, and allowing
// arbitrary schemes to please one operator is not worth it.
bool is_safe_url(const std::string& value) {
    if (value.empty()) return true;
    if (value.size() > 4096) return false;
    if (value.find_first_of("\"'()\\\n\r\t<>") != std::string::npos) return false;
    const auto starts_with = [&value](const char* prefix) {
        return value.rfind(prefix, 0) == 0;
    };
    if (starts_with("https://") || starts_with("http://")) return true;
    if (starts_with("/")) return true;  // something the operator put in the web root
    if (starts_with("data:image/")) return true;
    return false;
}

// What a link may open: a web page, or a page on this receiver. A data: or
// javascript: address would run in the listener's page, stopped today only
// by the page's Content-Security-Policy, which a proxy or a copy of the
// pages served elsewhere may not send.
bool is_link_url(const std::string& value) {
    return !value.empty() && is_safe_url(value) && value.rfind("data:", 0) != 0;
}

// A value checked as text or as a number must be one, or be left out: the
// checks read anything else as "" or as the default, and the theme is stored
// as it came, so a number where the page expects a URL passed and stopped
// the listener's page from starting.
bool text_or_absent(const Json& value) { return value.is_null() || value.is_string(); }
bool number_or_absent(const Json& value) { return value.is_null() || value.is_number(); }

bool known(const char* const* list, size_t count, const std::string& value) {
    for (size_t i = 0; i < count; i++) {
        if (value == list[i]) return true;
    }
    return false;
}

}  // namespace

Json default_theme() {
    Json theme = Json::make_object();
    theme.set("name", "Default");

    // Empty means "use the built-in monochrome palette". Storing the whole
    // palette here instead would freeze today's defaults into every receiver's
    // theme file and make future changes invisible to anyone who ever opened
    // the appearance page.
    theme.set("colors", Json::make_object());

    Json background = Json::make_object();
    background.set("image", "");
    background.set("opacity", 0.35);
    background.set("blur", 0);
    background.set("position", "cover");
    theme.set("background", background);

    theme.set("logo", "");
    theme.set("palette", "classic");
    theme.set("accentWaterfall", false);

    Json widgets = Json::make_array();
    theme.set("widgets", widgets);
    return theme;
}

bool validate_theme(const Json& theme, std::string& error) {
    if (!theme.is_object()) {
        error = "theme must be an object";
        return false;
    }

    const Json& colors = theme["colors"];
    if (!colors.is_null() && !colors.is_object()) {
        error = "colors must be an object";
        return false;
    }
    if (colors.is_object()) {
        for (const auto& [key, value] : colors.members()) {
            if (!known(kColorTokens, sizeof(kColorTokens) / sizeof(*kColorTokens), key)) {
                error = "unknown colour '" + key + "'";
                return false;
            }
            if (!value.is_string() || !is_hex_colour(value.string())) {
                error = "colour '" + key + "' must be a hex value like #1a2b3c";
                return false;
            }
        }
    }

    // Which signal meter listeners see. The operator's taste, not ours: the
    // bar is the default and stays, and the rest read the same calibrated
    // dBm underneath.
    const Json& meter = theme["meter"];
    if (!meter.is_null()) {
        static const char* kMeters[] = {"bar", "needle", "numeric", "history"};
        if (!meter.is_string() || !known(kMeters, sizeof(kMeters) / sizeof(*kMeters), meter.string())) {
            error = "the meter must be one of bar, needle, numeric or history";
            return false;
        }
    }

    const Json& background = theme["background"];
    if (!background.is_null() && !background.is_object()) {
        error = "background must be an object";
        return false;
    }
    if (background.is_object()) {
        if (!text_or_absent(background["image"]) || !is_safe_url(background["image"].string())) {
            error = "the background image must be an https:// URL, a path on this receiver, "
                    "or a data:image value";
            return false;
        }
        const double opacity = background["opacity"].number(0.35);
        if (!number_or_absent(background["opacity"]) || opacity < 0.0 || opacity > 1.0) {
            error = "background opacity must be between 0 and 1";
            return false;
        }
        const double blur = background["blur"].number(0);
        if (!number_or_absent(background["blur"]) || blur < 0.0 || blur > 40.0) {
            error = "background blur must be between 0 and 40 pixels";
            return false;
        }
    }

    if (!text_or_absent(theme["logo"]) || !is_safe_url(theme["logo"].string())) {
        error = "the logo must be an https:// URL, a path on this receiver, or a data:image value";
        return false;
    }

    const Json& widgets = theme["widgets"];
    if (!widgets.is_null() && !widgets.is_array()) {
        error = "widgets must be a list";
        return false;
    }
    if (widgets.is_array()) {
        if (widgets.size() > 24) {
            error = "that is more widgets than a page can usefully hold";
            return false;
        }
        for (const Json& widget : widgets.elements()) {
            if (!widget.is_object()) {
                error = "each widget must be an object";
                return false;
            }
            const std::string type = widget["type"].string();
            if (!known(kWidgetTypes, sizeof(kWidgetTypes) / sizeof(*kWidgetTypes), type)) {
                error = "unknown widget type '" + type + "'";
                return false;
            }
            if (!text_or_absent(widget["title"]) || !text_or_absent(widget["url"])) {
                error = "a widget's title and address must be text";
                return false;
            }
            const std::string title = widget["title"].string();
            if (title.size() > 120) {
                error = "a widget title that long will not fit anywhere useful";
                return false;
            }
            for (char c : title) {
                if (static_cast<unsigned char>(c) < 0x20) {
                    error = "a widget title cannot contain control characters";
                    return false;
                }
            }
            if (type == "embed" || type == "image" || type == "lightning") {
                const std::string url = widget["url"].string();
                if (!is_safe_url(url)) {
                    error = "widget '" + type + "' needs an https:// URL";
                    return false;
                }
            }
            if (type == "links") {
                const Json& items = widget["items"];
                if (!items.is_null() && !items.is_array()) {
                    error = "the links widget's items must be a list";
                    return false;
                }
                for (size_t j = 0; j < items.size(); j++) {
                    if (!is_link_url(items[j]["url"].string())) {
                        error = "each link needs an https:// address or a path on this receiver";
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

void ThemeStore::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
    theme_ = default_theme();

    std::string text;
    if (!read_text_file(path, text)) {
        LOG_INFO("theme", "no %s; using the built-in look", path.c_str());
        return;
    }
    Json parsed;
    std::string reason;
    if (!Json::parse(text, parsed, reason)) {
        LOG_WARN("theme", "%s cannot be read: %s; using the built-in look", path.c_str(), reason.c_str());
        return;
    }
    std::string error;
    if (!validate_theme(parsed, error)) {
        LOG_WARN("theme", "%s rejected (%s); using the built-in look", path.c_str(), error.c_str());
        return;
    }
    theme_ = parsed;
    LOG_INFO("theme", "loaded %s", path.c_str());
}

Json ThemeStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return theme_;
}

uint64_t ThemeStore::version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

bool ThemeStore::replace(const Json& theme, std::string& error) {
    if (!validate_theme(theme, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    theme_ = theme;
    version_++;
    return write(error);
}

bool ThemeStore::reset(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The built-in look, not the built-in page: the widgets are the
    // operator's content, a notice, links, the chat, and a reset of colours
    // must not take them away.
    Json widgets = theme_.has("widgets") ? theme_["widgets"] : Json::make_array();
    theme_ = default_theme();
    theme_.set("widgets", widgets);
    version_++;
    return write(error);
}

bool ThemeStore::write(std::string& error) {
    if (path_.empty()) return true;
    return write_text_file(path_, theme_.serialize(), error);
}

}  // namespace fernsdr

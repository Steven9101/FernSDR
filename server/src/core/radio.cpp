#include "radio.h"
#include <sched.h>
#include <algorithm>
#include <set>
#include <cctype>
#include <unistd.h>
#include <chrono>
#include <cmath>

#include "../util/log.h"
#include "../util/password.h"
#include "directory.h"
#include "space_weather.h"
#include "../version.h"

namespace fernsdr {

namespace {

// The settings both the panel and the config file can set, by the name they
// have in both.
const char* const kPanelKeys[] = {
    "name", "operator", "location", "grid", "antenna", "contact", "website", "notice", "max_users", "listener_timeout",
    "sdr_list", "public_host", "public_port", "band_plan",
};

// What [site] band_plan may say. The page carries a plan for each; one it
// does not know would show no plan at all without saying why.
const char* const kBandPlans[] = {"auto", "none", "r1", "r2", "r3", "us", "ca", "gb", "de", "au", "jp"};

bool valid_band_plan(const std::string& plan) {
    for (const char* candidate : kBandPlans) {
        if (plan == candidate) return true;
    }
    return false;
}

// One place that knows the overlay's field names, so reading and writing
// cannot drift apart.
template <typename Apply>
void for_each_site_field(SiteInfo& site, Apply apply) {
    apply("name", site.name);
    apply("operator", site.operator_name);
    apply("location", site.location);
    apply("grid", site.grid_square);
    apply("antenna", site.antenna);
    apply("contact", site.contact);
    apply("website", site.website);
    apply("notice", site.notice);
    apply("public_host", site.public_host);
    apply("band_plan", site.band_plan);
}

bool is_panel_key(const std::string& key) {
    for (const char* candidate : kPanelKeys) {
        if (key == candidate) return true;
    }
    return false;
}

// The panel's settings as `config` has them, null where it has none.
Json file_record(const Config& config) {
    const ConfigSection& site = config.section("site");
    Json record = Json::make_object();
    for (const char* key : kPanelKeys) record.set(key, site.has(key) ? Json(site.get(key)) : Json());
    return record;
}

bool valid_site_value(const std::string& key, const Json& value, std::string& why) {
    if (key == "max_users") {
        const double users = value.number(-1);
        if (users < 1 || users > 100000) {
            why = "the listener limit must be between 1 and 100000";
            return false;
        }
        return true;
    }
    if (key == "listener_timeout") {
        const double minutes = value.number(-1);
        if (minutes < 0 || minutes > 1440 || minutes != std::floor(minutes)) {
            why = "the listener timeout must be a whole number of minutes from 0 (never) to 1440";
            return false;
        }
        return true;
    }
    if (key == "sdr_list") {
        if (!value.is_bool()) {
            why = "'sdr_list' must be true or false";
            return false;
        }
        return true;
    }
    if (key == "public_port") {
        const double port = value.number(-1);
        if (port < 0 || port > 65535 || port != std::floor(port)) {
            why = "the public port must be a port number, or 0 for the one the receiver listens on";
            return false;
        }
        return true;
    }
    if (key == "band_plan" && !(value.is_string() && valid_band_plan(value.string()))) {
        why = "the band plan must be auto, none, r1, r2, r3, us, ca, gb, de, au or jp";
        return false;
    }
    if (key == "public_host" && value.is_string() && !value.string().empty() && !valid_public_host(value.string())) {
        why = "the public address must be a host name or IPv4 address, without http:// or a port";
        return false;
    }
    if (!value.is_string()) {
        why = "'" + key + "' must be text";
        return false;
    }
    if (value.string().size() > 2000) {
        why = "'" + key + "' is too long";
        return false;
    }
    return true;
}

}  // namespace

bool Radio::configure(const Config& config, std::string& error) {
    const ConfigSection& site = config.section("site");
    site_.name = site.get("name", "FernSDR");
    site_.operator_name = site.get("operator", "");
    site_.location = site.get("location", "");
    site_.grid_square = site.get("grid", "");
    site_.antenna = site.get("antenna", "");
    site_.contact = site.get("contact", "");
    site_.website = site.get("website", "");
    site_.notice = site.get("notice", "");
    site_.public_host = site.get("public_host", "");
    site_.band_plan = site.get("band_plan", "auto");
    if (!valid_band_plan(site_.band_plan)) {
        error = "[site] band_plan must be auto, none, r1, r2, r3, us, ca, gb, de, au or jp, not '" + site_.band_plan + "'";
        return false;
    }
    site_.source_url = site.get("source_url", "https://github.com/Steven9101/FernSDR");
    spot_server_ = site.get("spot_server", "report.pskreporter.info:4739");
    // Before the overlay, which may hold a later limit from the panel: read
    // after it, the file's value replaced the panel's on every restart.
    site_.max_users = static_cast<int>(site.get_int("max_users", 200));
    site_.listener_timeout_minutes = static_cast<int>(std::clamp<long>(site.get_int("listener_timeout", 0), 0, 1440));
    site_.sdr_list = site.get_bool("sdr_list", false);
    site_.public_port = static_cast<int>(site.get_int("public_port", 0));
    file_record_ = file_record(config);

    // Beside the config, so copying a receiver copies its look with it.
    //
    // Only when there IS a config file. A config built from a string - which is
    // what the tests do, and what an embedder would do - has no directory to
    // sit beside, and an earlier version resolved that to the current working
    // directory: one test wrote an overlay there and every later test in the
    // process silently loaded it, renaming a receiver that had asked to be
    // called something else.
    const std::string config_path = config.path();
    config_path_ = config_path;
    if (!config_path.empty()) {
        const size_t slash = config_path.find_last_of('/');
        const std::string directory = slash == std::string::npos ? std::string()
                                                                 : config_path.substr(0, slash + 1);
        theme_.load(site.get("theme_file", directory + "fernsdr-theme.json"));
        load_overlay(directory + "fernsdr-settings.json");
    }
    site_.max_users_per_address = std::max(0, static_cast<int>(site.get_int("max_users_per_address", 16)));
    site_.chat = site.get_bool("chat", true);
    chat_.set_enabled(site_.chat);

    const auto sections = config.sections_with_prefix("band");
    module_devices_.clear();
    if (sections.empty()) {
        error = "no [band:...] sections in the config; the receiver has nothing to listen to";
        return false;
    }

    // Modules live beside the configuration unless [modules] says otherwise.
    // The section is only ever read from the file on the machine: it says
    // which programs this receiver may run.
    int modules_sections = 0;
    for (const ConfigSection& section : config.sections()) {
        if (section.name() == "modules") modules_sections++;
    }
    if (modules_sections > 1) {
        error = "there is more than one [modules] section; merge them into one";
        return false;
    }
    const ConfigSection& modules = config.section("modules");
    std::string module_directory = modules.get("directory", "fernsdr-modules");
    if (!module_directory.empty() && module_directory[0] != '/' && !config_path.empty()) {
        const size_t slash = config_path.find_last_of('/');
        if (slash != std::string::npos) module_directory = config_path.substr(0, slash + 1) + module_directory;
    }
    module_catalog_.clear();
    {
        std::string current;
        for (char c : modules.get("catalog", "Steven9101/Fern-RTLSDR Steven9101/Fern-RX888 Steven9101/Fern-SDRPlay Steven9101/Fern-FT8") + " ") {
            if (c == ' ' || c == ',' || c == '\t') {
                if (!current.empty()) module_catalog_.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
    }
    if (!module_store_) module_store_ = std::make_shared<ModuleStore>(module_directory);

    bool stdin_used = false;
    if (bands_.empty()) {
        cpu_set_t affinity;
        CPU_ZERO(&affinity);
        const size_t cpus = ::sched_getaffinity(0, sizeof(affinity), &affinity) == 0
            ? static_cast<size_t>(CPU_COUNT(&affinity)) : std::thread::hardware_concurrency();
        const size_t spare = cpus > sections.size() + 1 ? cpus - sections.size() - 1 : 0;
        const long requested = config.section("server").get_int("dsp_workers", std::min<size_t>(8, spare));
        const size_t workers = static_cast<size_t>(std::clamp(requested, 0L, 32L));
        try {
            dsp_workers_ = std::make_unique<DspWorkers>(workers, sections.size());
        } catch (const std::exception& failure) {
            error = std::string("cannot start DSP workers: ") + failure.what();
            return false;
        }
    }
    for (const ConfigSection* section : sections) {
        const std::string kind = section->get("source", "test");
        const std::string path = section->get("path", kind == "stdin" ? "-" : "");
        if ((kind == "stdin" || kind == "file" || kind == "pipe") &&
            (path == "-" || path == "stdin")) {
            if (stdin_used) {
                error = "only one band may read stdin; use separate FIFO or UDP inputs for multiple bands";
                return false;
            }
            stdin_used = true;
        }
        const std::string id = section->name().substr(std::string("band:").size());
        if (id.empty()) {
            error = "a [band:] section has no name";
            return false;
        }
        // The id names files (the default history archive) and travels in
        // URLs, so it is kept to characters that are harmless in both.
        const bool plain = id.size() <= 64 && id.front() != '.' &&
                           std::all_of(id.begin(), id.end(), [](unsigned char c) {
                               return std::isalnum(c) || c == '-' || c == '_' || c == '.';
                           });
        if (!plain) {
            error = "band '" + id + "' needs a name of letters, digits, '-', '_' and '.', not starting with '.'";
            return false;
        }
        for (const auto& existing : bands_) {
            if (existing->id() == id) {
                error = "duplicate band '" + id + "'";
                return false;
            }
        }

        std::string source_error;
        SourceContext context;
        context.modules = module_store_;
        auto source = make_source(*section, context, source_error);
        if (!source) {
            error = source_error;
            return false;
        }
        BandHours hours;
        std::string hours_error;
        if (!BandHours::parse(section->get("hours", ""), hours, hours_error)) {
            error = "[" + section->name() + "] hours: " + hours_error;
            return false;
        }
        size_t input = bands_.size();
        if (kind == "module") {
            // One device, one band at a time: a second band on it would only
            // ever be told the device is busy, and the first would lose it at
            // each of the second's restarts. Bands may share one when their
            // hours never meet, which the schedule keeps to. Two names for one
            // device (an index and a serial) still meet at run time, where
            // the module says busy.
            std::string device = section->get("module.device", "");
            device.erase(0, device.find_first_not_of(" \t"));
            device.erase(device.find_last_not_of(" \t") + 1);
            std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return std::tolower(c); });
            const std::string module = section->get("module");
            const auto [where, fresh] = module_devices_.emplace(module + "\n" + device, id);
            if (!fresh) {
                const auto both = [&](const std::string& other) {
                    return device.empty()
                        ? "bands '" + other + "' and '" + id + "' both take the first " + module + " device"
                        : "bands '" + other + "' and '" + id + "' both name " + module + " device '" + device + "'";
                };
                for (size_t other = 0; other < bands_.size(); other++) {
                    if (bands_[other]->id() != where->second) continue;
                    input = input_of_[other];
                    // Hours of the clock meet or not wherever the station is,
                    // and so do hours that are always or the same. Other
                    // hours with sunrise or sunset may meet only somewhere
                    // else, or where the panel has put the station since;
                    // the schedule then gives the input to the band on the
                    // air first, and the log says why.
                    const StationPlace* place = nullptr;
                    StationPlace here;
                    if (locator_centre(site_.grid_square, here.lat, here.lon)) place = &here;
                    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch()).count();
                    for (size_t mate = 0; mate < bands_.size(); mate++) {
                        if (input_of_[mate] != input) continue;
                        const bool always_meet = hours.always() || hours_[mate].always() || hours == hours_[mate];
                        if (!always_meet && hours_overlap(hours_[mate], hours, now_ms, 366, place) < 0) continue;
                        const std::string pair = both(bands_[mate]->id());
                        if (always_meet || (!hours_[mate].uses_sun() && !hours.uses_sun())) {
                            error = pair + (hours.always() && hours_[mate].always()
                                ? "; give each its own module.device, as the Modules page lists them, or give them "
                                  "hours that do not overlap, as 06:00-18:00 and 18:00-06:00"
                                : " at the same hours; give them hours that do not overlap, as sunrise-sunset and "
                                  "sunset-sunrise");
                            return false;
                        }
                        LOG_WARN("radio", "%s, and their hours overlap; whichever is on the air first keeps it",
                                 pair.c_str());
                    }
                    break;
                }
            }
            module_bands_[module].push_back(id);
        }
        if (source->sample_rate() <= 0.0) {
            error = "[" + section->name() + "] sample_rate must be positive";
            return false;
        }

        bands_.push_back(std::make_unique<Band>(id, section->get("name", id), std::move(source), *section));
        bands_.back()->set_dsp_workers(dsp_workers_.get());
        hours_.push_back(hours);
        input_of_.push_back(input);
        next_change_.push_back(-1);
    }

    // Now the bands exist, the overlay's per-band settings can be applied.
    // Each on its own: a setting saved for wiring that has since changed, a
    // swap saved for what is now a real input, must not take the others with it.
    if (pending_bands_.is_object()) {
        for (const auto& [id, entry] : pending_bands_.members()) {
            Band* target = band(id);
            if (!target || !entry.is_object()) {
                LOG_WARN("radio", "saved settings for band '%s' ignored: %s", id.c_str(),
                         target ? "they are not an object" : "there is no such band");
                continue;
            }
            // The history keys go together: applied one at a time, each would
            // reopen the archive in a shape the next one changes, and an
            // archive reopened in another shape starts again empty.
            Json history = Json::make_object();
            std::vector<std::pair<std::string, Json>> groups;
            for (const auto& [key, value] : entry.members()) {
                if (key == "history" || key.rfind("history_", 0) == 0) {
                    history.set(key, value);
                    continue;
                }
                Json single = Json::make_object();
                single.set(key, value);
                groups.emplace_back(key, std::move(single));
            }
            if (history.size() > 0) groups.emplace_back("history", std::move(history));
            for (const auto& [name, values] : groups) {
                std::string band_error;
                if (!apply_band_values(target, values, band_error)) {
                    LOG_WARN("radio", "saved %s for band '%s' ignored: %s", name.c_str(), id.c_str(),
                             band_error.c_str());
                }
            }
        }
        pending_bands_ = Json();
    }

    if (!configure_decoders(config, error)) return false;
    // Every decode kept goes to the spot reporter, which takes it only from
    // a decoder that reports and only when the rules allow.
    spots_.set_software([this] { return spot_software(); });
    decodes_.set_added_callback([this](const Decode& decode) {
        {
            std::lock_guard<std::mutex> lock(reporting_mutex_);
            if (!reporting_.count(decode.decoder)) return;
        }
        spots_.add(decode);
    });
    configure_spots();

    LOG_INFO("radio", "%s: %zu band(s), up to %d users", site_.name.c_str(), bands_.size(), site_.max_users);
    return true;
}

bool Radio::configure_decoders(const Config& config, std::string& error) {
    if (decoder_launcher_.empty()) {
        char path[4096];
        const ssize_t size = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (size > 0) decoder_launcher_.assign(path, static_cast<size_t>(size));
    }
    std::vector<DecoderConfig> configs;
    DecoderSettings settings;
    if (!parse_decoders(config, configs, settings, error)) return false;
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    decoder_configs_ = std::move(configs);
    decoder_settings_ = std::move(settings);
    public_from_.clear();
    for (const auto& c : decoder_configs_) {
        if (c.listed) public_from_[c.id] = 0;
    }
    return true;
}

bool Radio::parse_decoders(const Config& config, std::vector<DecoderConfig>& configs, DecoderSettings& all_settings,
                           std::string& error, std::map<std::string, UnplacedDecoder>* unplaced) {
    configs.clear();
    all_settings.clear();
    std::set<std::string> seen;
    for (const ConfigSection* section : config.sections_with_prefix("decoder")) {
        DecoderConfig decoder;
        decoder.id = section->name().substr(std::string("decoder:").size());
        // Two sections of one name would be two decoders the panel and the
        // decodes could not tell apart.
        if (!seen.insert(decoder.id).second) {
            error = "[" + section->name() + "] appears twice; merge the two sections";
            return false;
        }
        const bool plain = !decoder.id.empty() && decoder.id.size() <= 32 &&
                           std::all_of(decoder.id.begin(), decoder.id.end(), [](unsigned char c) {
                               return std::islower(c) || std::isdigit(c) || c == '-';
                           });
        if (!plain) {
            error = "[" + section->name() + "] needs a name of lowercase letters, digits and '-'";
            return false;
        }
        decoder.module = section->get("module", "");
        if (!valid_module_id(decoder.module)) {
            error = "[" + section->name() + "] needs module = <the decoder module's id>";
            return false;
        }
        const std::string mode = section->get("mode", "ft8");
        const bool mode_plain = !mode.empty() && mode.size() <= 8 &&
                                std::all_of(mode.begin(), mode.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c); });
        if (!mode_plain) {
            error = "[" + section->name() + "] mode must be a short lowercase name such as ft8";
            return false;
        }
        double width = 4000.0;
        if (!parse_frequency(section->get("width", "4000"), width) || width < 500.0 || width > 48000.0) {
            error = "[" + section->name() + "] width must be between 500 Hz and 48 kHz";
            return false;
        }
        double offset = width / 2.0;
        if (!parse_frequency(section->get("offset", std::to_string(width / 2.0)), offset) || offset < -48000.0 ||
            offset > 48000.0) {
            error = "[" + section->name() + "] offset must be within 48 kHz of the dial";
            return false;
        }
        const std::string listed = section->get("public", "no");
        if (listed != "yes" && listed != "no") {
            error = "[" + section->name() + "] public must be yes or no";
            return false;
        }
        decoder.listed = listed == "yes";
        const std::string report = section->get("report", "none");
        if (report != "none" && report != "pskreporter") {
            error = "[" + section->name() + "] report must be none or pskreporter";
            return false;
        }
        decoder.report = report == "pskreporter";
        // Set when a channel names a band this receiver does not run, or runs
        // narrower, yet: see apply_decoders.
        std::string misplaced;
        std::string token;
        for (char c : section->get("channels", "") + " ") {
            if (c != ' ' && c != ',' && c != '\t') {
                token += c;
                continue;
            }
            if (token.empty()) continue;
            const size_t colon = token.find(':');
            DecoderChannelConfig channel;
            channel.mode = mode;
            channel.offset_hz = offset;
            channel.width_hz = width;
            channel.band = colon == std::string::npos ? "" : token.substr(0, colon);
            Band* target = band(channel.band);
            if (colon == std::string::npos || !parse_frequency(token.substr(colon + 1), channel.dial_hz) ||
                (!target && !unplaced)) {
                error = "[" + section->name() + "] channel '" + token +
                        "' is not <band>:<dial frequency> with a configured band";
                return false;
            }
            const double low = channel.dial_hz + offset - width / 2.0;
            const double high = channel.dial_hz + offset + width / 2.0;
            if (!target) {
                misplaced = "band '" + channel.band + "' is not running yet";
            } else if (low < target->sample_low_hz() || high > target->sample_high_hz()) {
                const std::string why = "channel '" + token + "' reaches outside what band '" + channel.band + "' receives";
                if (!unplaced) {
                    error = "[" + section->name() + "] " + why;
                    return false;
                }
                misplaced = why + " now";
            }
            for (const auto& other : decoder.channels) {
                if (other.id() == channel.id()) {
                    error = "[" + section->name() + "] lists channel '" + token + "' twice";
                    return false;
                }
            }
            decoder.channels.push_back(channel);
            token.clear();
        }
        if (decoder.channels.empty() || decoder.channels.size() > 32) {
            error = "[" + section->name() + "] needs channels = <band>:<dial> ..., at most 32";
            return false;
        }
        std::map<std::string, std::string> settings;
        for (const auto& [key, value] : section->values()) {
            if (key.rfind("module.", 0) == 0) settings[key.substr(7)] = value;
        }
        if (!misplaced.empty()) {
            (*unplaced)[decoder.id] = {decoder.listed, decoder.report, misplaced};
            continue;
        }
        all_settings[decoder.id] = settings;
        configs.push_back(decoder);
    }
    return true;
}

std::vector<DecoderConfig> Radio::decoder_configs() const {
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    return decoder_configs_;
}

Json Radio::decoders_status() const {
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    Json list = Json::make_array();
    for (const auto& decoder : decoders_) {
        Json entry = decoder->status();
        const auto config = std::find_if(decoder_configs_.begin(), decoder_configs_.end(),
                                         [&](const DecoderConfig& c) { return c.id == decoder->config().id; });
        entry.set("public", config != decoder_configs_.end() && config->listed);
        entry.set("report", config != decoder_configs_.end() && config->report);
        list.push_back(entry);
    }
    return list;
}

bool Radio::restart_decoder(const std::string& id) {
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    for (auto& decoder : decoders_) {
        if (decoder->config().id != id) continue;
        decoder->restart();
        return true;
    }
    return false;
}

Json Radio::listed_decoders_json() const {
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    Json out = Json::make_array();
    for (const DecoderConfig& config : decoder_configs_) {
        if (!config.listed) continue;
        Json decoder = Json::make_object();
        decoder.set("id", config.id);
        Json channels = Json::make_array();
        for (const auto& channel : config.channels) {
            Json entry = Json::make_object();
            entry.set("id", channel.id());
            entry.set("band", channel.band);
            entry.set("mode", channel.mode);
            entry.set("dial", channel.dial_hz);
            entry.set("low", channel.offset_hz - channel.width_hz / 2.0);
            entry.set("high", channel.offset_hz + channel.width_hz / 2.0);
            channels.push_back(entry);
        }
        decoder.set("channels", channels);
        out.push_back(decoder);
    }
    return out;
}

bool Radio::decoder_listed(const std::string& id) const {
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    return public_from_.count(id) > 0;
}

std::map<std::string, uint64_t> Radio::public_decoders() const {
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    return public_from_;
}

void Radio::start_decoders() {
    std::lock_guard<std::mutex> lock(decoders_mutex_);
    for (const DecoderConfig& config : decoder_configs_) {
        auto decoder = make_decoder(config, decoder_settings_[config.id]);
        if (decoder) decoders_.push_back(std::move(decoder));
    }
}

std::unique_ptr<Decoder> Radio::make_decoder(const DecoderConfig& config, const std::map<std::string, std::string>& raw) {
    {
        auto store = module_store_;
        const std::string launcher = decoder_launcher_;
        auto resolve = [store, raw, launcher](const std::string& module, Decoder::Program& out, std::string& error) {
            if (!store) {
                error = "this receiver has no module directory";
                return false;
            }
            ModuleStore::Launch launch;
            if (store->acquire(module, launch, error) != ModuleStore::Resolve::Ok) return false;
            auto give_back = [store, module, version = launch.version] { store->release(module, version); };
            if (launch.manifest.kind != "decoder") {
                give_back();
                error = module + " is not a decoder module";
                return false;
            }
            Json settings = Json::make_object();
            for (const auto& [key, text] : raw) {
                const ModuleSetting* setting = launch.manifest.setting(key);
                Json value;
                if (!setting) {
                    give_back();
                    error = module + " has no setting '" + key + "'";
                    return false;
                }
                if (!type_module_setting(*setting, text, value, error)) {
                    give_back();
                    return false;
                }
                settings.set(key, value);
            }
            out.executable = launch.executable;
            out.launcher = launcher;
            out.settings = settings;
            out.release = give_back;
            return true;
        };
        auto decoder = std::make_unique<Decoder>(config, resolve, [this](const std::string& id) { return band(id); },
                                                 decodes_);
        std::string problem;
        if (!decoder->start(problem)) {
            LOG_WARN("radio", "decoder %s not started: %s", config.id.c_str(), problem.c_str());
            return nullptr;
        }
        return decoder;
    }
}

namespace {

// What a decoder's process depends on: everything in its section but
// `public`, which only decides who sees the decodes.
std::string decoder_signature(const DecoderConfig& config, const std::map<std::string, std::string>& settings) {
    std::string out = config.module + "\n";
    for (const auto& channel : config.channels) {
        out += channel.id() + " " + std::to_string(channel.dial_hz) + " " + std::to_string(channel.offset_hz) + " " +
               std::to_string(channel.width_hz) + "\n";
    }
    for (const auto& [key, value] : settings) out += key + "=" + value + "\n";
    return out;
}

}  // namespace

DecoderChanges Radio::apply_decoders(const Config& config) {
    DecoderChanges result;
    std::vector<DecoderConfig> configs;
    DecoderSettings settings;
    std::map<std::string, UnplacedDecoder> unplaced;
    std::string error;
    if (!parse_decoders(config, configs, settings, error, &unplaced)) {
        LOG_WARN("radio", "decoders left as they were: %s", error.c_str());
        result.waiting["*"] = error;
        return result;
    }
    // Read before the decoders' lock: the decode store is locked first
    // wherever both are held.
    const uint64_t newest = decodes_.last_sequence();
    std::vector<std::unique_ptr<Decoder>> retired;
    {
        std::lock_guard<std::mutex> lock(decoders_mutex_);
        // A decoder whose channels cannot be placed on the bands running now
        // (a band added or widened in the same save, which needs a restart)
        // keeps its present form until then, and nothing new starts for it.
        // Its `public` applies at once all the same: that one needs no band.
        for (const auto& [id, why] : unplaced) {
            result.waiting[id] = why.reason;
            const auto old = std::find_if(decoder_configs_.begin(), decoder_configs_.end(),
                                          [&](const DecoderConfig& c) { return c.id == id; });
            if (old == decoder_configs_.end()) continue;
            DecoderConfig kept = *old;
            kept.listed = why.listed;
            kept.report = why.report;
            configs.push_back(kept);
            settings[id] = decoder_settings_[id];
        }
        const auto signature_of = [](const std::vector<DecoderConfig>& list, const DecoderSettings& all,
                                     const std::string& id) -> std::string {
            for (const auto& c : list) {
                if (c.id == id) return decoder_signature(c, all.count(id) ? all.at(id) : std::map<std::string, std::string>{});
            }
            return "";
        };
        for (auto it = decoders_.begin(); it != decoders_.end();) {
            const std::string id = (*it)->config().id;
            const std::string before = signature_of(decoder_configs_, decoder_settings_, id);
            const std::string after = signature_of(configs, settings, id);
            if (!after.empty() && after == before) {
                ++it;
                continue;
            }
            LOG_INFO("radio", "decoder %s %s", id.c_str(), after.empty() ? "removed" : "changed");
            result.changed.push_back(id);
            retired.push_back(std::move(*it));
            it = decoders_.erase(it);
        }
        // Published from the moment it is made public: what it decoded while
        // private stays with the operator.
        std::map<std::string, uint64_t> public_from;
        for (const auto& c : configs) {
            if (!c.listed) continue;
            const auto was = public_from_.find(c.id);
            public_from[c.id] = was != public_from_.end() ? was->second : newest;
        }
        public_from_ = std::move(public_from);
        decoder_configs_ = configs;
        decoder_settings_ = settings;
        for (const DecoderConfig& c : decoder_configs_) {
            const bool running = std::any_of(decoders_.begin(), decoders_.end(),
                                             [&](const std::unique_ptr<Decoder>& d) { return d->config().id == c.id; });
            if (running) continue;
            if (std::find(result.changed.begin(), result.changed.end(), c.id) == result.changed.end()) {
                result.changed.push_back(c.id);
            }
            if (auto decoder = make_decoder(c, decoder_settings_[c.id])) decoders_.push_back(std::move(decoder));
        }
    }
    // Stopping waits for the module to exit, up to seconds for one that does
    // not listen; this runs on the server's thread, which everyone's audio
    // goes through. Radio::stop() waits for these.
    if (!retired.empty()) {
        std::lock_guard<std::mutex> lock(stoppers_mutex_);
        for (auto it = stoppers_.begin(); it != stoppers_.end();) {
            if (!it->done->load()) {
                ++it;
                continue;
            }
            it->thread.join();
            it = stoppers_.erase(it);
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread thread([retired = std::move(retired), done]() mutable {
            for (auto& decoder : retired) decoder->stop();
            retired.clear();
            done->store(true);
        });
        stoppers_.push_back({std::move(thread), std::move(done)});
    }
    configure_spots();
    return result;
}

bool Radio::start(std::string& error) {
    // Only the bands whose hours it is; the rest start when theirs come.
    schedule_known_ = false;
    apply_schedule(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count());
    for (auto& band : bands_) {
        if (!band->on_air()) {
            LOG_INFO("band", "%s is off the air until its hours come (%s)", band->id().c_str(), band->status().c_str());
            continue;
        }
        if (!band->start(error)) {
            stop();
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        schedule_stopping_ = false;
        if (!schedule_thread_.joinable()) schedule_thread_ = std::thread([this] { run_schedule(); });
    }
    bool none;
    {
        std::lock_guard<std::mutex> lock(decoders_mutex_);
        none = decoders_.empty();
    }
    if (none) start_decoders();
    if (!modules_ && module_store_) {
        modules_ = std::make_unique<ModuleManager>(module_store_, module_catalog_);
        modules_->set_bands_callback([this](const std::string& id) { return bands_using_module(id); });
        modules_->set_changed_callback([this](const std::string& id) { restart_module_bands(id); });
        modules_->publish();
    }
    return true;
}

void Radio::stop() {
    // The schedule first, so it neither starts a band being stopped nor
    // holds one it is stopping; what it had yet to do no longer matters.
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        schedule_stopping_ = true;
        schedule_jobs_.clear();
    }
    schedule_wake_.notify_all();
    if (schedule_thread_.joinable()) schedule_thread_.join();
    // Then modules, so no module job restarts a band while they are being stopped.
    modules_.reset();
    // Then the decoders, whose channels are on the bands, and those still
    // being stopped after a change.
    {
        std::lock_guard<std::mutex> lock(decoders_mutex_);
        for (auto& decoder : decoders_) decoder->stop();
        decoders_.clear();
    }
    std::vector<Stopper> stoppers;
    {
        std::lock_guard<std::mutex> lock(stoppers_mutex_);
        stoppers.swap(stoppers_);
    }
    for (auto& stopper : stoppers) stopper.thread.join();
    for (auto& band : bands_) band->stop();
}

std::vector<std::string> Radio::bands_using_module(const std::string& id) const {
    const auto found = module_bands_.find(id);
    return found == module_bands_.end() ? std::vector<std::string>{} : found->second;
}

void Radio::restart_module_bands(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(decoders_mutex_);
        for (auto& decoder : decoders_) {
            if (decoder->config().module == id) {
                LOG_INFO("radio", "restarting decoder %s for a change to module %s", decoder->config().id.c_str(), id.c_str());
                decoder->restart();
            }
        }
    }
    for (const std::string& band_id : bands_using_module(id)) {
        if (Band* band = this->band(band_id)) {
            LOG_INFO("radio", "restarting %s for a change to module %s", band_id.c_str(), id.c_str());
            band->restart();
        }
    }
}

Band* Radio::band(const std::string& id) {
    for (auto& b : bands_) {
        if (b->id() == id) return b.get();
    }
    return nullptr;
}

Band* Radio::default_band() {
    for (const auto& band : bands_) {
        if (band->on_air()) return band.get();
    }
    return bands_.empty() ? nullptr : bands_.front().get();
}

Band* Radio::successor(const Band* band) {
    for (size_t i = 0; i < bands_.size(); i++) {
        if (bands_[i].get() != band) continue;
        for (size_t j = 0; j < bands_.size(); j++) {
            if (j != i && input_of_[j] == input_of_[i] && bands_[j]->on_air()) return bands_[j].get();
        }
    }
    return nullptr;
}

std::string Radio::shared_input(const Band* band) const {
    for (size_t i = 0; i < bands_.size(); i++) {
        if (bands_[i].get() != band) continue;
        for (size_t j = 0; j < bands_.size(); j++) {
            if (j != i && input_of_[j] == input_of_[i]) return bands_[input_of_[i]]->id();
        }
    }
    return "";
}

void Radio::apply_hours(const Config& config) {
    for (size_t i = 0; i < bands_.size(); i++) {
        BandHours hours;
        std::string error;
        const ConfigSection& section = config.section("band:" + bands_[i]->id());
        // Only for the band that runs: a section rewired under the same id,
        // such as a radio's band in place of the test band, is another band,
        // and its hours start with it at the restart.
        auto wanted = section.values();
        auto built = bands_[i]->configured_values();
        wanted.erase("hours");
        built.erase("hours");
        if (wanted != built) continue;
        if (!BandHours::parse(section.get("hours", ""), hours, error)) continue;
        if (hours != hours_[i]) {
            LOG_INFO("radio", "%s is on the air %s from now", bands_[i]->id().c_str(), hours.text().c_str());
            hours_[i] = hours;
            schedule_key_.clear();
        }
    }
}

std::vector<Band*> Radio::apply_schedule(int64_t utc_ms) {
    std::vector<Band*> changed;
    if (bands_.empty()) return changed;
    StationPlace here;
    const bool placed = locator_centre(site_.grid_square, here.lat, here.lon);
    const StationPlace* place = placed ? &here : nullptr;
    // What the cached changes were worked out from: the place, since the
    // panel can move the station, and whether the hours were reloaded.
    const std::string key = placed ? site_.grid_square : std::string("-");
    const bool fresh = key != schedule_key_;
    schedule_key_ = key;

    std::vector<bool> wanted(bands_.size());
    for (size_t i = 0; i < bands_.size(); i++) wanted[i] = hours_[i].on_air(utc_ms, place);
    // One band per input: the one already on the air keeps it while its
    // hours last, else the first listed takes it.
    // A band its hours would have on, held off by the one keeping its input,
    // cannot say when it comes on: that is when the other's hours end.
    std::vector<bool> held(bands_.size(), false);
    for (size_t i = 0; i < bands_.size(); i++) {
        if (input_of_[i] != i) continue;
        size_t keeper = bands_.size();
        for (size_t j = 0; j < bands_.size(); j++) {
            if (input_of_[j] != i || !wanted[j]) continue;
            if (keeper == bands_.size() || (bands_[j]->on_air() && schedule_known_ && !bands_[keeper]->on_air())) keeper = j;
        }
        for (size_t j = 0; j < bands_.size(); j++) {
            if (input_of_[j] != i || j == keeper || !wanted[j]) continue;
            wanted[j] = false;
            held[j] = true;
        }
    }

    std::vector<std::pair<Band*, bool>> jobs;
    for (size_t i = 0; i < bands_.size(); i++) {
        Band& band = *bands_[i];
        const bool flips = wanted[i] != band.on_air();
        if (fresh || flips || (next_change_[i] >= 0 && utc_ms >= next_change_[i])) {
            next_change_[i] = held[i] ? -1 : hours_[i].next_change(utc_ms, place);
            band.set_schedule(wanted[i], hours_[i].text(), next_change_[i]);
        }
        if (!schedule_known_) continue;  // start() starts only those on the air
        if (flips) {
            LOG_INFO("radio", "%s goes %s the air by its hours (%s)", band.id().c_str(), wanted[i] ? "on" : "off",
                     hours_[i].text().c_str());
            jobs.emplace_back(&band, wanted[i]);
            changed.push_back(&band);
        }
    }
    schedule_known_ = true;
    if (jobs.empty()) return changed;
    bool threaded;
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        threaded = schedule_thread_.joinable();
        if (threaded) schedule_jobs_.insert(schedule_jobs_.end(), jobs.begin(), jobs.end());
    }
    if (threaded) schedule_wake_.notify_one();
    else carry_out(std::move(jobs));
    return changed;
}

void Radio::carry_out(std::vector<std::pair<Band*, bool>> jobs) {
    // The last word on each band stands; then all stops before any start,
    // so a band handing its input over lets go of it first.
    std::vector<std::pair<Band*, bool>> last;
    for (const auto& [band, on] : jobs) {
        auto found = std::find_if(last.begin(), last.end(), [&](const auto& entry) { return entry.first == band; });
        if (found != last.end()) found->second = on;
        else last.emplace_back(band, on);
    }
    for (const auto& [band, on] : last) {
        if (!on) band->stop();
    }
    for (const auto& [band, on] : last) {
        if (!on) continue;
        std::string error;
        if (!band->start(error)) LOG_ERROR("radio", "%s could not go on the air: %s", band->id().c_str(), error.c_str());
    }
}

void Radio::run_schedule() {
    std::unique_lock<std::mutex> lock(schedule_mutex_);
    while (true) {
        schedule_wake_.wait(lock, [&] { return schedule_stopping_ || !schedule_jobs_.empty(); });
        if (schedule_stopping_) break;
        std::vector<std::pair<Band*, bool>> jobs;
        jobs.swap(schedule_jobs_);
        schedule_busy_ = true;
        lock.unlock();
        carry_out(std::move(jobs));
        lock.lock();
        schedule_busy_ = false;
        schedule_idle_.notify_all();
    }
    schedule_busy_ = false;
    schedule_idle_.notify_all();
}

void Radio::wait_for_schedule() {
    std::unique_lock<std::mutex> lock(schedule_mutex_);
    schedule_idle_.wait(lock, [&] { return schedule_jobs_.empty() && !schedule_busy_; });
}

std::vector<BandInfo> Radio::band_info() const {
    std::vector<BandInfo> out;
    out.reserve(bands_.size());
    for (const auto& band : bands_) out.push_back(band->info());
    return out;
}

int Radio::total_listeners() const {
    int total = 0;
    for (const auto& band : bands_) total += band->listener_count();
    return total;
}

void Radio::set_wake_callback(const std::function<void()>& callback) {
    for (auto& band : bands_) band->set_wake_callback(callback);
}

void Radio::apply_site(const Config& after) {
    // Only the fields that mean something to a running receiver. Everything
    // here is read fresh on each request or announcement, so setting it is
    // enough - nothing has to be told.
    const ConfigSection& site = after.section("site");
    const Json record = file_record(after);
    const auto edited = [&](const char* key) { return record[key].serialize() != file_record_[key].serialize(); };
    // A setting deleted from the file goes back to its default, as at startup.
    for_each_site_field(site_, [&](const char* key, std::string& field) {
        if (edited(key)) field = site.get(key, std::string(key) == "name" ? "FernSDR" : "");
    });
    if (edited("max_users")) site_.max_users = static_cast<int>(site.get_int("max_users", 200));
    if (edited("listener_timeout")) {
        site_.listener_timeout_minutes = static_cast<int>(std::clamp<long>(site.get_int("listener_timeout", 0), 0, 1440));
    }
    if (edited("sdr_list")) site_.sdr_list = site.get_bool("sdr_list", false);
    if (edited("public_port")) site_.public_port = static_cast<int>(site.get_int("public_port", 0));
    file_record_ = record;
    site_.source_url = site.get("source_url", site_.source_url);
    spot_server_ = site.get("spot_server", "report.pskreporter.info:4739");
    configure_spots();
    site_.max_users_per_address =
        std::max(0, static_cast<int>(site.get_int("max_users_per_address", site_.max_users_per_address)));
    site_.chat = site.get_bool("chat", site_.chat);
    chat_.set_enabled(site_.chat);
    // Settings saved by a build that kept no record of the file would
    // otherwise win over this edit at the next start.
    std::string error;
    if (!save_overlay(overlay_document(), error)) LOG_WARN("radio", "settings not saved: %s", error.c_str());
    LOG_INFO("radio", "site settings reloaded: %s, up to %d users", site_.name.c_str(),
             site_.max_users);
}

std::vector<Radio::BandChange> Radio::band_changes(const Config& config) const {
    std::vector<BandChange> changes;
    for (const ConfigSection* section : config.sections_with_prefix("band")) {
        const std::string id = section->name().substr(section->name().find(':') + 1);
        const Band* running = nullptr;
        for (const auto& band : bands_) {
            if (band->id() == id) { running = band.get(); break; }
        }
        auto wanted = section->values();
        if (!running) {
            changes.push_back({id, "(the whole band)", "", "would start"});
            continue;
        }
        // Hours apply when the file is saved (apply_hours), not at a restart.
        wanted.erase("hours");
        auto built = running->configured_values();
        built.erase("hours");
        // A module band takes new module settings on a band restart; its
        // other settings shape the band's transforms and need a new band.
        const auto source = built.find("source");
        const bool module_band = source != built.end() && source->second == "module" && section->get("source") == "module";
        const auto band_restart = [&](const std::string& key) {
            return module_band && key.rfind("module.", 0) == 0;
        };
        for (const auto& [key, value] : wanted) {
            const auto found = built.find(key);
            if (found == built.end()) {
                changes.push_back({id, key, "", value, band_restart(key)});
            } else if (found->second != value) {
                changes.push_back({id, key, found->second, value, band_restart(key)});
            }
        }
        for (const auto& [key, value] : built) {
            if (wanted.find(key) == wanted.end()) changes.push_back({id, key, value, "", band_restart(key)});
        }
    }
    for (const auto& band : bands_) {
        bool still_configured = false;
        for (const ConfigSection* section : config.sections_with_prefix("band")) {
            if (section->name().substr(section->name().find(':') + 1) == band->id()) {
                still_configured = true;
                break;
            }
        }
        if (!still_configured) changes.push_back({band->id(), "(the whole band)", "running", ""});
    }
    return changes;
}

std::vector<std::string> Radio::bands_needing_restart(const Config& config) const {
    std::vector<std::string> changed;
    for (const ConfigSection* section : config.sections_with_prefix("band")) {
        const std::string id = section->name().substr(section->name().find(':') + 1);
        const Band* running = nullptr;
        for (const auto& band : bands_) {
            if (band->id() == id) { running = band.get(); break; }
        }
        // A band that is not running at all is a restart-to-appear, which the
        // operator has to do by restarting the receiver; say so either way.
        if (!running) {
            changed.push_back(id);
            continue;
        }
        // Hours apply when the file is saved (apply_hours), not at a restart.
        auto wanted = section->values();
        auto built = running->configured_values();
        wanted.erase("hours");
        built.erase("hours");
        if (built != wanted) changed.push_back(id);
    }
    // A band removed from the config is still running, and stopping it needs a
    // restart too.
    for (const auto& band : bands_) {
        bool still_configured = false;
        for (const ConfigSection* section : config.sections_with_prefix("band")) {
            if (section->name().substr(section->name().find(':') + 1) == band->id()) {
                still_configured = true;
                break;
            }
        }
        if (!still_configured) changed.push_back(band->id());
    }
    return changed;
}

std::vector<std::pair<std::string, std::string>> Radio::own_files() const {
    std::vector<std::pair<std::string, std::string>> files;
    if (!config_path_.empty()) files.push_back({"the configuration", config_path_});
    if (!overlay_path_.empty()) files.push_back({"the settings saved from the admin panel", overlay_path_});
    if (module_store_) files.push_back({"the module directory", module_store_->directory()});
    for (const auto& band : bands_) {
        files.push_back({"the waterfall archive of [band:" + band->id() + "]", band->history_path()});
    }
    return files;
}

void Radio::load_overlay(const std::string& path) {
    overlay_path_ = path;
    std::string text;
    if (!read_text_file(path, text)) return;
    Json values;
    std::string reason;
    if (!Json::parse(text, values, reason) || !values.is_object()) {
        LOG_WARN("radio", "%s cannot be read: %s; ignoring it", path.c_str(),
                 reason.empty() ? "it is not a JSON object" : reason.c_str());
        return;
    }
    // A setting the file has changed since the overlay was written was
    // edited later than the panel's value, so the file's stands. An overlay
    // written before it kept a record of the file wins as it always did.
    Json settings = Json::make_object();
    const Json& file_then = values["file"];
    for (const auto& [key, value] : values.members()) {
        if (file_then.is_object() && file_then.has(key) && file_record_.has(key) &&
            file_then[key].serialize() != file_record_[key].serialize()) {
            LOG_INFO("radio", "[site] %s changed in the config file since the panel saved it; using the file's",
                     key.c_str());
            continue;
        }
        settings.set(key, value);
    }
    // A setting that fails its check is skipped rather than costing the rest,
    // and the bands and mutes below load whatever became of it.
    std::string error;
    apply_site_values(settings, false, error);
    // Per-band overrides live in the same file, under the band's id. They are
    // kept rather than applied: this runs while the config is being read, and
    // the bands they refer to have not been built yet.
    if (values.has("bands") && values["bands"].is_object()) pending_bands_ = values["bands"];
    // Mutes are the one piece of moderation state there is, and losing it on a
    // restart would make it useless: a reboot is exactly what somebody being
    // muted waits for.
    if (values.has("muted")) chat_.load_mutes(values["muted"]);
    if (values["directory_id"].is_string() && values["directory_id"].string().size() == 32) {
        directory_id_ = values["directory_id"].string();
    }
    LOG_INFO("radio", "station details loaded from %s", path.c_str());
}

/**
 * The overlay file as it should be written: what is on disk, with everything
 * this build owns rewritten over the top.
 *
 * Both writers go through this. Before it, saving the station details wrote
 * `site_json()` alone, which silently deleted every per-band override in the
 * same file - change the receiver's name and the noise blanker settings went
 * with it. Carrying unknown keys through also means a file written by a newer
 * build survives being loaded by an older one.
 */
Json Radio::overlay_document() const {
    Json document = Json::make_object();
    std::string existing;
    if (!overlay_path_.empty() && read_text_file(overlay_path_, existing)) {
        Json parsed;
        if (Json::parse(existing, parsed) && parsed.is_object()) document = parsed;
    }
    // Held in a named copy: `for (... : site_json().members())` binds to a
    // reference into a temporary that is gone before the first iteration, and
    // the receiver dies on the second line of its own startup.
    const Json site = site_json();
    for (const auto& [key, value] : site.members()) document.set(key, value);
    document.set("file", file_record_);
    if (!directory_id_.empty()) document.set("directory_id", directory_id_);
    return document;
}

const std::string& Radio::directory_id() {
    if (directory_id_.empty()) {
        directory_id_ = random_hex(16);
        std::string error;
        if (!save_overlay(overlay_document(), error)) LOG_WARN("radio", "listing id not saved: %s", error.c_str());
    }
    return directory_id_;
}

bool Radio::save_overlay(const Json& document, std::string& error) const {
    if (overlay_path_.empty()) return true;
    if (overlay_held_) {
        error = "a backup was restored; restart FernSDR to take it before changing settings";
        return false;
    }
    return write_text_file(overlay_path_, document.serialize(), error, FileAccess::OwnerOnly);
}

Json Radio::site_json() const {
    Json out = Json::make_object();
    SiteInfo copy = site_;
    for_each_site_field(copy, [&out](const char* key, const std::string& value) {
        out.set(key, value);
    });
    out.set("max_users", static_cast<double>(site_.max_users));
    out.set("listener_timeout", static_cast<double>(site_.listener_timeout_minutes));
    out.set("sdr_list", site_.sdr_list);
    out.set("public_port", site_.public_port);
    return out;
}

bool Radio::apply_site_json(const Json& values, std::string& error) {
    return apply_site_values(values, true, error);
}

bool Radio::apply_site_values(const Json& values, bool from_panel, std::string& error) {
    if (!values.is_object()) {
        error = "expected an object";
        return false;
    }

    // Checked before anything is applied, so a rejected field cannot leave the
    // station half-renamed. Keys this does not own are ignored rather than
    // refused: the overlay also carries per-band settings, and a file written
    // by a newer build should not stop an older one from starting. The panel
    // sends every field, and only what it changes has to pass: a limit the
    // file sets outside the panel's range must not stop a new notice.
    const Json now = site_json();
    Json accepted = Json::make_object();
    for (const auto& [key, value] : values.members()) {
        if (!is_panel_key(key)) continue;
        if (from_panel && value.serialize() == now[key].serialize()) continue;
        std::string why;
        if (!valid_site_value(key, value, why)) {
            if (from_panel) {
                error = why;
                return false;
            }
            LOG_WARN("radio", "a saved setting was left out: %s", why.c_str());
            continue;
        }
        accepted.set(key, value);
    }
    if (from_panel && !accepted.members().empty() && !config_path_.empty()) {
        // What the file says now about the settings this save changes, so the
        // file wins at the next start only if it is edited after this. The
        // other settings keep their record, so a hand edit of one of them
        // still counts. Unreadable, the record stays as it was.
        Config file;
        std::string read_error;
        if (file.load(config_path_, read_error)) {
            const Json record = file_record(file);
            for (const auto& [key, value] : accepted.members()) file_record_.set(key, record[key]);
        }
    }

    for_each_site_field(site_, [&accepted](const char* key, std::string& field) {
        if (accepted.has(key)) field = accepted[key].string();
    });
    if (accepted.has("max_users")) site_.max_users = static_cast<int>(accepted["max_users"].number());
    if (accepted.has("listener_timeout")) {
        site_.listener_timeout_minutes = static_cast<int>(accepted["listener_timeout"].number());
    }
    if (accepted.has("sdr_list")) site_.sdr_list = accepted["sdr_list"].boolean();
    if (accepted.has("public_port")) site_.public_port = static_cast<int>(accepted["public_port"].number());

    std::string write_error;
    if (!save_overlay(overlay_document(), write_error)) {
        error = write_error;
        return false;
    }
    configure_spots();
    return true;
}

std::string Radio::spot_software() const {
    std::set<std::string> reporting;
    {
        std::lock_guard<std::mutex> lock(reporting_mutex_);
        reporting = reporting_;
    }
    std::set<std::string> parts;
    {
        std::lock_guard<std::mutex> lock(decoders_mutex_);
        for (const auto& decoder : decoders_) {
            if (!reporting.count(decoder->config().id)) continue;
            // The version the module said it is, and its name as installed.
            const std::string version = decoder->status()["version"].string();
            if (version.empty()) continue;
            std::string name = decoder->config().module;
            ModuleStore::Module module;
            if (module_store_ && module_store_->find(name, module)) {
                for (const auto& manifest : module.versions) {
                    if (manifest.version == module.active && !manifest.name.empty()) name = manifest.name;
                }
            }
            parts.insert(name + " " + version);
        }
    }
    std::string out = std::string("FernSDR ") + kVersion;
    for (const std::string& part : parts) out += " / " + part;
    return out;
}

void Radio::configure_spots() {
    {
        std::lock_guard<std::mutex> decoders(decoders_mutex_);
        std::lock_guard<std::mutex> lock(reporting_mutex_);
        reporting_.clear();
        for (const auto& c : decoder_configs_) {
            if (c.report) reporting_.insert(c.id);
        }
    }
    SpotReceiver receiver;
    receiver.callsign = site_.operator_name;
    receiver.locator = site_.grid_square;
    receiver.software = std::string("FernSDR ") + kVersion;
    receiver.antenna = site_.antenna;
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(reporting_mutex_);
        any = !reporting_.empty();
    }
    // host:port, the port optional.
    std::string host = spot_server_;
    uint16_t port = 4739;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        const long parsed = std::strtol(host.c_str() + colon + 1, nullptr, 10);
        if (parsed > 0 && parsed < 65536) port = static_cast<uint16_t>(parsed);
        host.resize(colon);
    }
    spots_.configure(receiver, any ? host : "", port);
}

Json Radio::bands_json() const {
    Json list = Json::make_array();
    for (const auto& band : bands_) {
        const BandInfo info = band->info();
        Json entry = Json::make_object();
        entry.set("id", info.id);
        entry.set("name", info.name);
        // Live-settable.
        entry.set("noise_blanker", std::round(info.noise_blanker * 100.0) / 100.0);
        entry.set("noise_floor", std::round(info.noise_floor_dbfs * 10.0) / 10.0);
        entry.set("calibration", band->calibration_text());
        entry.set("history", band->history_access());
        entry.set("history_hours", static_cast<double>(band->history_hours()));
        entry.set("history_bins", static_cast<double>(band->history_bins()));
        entry.set("history_interval", band->history_interval());
        // What it costs, so an operator reads the number before switching it
        // on rather than discovering it on a full disk later.
        entry.set("history_bytes", static_cast<double>(band->history_bytes()));
        entry.set("max_bandwidth", band->max_bandwidth_hz());
        entry.set("max_user_bitrate", static_cast<double>(band->max_user_bitrate()));
        entry.set("default_audio_bitrate", static_cast<double>(band->default_audio_bitrate()));
        entry.set("iq_swap", info.iq_swap);
        entry.set("dc_remove", info.dc_remove);
        entry.set("iq_balance", info.iq_balance);
        // What the corrections found, so the panel can say whether they are
        // doing anything worth having.
        Json input = Json::make_object();
        input.set("dc_offset_dbfs", std::round(info.dc_offset_dbfs * 10.0) / 10.0);
        input.set("gain_error_db", std::round(info.gain_error_db * 100.0) / 100.0);
        input.set("phase_error_degrees", std::round(info.phase_error_degrees * 100.0) / 100.0);
        input.set("image_rejection_db", std::round(info.image_rejection_db * 10.0) / 10.0);
        entry.set("input", input);
        // Wiring, shown so an operator can see it without opening a file, and
        // marked so the panel can grey it out rather than pretend.
        Json fixed = Json::make_object();
        fixed.set("low", info.low_hz);
        fixed.set("high", info.high_hz);
        fixed.set("center", info.center_hz);
        fixed.set("sample_rate", info.sample_rate);
        fixed.set("signal", info.signal);
        fixed.set("ppm", std::round(info.ppm * 1000.0) / 1000.0);
        fixed.set("frequency_offset", band->frequency_offset_hz());
        fixed.set("source", band->configured_values().count("source")
                                ? band->configured_values().at("source") : std::string("test"));
        fixed.set("running", info.running);
        fixed.set("listeners", info.listeners);
        entry.set("fixed", fixed);
        // The schedule: the hours as the file has them, and the bands that
        // take turns with this one on its input, so the panel can set them
        // together. `located` says whether sunrise and sunset are the
        // station's or the 06:00 and 18:00 UTC taken without a grid square.
        entry.set("hours", info.hours);
        entry.set("on_air", info.on_air);
        entry.set("next_change", static_cast<double>(info.next_change_ms));
        Json partners = Json::make_array();
        const std::string shared = shared_input(band.get());
        for (const auto& other : bands_) {
            if (other.get() == band.get() || shared.empty() || shared_input(other.get()) != shared) continue;
            Json partner = Json::make_object();
            partner.set("id", other->id());
            partner.set("name", other->name());
            partner.set("hours", other->info().hours);
            partners.push_back(partner);
        }
        entry.set("partners", partners);
        double lat = 0.0, lon = 0.0;
        entry.set("located", locator_centre(site_.grid_square, lat, lon));
        list.push_back(entry);
    }
    return list;
}

bool Radio::apply_band_json(const std::string& id, const Json& values, std::string& error) {
    Band* target = band(id);
    if (!target) {
        error = "no band called '" + id + "'";
        return false;
    }
    if (!apply_band_values(target, values, error)) return false;
    if (overlay_path_.empty()) return true;

    // Merged into the same overlay the station details use, under the band's
    // id, so one file holds everything the panel can change.
    Json overlay = overlay_document();
    Json bands_overlay = overlay.has("bands") ? overlay["bands"] : Json::make_object();
    Json entry = bands_overlay.has(id) ? bands_overlay[id] : Json::make_object();
    for (const auto& [key, value] : values.members()) entry.set(key, value);
    bands_overlay.set(id, entry);
    overlay.set("bands", bands_overlay);
    return save_overlay(overlay, error);
}

bool Radio::apply_band_values(Band* target, const Json& values, std::string& error) {
    // Every bound checked before anything is applied: a rejected field must
    // not leave the band half-configured.
    if (values.has("name") && (!values["name"].is_string() || values["name"].string().empty() ||
                               values["name"].string().size() > 80)) {
        error = "the band name must be between 1 and 80 characters";
        return false;
    }
    const double blanker = values["noise_blanker"].number(0.0);
    if (values.has("noise_blanker") && (blanker < 0.0 || blanker > 1.0)) {
        error = "the noise blanker must be between 0 and 1";
        return false;
    }
    const double bandwidth = values["max_bandwidth"].number(0.0);
    if (values.has("max_bandwidth") && (bandwidth < 500.0 || bandwidth > target->sample_rate())) {
        error = "the widest passband must be between 500 Hz and the band's own width";
        return false;
    }
    const double bitrate = values["max_user_bitrate"].number(0.0);
    if (values.has("max_user_bitrate") && (bitrate < 8000.0 || bitrate > 2000000.0)) {
        error = "the per-listener ceiling must be between 8 kbit/s and 2 Mbit/s";
        return false;
    }
    const double audio = values["default_audio_bitrate"].number(0.0);
    if (values.has("default_audio_bitrate") && (audio < 8000.0 || audio > 128000.0)) {
        error = "the default audio bitrate must be between 8 and 128 kbit/s";
        return false;
    }
    for (const char* key : {"iq_swap", "dc_remove", "iq_balance"}) {
        if (values.has(key) && !values[key].is_bool()) {
            error = std::string(key) + " must be true or false";
            return false;
        }
    }
    if ((values["iq_swap"].boolean(false) || values["iq_balance"].boolean(false)) &&
        target->signal_kind() == SignalKind::Real) {
        error = "a real input has no I and Q to swap or balance";
        return false;
    }

    if (values.has("history") || values.has("history_hours") || values.has("history_bins") ||
        values.has("history_interval")) {
        const std::string access =
            values.has("history") ? values["history"].string() : target->history_access();
        const int hours = static_cast<int>(
            values["history_hours"].number(static_cast<double>(target->history_hours())));
        const size_t width = static_cast<size_t>(
            values["history_bins"].number(static_cast<double>(target->history_bins())));
        const double interval = values["history_interval"].number(target->history_interval());
        std::string problem;
        if (!target->set_history(access, hours, width, interval, problem)) {
            error = problem;
            return false;
        }
    }

    if (values.has("calibration")) {
        std::string problem;
        if (!target->set_calibration(values["calibration"].string(), problem)) {
            error = problem;
            return false;
        }
    }

    if (values.has("name")) target->set_display_name(values["name"].string());
    if (values.has("noise_blanker")) target->set_noise_blanker(static_cast<float>(blanker));
    if (values.has("iq_swap")) target->set_iq_swap(values["iq_swap"].boolean(false));
    if (values.has("dc_remove")) target->set_dc_remove(values["dc_remove"].boolean(false));
    if (values.has("iq_balance")) target->set_iq_balance(values["iq_balance"].boolean(false));
    if (values.has("max_bandwidth")) target->set_max_bandwidth_hz(bandwidth);
    if (values.has("max_user_bitrate")) target->set_max_user_bitrate(static_cast<int>(bitrate));
    if (values.has("default_audio_bitrate")) {
        target->set_default_audio_bitrate(static_cast<int>(audio));
    }
    return true;
}

bool Radio::save_mutes(int64_t now_ms, std::string& error) {
    Json overlay = overlay_document();
    overlay.set("muted", chat_.mutes_json(now_ms));
    return save_overlay(overlay, error);
}

}  // namespace fernsdr

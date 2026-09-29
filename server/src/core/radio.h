// The receiver as a whole: site metadata plus the configured bands.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../util/config.h"
#include "band.h"
#include "band_hours.h"
#include "chat.h"
#include "decoder.h"
#include "spots.h"
#include "modules.h"
#include "theme.h"

namespace fernsdr {

// What a save of the configuration did to the decoders: the ids started or
// stopped, and those left as they were until a restart, with why.
struct DecoderChanges {
    std::vector<std::string> changed;
    std::map<std::string, std::string> waiting;
};

// Everything the client shows about where it is listening from.  Operators
// care about this being right; users orient themselves by it.
struct SiteInfo {
    std::string name = "FernSDR";
    std::string operator_name;
    std::string location;
    std::string grid_square;
    std::string antenna;
    std::string contact;
    std::string website;
    std::string notice;  // free-text banner, e.g. maintenance windows
    // Which band plan listeners see drawn over the waterfall: an ITU region
    // ("r1".."r3"), a country ("us", "gb", ...), "none", or "auto", which the
    // page works out from the grid square. The plans themselves ship with
    // the page; the receiver only says which one.
    std::string band_plan = "auto";
    // AGPL section 13: everyone using this over a network is entitled to the
    // source it is running, so the client shows this link. An operator who has
    // modified FernSDR points it at their own repository.
    std::string source_url;
    int max_users = 200;
    // Minutes a listener may go without touching anything before the
    // receiver lets their place go; 0 for never. A minute before, the page
    // asks whether they are still listening. For a busy receiver whose slots
    // fill with tabs nobody is listening to any more.
    int listener_timeout_minutes = 0;
    // Per IPv4 address or IPv6 /64; 0 for no limit.
    int max_users_per_address = 16;
    // Whether listeners may chat at all. Off, nothing is accepted, kept or sent.
    bool chat = true;
    // Listed on sdr-list.xyz under this public address; see DirectoryListing.
    bool sdr_list = false;
    std::string public_host;
    // The port listeners use, 0 for the one the receiver listens on.
    int public_port = 0;
};

class Radio {
public:
    ~Radio() { stop(); }
    // Builds the bands described by the config.  Returns false with a
    // human-readable error if any band cannot be created.
    bool configure(const Config& config, std::string& error);

    bool start(std::string& error);
    void stop();

    const SiteInfo& site() const { return site_; }
    /** Where the panel's settings are kept beside the configuration. */
    const std::string& overlay_path() const { return overlay_path_; }
    // After a backup was played back into the overlay file, this receiver's
    // settings are the old ones until it restarts, and writing them would
    // undo the backup; from here every save is refused with the reason.
    void hold_overlay() { overlay_held_ = true; }

    // The operator's colours, background and widgets. Owned here because it
    // outlives any one session and every session needs it.
    ChatRoom& chat() { return chat_; }

    ThemeStore& theme() { return theme_; }
    const ThemeStore& theme() const { return theme_; }

    // Applies the [site] section of an already-validated config that has just
    // been written to a running receiver. Of the settings the panel also
    // edits, only those the file now says differently from what it said last
    // time are taken: the rest of the file may still hold values the panel
    // has since replaced, and a hand edit made since the start counts as an
    // edit here too. Band changes are
    // deliberately NOT applied: a band owns a thread and every listener
    // attached to it holds a pointer to it, so swapping one under them is a
    // much larger piece of work than it looks, and doing it badly would cost
    // people their audio. The caller reports which bands differ so an
    // operator can restart those.
    void apply_site(const Config& after);

    // The station details an operator edits from the admin panel, kept in a
    // JSON overlay beside the config rather than in it. The config file is
    // the receiver's wiring, hand-edited and full of comments; rewriting it
    // to change a notice would destroy those comments, and asking an
    // operator to edit it to change their callsign is why the panel exists.
    //
    // Both can set the same setting, and the later change wins: the overlay
    // remembers what the file said when the overlay was last written, and a
    // setting the file has changed since is taken from the file.
    void load_overlay(const std::string& path);
    Json site_json() const;
    /** The overlay as it should be written: the file on disk plus what we own. */
    Json overlay_document() const;
    bool save_overlay(const Json& document, std::string& error) const;
    /** Writes the chat mute list into the overlay so it survives a restart. */
    bool save_mutes(int64_t now_ms, std::string& error);
    bool apply_site_json(const Json& values, std::string& error);

    /**
     * The id this receiver is listed under in directories: drawn at random the
     * first time it is needed and kept with the saved settings, so a restart
     * or a new address does not make a second entry.
     */
    const std::string& directory_id();

    // Per-band settings the panel can change while the receiver is running.
    // Wiring - source, sample rate, transform size - is not here: those are
    // fixed when a band is built and changing them means rebuilding it.
    Json bands_json() const;
    bool apply_band_json(const std::string& id, const Json& values, std::string& error);

    // Band ids whose configuration in `config` differs from what is running.
    std::vector<std::string> bands_needing_restart(const Config& config) const;

    /** One setting that a restart would change, and what it would change to. */
    struct BandChange {
        std::string band;
        std::string key;
        std::string running;    // what the band was built with, or "" if new
        std::string configured; // what the file now says, or "" if removed
        // True when restarting the band applies it; otherwise it needs the
        // whole receiver restarted, because the band's transforms depend on it.
        bool band_restart = false;
    };

    /**
     * The settings a restart would actually apply, per band.
     *
     * `bands_needing_restart` says which bands changed; this says what
     * changed in them. Restarting a band drops everyone listening to it, so
     * an operator deserves to see whether that is worth it before they do it.
     */
    std::vector<BandChange> band_changes(const Config& config) const;
    const std::vector<std::unique_ptr<Band>>& bands() const { return bands_; }

    // Nullptr when the id is unknown.
    Band* band(const std::string& id);
    // The first band on the air, or the first of all when none is.
    Band* default_band();

    /**
     * The schedule. A band's `hours` (see BandHours) say when it is on the
     * air; several bands may share one input when their hours do not
     * overlap, and the one whose hours it is has it.
     *
     * apply_schedule() brings the bands in line with their hours at
     * `utc_ms`: it marks each on or off the air at once, so sessions can
     * follow, and has those going off stopped and then those coming on
     * started, on a thread of its own once start() has run (a stop waits for
     * the input to let go, which can take seconds) and in the call before.
     * It acts only where a band's state changes, so a band that stopped by
     * itself is left for the operator. Returns the bands that changed. Runs
     * on the server's thread, as the site settings it reads are changed
     * there.
     */
    std::vector<Band*> apply_schedule(int64_t utc_ms);
    // Waits until the stops and starts asked for so far are done; for tests.
    void wait_for_schedule();
    // The band on the air on the same input as `band`, or null.
    Band* successor(const Band* band);
    // For a band sharing its input with others by their hours, the id of the
    // first of them, the same for all; empty for a band with its own.
    std::string shared_input(const Band* band) const;
    // Takes each band's hours from `config`, which a probe Radio has
    // accepted: they apply without a restart.
    void apply_hours(const Config& config);

    std::vector<BandInfo> band_info() const;
    int total_listeners() const;

    // Wired to the server's wake so audio is flushed as soon as it exists.
    void set_wake_callback(const std::function<void()>& callback);

    /**
     * Installed modules and the jobs that change them. Null until start():
     * a Radio built only to check a configuration never starts the thread
     * that runs them.
     */
    ModuleManager* modules() { return modules_.get(); }
    const std::shared_ptr<ModuleStore>& module_store() const { return module_store_; }
    /**
     * The files this receiver keeps for itself, each with what it is: the
     * configuration, the settings saved beside it, the module directory and
     * every band's waterfall archive, on or not, since the panel can turn one
     * on. None of them may be served, which startup checks.
     */
    std::vector<std::pair<std::string, std::string>> own_files() const;
    /** The bands configured with a module, by the module's id. */
    std::vector<std::string> bands_using_module(const std::string& id) const;
    /** Restarts every band and decoder that runs module `id`, so it picks up a change. */
    void restart_module_bands(const std::string& id);

    /** What the decoders heard, for the pages. */
    DecodeStore& decodes() { return decodes_; }
    std::vector<DecoderConfig> decoder_configs() const;
    // Spot reporting's state, for the admin panel.
    Json spots_status() const { return spots_.status(); }
    SpotReporter& spot_reporter() { return spots_; }
    // "FernSDR 0.1.0 / Fern-FT8 0.1.0": the receiver and the modules that
    // decode what is reported, as they run now.
    std::string spot_software() const;
    // For the admin panel: each running decoder's status, with its section's
    // `public`.
    Json decoders_status() const;
    // Ends decoder `id`'s session so its module starts afresh; false when no
    // decoder has that id.
    bool restart_decoder(const std::string& id);
    // The decoders listeners may see (public = yes) and the channels each
    // listens to, for the welcome and station messages.
    Json listed_decoders_json() const;
    bool decoder_listed(const std::string& id) const;
    // The public decoders, each with the number of the last decode made
    // before it became public: listeners see only the decodes after it.
    std::map<std::string, uint64_t> public_decoders() const;
    /**
     * Brings the decoders in line with `config`'s [decoder:...] sections, which
     * the caller has had a probe Radio accept: stops those removed or changed,
     * starts those added or changed, and leaves the rest running. A change of
     * `public` alone restarts nothing and always applies. A decoder with a
     * channel on a band that is not running yet, or not as wide yet, keeps
     * its present form until a restart and is reported as waiting.
     */
    DecoderChanges apply_decoders(const Config& config);
    // Threads still held for stopping retired decoders, for tests.
    size_t decoder_stoppers() const {
        std::lock_guard<std::mutex> lock(stoppers_mutex_);
        return stoppers_.size();
    }
    /**
     * The program decoders are started through, for their sandbox: this
     * receiver's own executable. Tests, whose binary is not the receiver,
     * point it at one that is.
     */
    void set_decoder_launcher(std::string path) { decoder_launcher_ = std::move(path); }

private:
    // Checks and applies `values` to a band without saving them: the part of
    // apply_band_json that startup shares, applying what was saved before.
    bool apply_band_values(Band* target, const Json& values, std::string& error);
    // Checks and applies station settings. From the panel, a change that fails
    // a check refuses the whole save; from the saved settings at startup, the
    // failing setting is skipped and the rest still load.
    bool apply_site_values(const Json& values, bool from_panel, std::string& error);
    using DecoderSettings = std::map<std::string, std::map<std::string, std::string>>;
    // A decoder whose channels the running bands cannot take, when parsing
    // leniently for apply_decoders.
    struct UnplacedDecoder {
        bool listed = false;
        bool report = false;
        std::string reason;
    };
    bool parse_decoders(const Config& config, std::vector<DecoderConfig>& configs, DecoderSettings& settings,
                        std::string& error, std::map<std::string, UnplacedDecoder>* unplaced = nullptr);
    bool configure_decoders(const Config& config, std::string& error);
    void start_decoders();
    std::unique_ptr<Decoder> make_decoder(const DecoderConfig& config, const std::map<std::string, std::string>& raw);

    // The schedule's, per band in bands_ order: the hours, the band that
    // stands for the input it shares (itself when it has one of its own),
    // and when its state next changes, cached until then or until the
    // hours or the station's place change.
    std::vector<BandHours> hours_;
    std::vector<size_t> input_of_;
    std::vector<int64_t> next_change_;
    std::string schedule_key_;
    bool schedule_known_ = false;
    void run_schedule();
    void carry_out(std::vector<std::pair<Band*, bool>> jobs);
    std::mutex schedule_mutex_;
    std::condition_variable schedule_wake_;
    std::condition_variable schedule_idle_;
    std::vector<std::pair<Band*, bool>> schedule_jobs_;
    bool schedule_busy_ = false;
    bool schedule_stopping_ = false;
    std::thread schedule_thread_;

    SiteInfo site_;
    ThemeStore theme_;
    ChatRoom chat_;
    std::string config_path_;
    std::string overlay_path_;
    std::atomic<bool> overlay_held_{false};
    // What the config file said about the panel's settings when the overlay's
    // values were last reconciled with it: at startup, when the config editor
    // writes it, and for a setting the panel changes. Kept here rather than
    // read from disk at each save, or a mute saved after a hand edit would
    // mark the edit as seen and the next start would drop it.
    Json file_record_;
    std::string directory_id_;
    // Band settings read from the overlay before the bands existed.
    Json pending_bands_;
    std::unique_ptr<DspWorkers> dsp_workers_;
    std::shared_ptr<ModuleStore> module_store_;
    std::vector<std::string> module_catalog_;
    // Fixed at configure(): a band's module cannot change without rebuilding it.
    std::map<std::string, std::vector<std::string>> module_bands_;
    // "<module>\n<module.device>" to the band that has it, while configuring.
    std::map<std::string, std::string> module_devices_;
    std::vector<std::unique_ptr<Band>> bands_;
    // After bands_, so they are stopped first: their channels are on bands.
    DecodeStore decodes_;
    // Guards the three below: the panel changes them on the server's thread
    // while a module job restarts decoders from its own.
    mutable std::mutex decoders_mutex_;
    std::vector<DecoderConfig> decoder_configs_;
    // The operator's `module.<key>` text per decoder, typed when it starts.
    DecoderSettings decoder_settings_;
    std::vector<std::unique_ptr<Decoder>> decoders_;
    std::map<std::string, uint64_t> public_from_;
    // Who reports spots, and where: the station from [site], the decoders
    // with report = pskreporter. Set again whenever either changes.
    void configure_spots();

    std::string spot_server_ = "report.pskreporter.info:4739";
    // The decoders whose decodes are reported. A lock of its own: the decode
    // callback runs on a decoder's thread, which stop() waits for while it
    // holds decoders_mutex_.
    mutable std::mutex reporting_mutex_;
    std::set<std::string> reporting_;
    SpotReporter spots_;
    // Threads stopping decoders a change retired. Those done are joined
    // before the next starts, so repeated saves do not pile up stacks (a
    // 32-bit build runs out of address space after a few hundred); stop()
    // joins the rest.
    struct Stopper {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    mutable std::mutex stoppers_mutex_;
    std::vector<Stopper> stoppers_;
    std::string decoder_launcher_;
    // After bands_, so it is destroyed first: its jobs restart bands.
    std::unique_ptr<ModuleManager> modules_;
};

}  // namespace fernsdr

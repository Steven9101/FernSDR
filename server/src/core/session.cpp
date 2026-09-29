#include "session.h"
#include "protocol.h"

#include <chrono>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../dsp/ctcss.h"
#include "../net/server.h"
#include "../util/log.h"

namespace fernsdr {

namespace {

std::atomic<uint64_t>& listener_id_counter() {
    static std::atomic<uint64_t> counter{1};
    return counter;
}

}  // namespace

namespace {

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Session::Session(uint64_t id, Radio& radio)
    : id_(id), radio_(radio), connected_at_ms_(steady_ms()), last_activity_ms_(connected_at_ms_) {}

Session::Inactivity Session::check_inactivity(int64_t limit_ms, int64_t now_ms) {
    if (limit_ms <= 0 || inactivity_expired_) return Inactivity::None;
    const int64_t idle = now_ms - last_activity_ms_;
    if (idle >= limit_ms) {
        // Once: the connection closes after its queue drains, and later ticks
        // must not close or log it again.
        inactivity_expired_ = true;
        return Inactivity::Expired;
    }
    const int64_t left = limit_ms - idle;
    // A minute's warning, or half the limit when that is shorter, so an
    // answer is not followed at once by the next question.
    if (left <= std::min(kInactivityWarningMs, limit_ms / 2) && !inactivity_warned_) {
        inactivity_warned_ = true;
        Json warning = Json::make_object();
        warning.set("type", "inactivity");
        warning.set("seconds", static_cast<double>((left + 999) / 1000));
        queue_text(warning.serialize());
    }
    return Inactivity::None;
}

void Session::note_activity() {
    last_activity_ms_ = steady_ms();
    inactivity_warned_ = false;
}

Session::Summary Session::summarise() const {
    Summary summary;
    summary.id = id_;
    summary.band = band_id_;
    summary.frequency_hz = channel_.frequency_hz;
    summary.mode = mode_name(channel_.mode);
    summary.bandwidth_hz = channel_.bandwidth_high - channel_.bandwidth_low;
    summary.connected_seconds = (steady_ms() - connected_at_ms_) / 1000;
    if (listener_) {
        const ListenerTelemetry telemetry = listener_->telemetry();
        summary.audio_bitrate = telemetry.audio_bitrate;
        summary.waterfall_bitrate = telemetry.waterfall_bitrate;
    }
    return summary;
}

Session::~Session() { detach(); }

void Session::begin() {
    send_welcome();
    attach_to(radio_.default_band());
}

void Session::detach() {
    if (band_ && listener_) band_->remove_listener(listener_->id());
    listener_.reset();
    band_ = nullptr;
}

void Session::follow_schedule() {
    if (!band_ || band_->on_air()) return;
    const std::string note = band_->name() + " is " + band_->status();
    if (Band* next = radio_.successor(band_)) {
        attach_to(next);
        send_state(note + "; now on " + next->name());
    } else {
        send_state(note);
    }
}

void Session::attach_to(Band* band) {
    if (!band) {
        send_error("this receiver has no bands configured");
        return;
    }
    // Off the air by its hours, a band's input belongs to the band whose
    // hours it is; a page reopened with the other remembered goes there.
    if (!band->on_air()) {
        if (Band* next = radio_.successor(band)) band = next;
    }
    if (band == band_) return;

    const uint8_t generation_seed = listener_ ? listener_->audio_generation() : 0;
    detach();
    band_ = band;
    band_id_ = band->id();

    listener_ = std::make_shared<Listener>(listener_id_counter()++, *band, generation_seed);
    listener_->set_bitrate_budget(band->max_user_bitrate());
    // A new listener counts its RDS from the start again: whatever it
    // reports first goes out, even at a count the last one had reached.
    rds_sent_sequence_ = UINT64_MAX;
    rds_sent_station_ = UINT64_MAX;

    // A listener who has not chosen a bitrate takes the band's default. One
    // who has keeps their choice across a band change: it is their connection
    // that is being budgeted, and they know more about it than we do.
    if (!bitrate_chosen_) channel_.audio_bitrate = band->default_audio_bitrate();

    // Start in the middle of the new band unless the user is already tuned
    // inside it, so switching bands lands somewhere sensible.
    if (channel_.frequency_hz < band->low_hz() || channel_.frequency_hz > band->high_hz()) {
        channel_.frequency_hz = band->center_hz();
    }
    viewport_.low_hz = band->low_hz();
    viewport_.high_hz = band->high_hz();

    apply_channel("");
    sanitise(viewport_, band->low_hz(), band->high_hz(), 4096, 30.0);
    listener_->set_viewport(viewport_);

    band->add_listener(listener_);
    send_state("");
}

void Session::apply_channel(const std::string& note_prefix) {
    if (!band_ || !listener_) return;
    std::string fallback;
    // Onto a band that cannot carry broadcast FM, a listener in it goes on
    // in narrow FM rather than hearing nothing.
    if (channel_.mode == Mode::Wfm && !band_->wfm()) {
        channel_.mode = Mode::Nfm;
        apply_mode_defaults(channel_);
        fallback = "this band has no broadcast FM; narrow FM instead";
    }
    std::string note = sanitise(channel_, band_->low_hz(), band_->high_hz(), band_->max_bandwidth_hz());
    if (!fallback.empty()) note = fallback;
    // Broadcast FM picks the channel its passband needs, up to the band.
    const double half_rate = channel_.mode == Mode::Wfm
        ? band_->sample_rate() / 2
        : channel_audio_rate(band_->sample_rate(), channel_.requested_audio_rate, band_->fft_size()) / 2;
    const double minimum = std::max(-half_rate, band_->sample_low_hz() - channel_.frequency_hz);
    const double maximum = std::min(half_rate, band_->sample_high_hz() - channel_.frequency_hz);
    const double minimum_width = std::min(50.0, std::max(0.0, maximum - minimum));
    const double low = maximum > minimum ? std::clamp(channel_.bandwidth_low, minimum, maximum - minimum_width) : 0;
    const double high = maximum > minimum ? std::clamp(channel_.bandwidth_high, low + minimum_width, maximum) : 0;
    if (low != channel_.bandwidth_low || high != channel_.bandwidth_high) {
        channel_.bandwidth_low = low;
        channel_.bandwidth_high = high;
        note = "passband clamped to the sampled channel";
    }
    listener_->set_channel(channel_);
    if (!note.empty()) send_state(note_prefix.empty() ? note : note_prefix + ": " + note);
}

void Session::handle_text(const std::string& text) {
    Json message;
    if (!Json::parse(text, message) || !message.is_object()) {
        send_error("could not parse that message as a JSON object");
        return;
    }

    const std::string type = message["type"].string();
    const double request_id = message["request_id"].number(0);
    if (request_id >= 0 && request_id <= 9007199254740991.0 && std::floor(request_id) == request_id) {
        if (type == "tune") tune_request_id_ = request_id;
        else if (type == "viewport") viewport_request_id_ = request_id;
        else if (type == "dsp") dsp_request_id_ = request_id;
    }
    // What a person does, as opposed to what the page does by itself: pings,
    // state requests and a viewport resized with the window do not count.
    if (type == "tune" || type == "dsp" || type == "audio" || type == "chat" || type == "active") note_activity();
    if (type == "hello") {
        const Json& capabilities = message["capabilities"];
        binary_meter_ = false;
        ctcss_meter_ = false;
        channel_.compact_audio = false;
        channel_.packet_audio = false;
        delivery_.set_audio_discontinuity(false);
        for (size_t i = 0; i < capabilities.size() && i < 16; i++) {
            if (capabilities[i].string() == "meter-v1") binary_meter_ = true;
            if (capabilities[i].string() == "meter-ctcss") ctcss_meter_ = true;
            if (capabilities[i].string() == "nac2") channel_.compact_audio = true;
            if (capabilities[i].string() == "nac3") channel_.packet_audio = true;
            if (capabilities[i].string() == "audio-discontinuity") delivery_.set_audio_discontinuity(true);
        }
        if (listener_) listener_->set_channel(channel_);
        if (!binary_meter_) pending_meter_.clear();
    } else if (type == "tune") {
        handle_tune(message);
    } else if (type == "viewport") {
        handle_viewport(message);
    } else if (type == "audio") {
        handle_audio(message);
    } else if (type == "dsp") {
        handle_dsp(message);
    } else if (type == "chat") {
        handle_chat(message);
    } else if (type == "ping") {
        Json reply = Json::make_object();
        reply.set("type", "pong");
        reply.set("t", message["t"].number(0.0));
        queue_text(reply.serialize());
    } else if (type == "state") {
        send_state("");
    } else if (type == "active") {
        // The listener answered "still listening?"; note_activity() above.
    } else if (type.empty()) {
        send_error("message has no 'type'");
    } else {
        send_error("unknown message type '" + type + "'");
    }
}

void Session::handle_tune(const Json& message) {
    std::string schedule_note;
    if (message.has("band")) {
        const std::string id = message["band"].string();
        Band* target = radio_.band(id);
        if (!target) {
            send_error("no band called '" + id + "'");
            return;
        }
        // A band off the air is not refused: every tune names a band, and a
        // page that asked for one just before it went off would otherwise
        // have each tune after refused too. The tune goes to the band on its
        // input, or stays where it is, and the state says which.
        if (!target->on_air()) {
            schedule_note = target->name() + " is " + target->status();
            Band* next = radio_.successor(target);
            if (next) schedule_note += "; now on " + next->name();
            target = next ? next : (band_ ? band_ : target);
        }
        if (target != band_) {
            attach_to(target);
            if (!band_) return;
        }
    }
    if (!band_ || !listener_) return;

    bool mode_changed = false;
    if (message.has("mode")) {
        Mode mode;
        if (!mode_from_name(message["mode"].string(), mode)) {
            send_error("unknown mode '" + message["mode"].string() + "'");
            return;
        }
        // A page moving to another band asks for the mode it had; where that
        // was broadcast FM and the new band has none, apply_channel() goes on
        // in narrow FM and says so.
        mode_changed = mode != channel_.mode;
        channel_.mode = mode;
    }
    if (message.has("cw_pitch")) channel_.cw_pitch_hz = message["cw_pitch"].number(channel_.cw_pitch_hz);

    // A mode change without an explicit passband adopts that mode's default;
    // otherwise switching from AM to CW would leave a 9 kHz CW filter.
    const bool explicit_passband = message.has("low") || message.has("high");
    if (mode_changed && !explicit_passband) apply_mode_defaults(channel_);

    // The frequency was meant for the band asked for; on another it would
    // be clamped to an edge, so the band's own place stands.
    if (message.has("freq") && schedule_note.empty()) channel_.frequency_hz = message["freq"].number(channel_.frequency_hz);
    if (message.has("low")) channel_.bandwidth_low = message["low"].number(channel_.bandwidth_low);
    if (message.has("high")) channel_.bandwidth_high = message["high"].number(channel_.bandwidth_high);

    apply_channel("");
    send_state(schedule_note);
}

void Session::handle_viewport(const Json& message) {
    if (!band_ || !listener_) return;
    if (message.has("codec")) {
        const auto codec = message["codec"].string();
        viewport_.range_coded = codec == "wfc5";
        viewport_.adaptive_codec = codec == "wfc3" || codec == "wfc4" || viewport_.range_coded;
        viewport_.native_grid = viewport_.adaptive_codec;
        viewport_.zero_runs = codec == "wfc2" || viewport_.adaptive_codec;
        viewport_.step_db = (codec == "wfc4" || codec == "wfc5") && message["step_db"].number() == 2 ? 2 : 1;
    }

    if (message.has("enabled")) viewport_.enabled = message["enabled"].boolean(viewport_.enabled);
    // Panning or zooming the waterfall is someone at the page; a width change
    // alone is the window being resized.
    if ((message.has("low") && message["low"].number(viewport_.low_hz) != viewport_.low_hz) ||
        (message.has("high") && message["high"].number(viewport_.high_hz) != viewport_.high_hz)) {
        note_activity();
    }
    if (message.has("low")) viewport_.low_hz = message["low"].number(viewport_.low_hz);
    if (message.has("high")) viewport_.high_hz = message["high"].number(viewport_.high_hz);
    if (message.has("width")) viewport_.width = static_cast<int>(std::clamp(message["width"].number(viewport_.width), 0.0, 65535.0));
    if (message.has("fps")) viewport_.lines_per_second = message["fps"].number(viewport_.lines_per_second);

    const std::string note = sanitise(viewport_, band_->low_hz(), band_->high_hz(), 4096, 30.0);
    listener_->set_viewport(viewport_);
    if (!note.empty()) send_state(note);
}

void Session::handle_chat(const Json& message) {
    // "history" is a client asking for the backlog it missed, which happens
    // when the operator turns the chat widget on while people are listening.
    if (message["history"].boolean(false)) {
        Json out = Json::make_object();
        out.set("type", "chat-history");
        Json list = Json::make_array();
        for (const ChatMessage& entry : radio_.chat().history()) {
            Json item = Json::make_object();
            item.set("id", static_cast<double>(entry.id));
            item.set("name", entry.name);
            item.set("text", entry.text);
            item.set("at", static_cast<double>(entry.at_ms));
            list.push_back(item);
        }
        out.set("messages", list);
        queue_text(out.serialize());
        return;
    }

    if (message.has("name")) chat_name_ = clean_chat_text(message["name"].string(), 24);

    ChatMessage posted;
    std::string error;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    if (!radio_.chat().post(id_, address_, chat_name_, message["text"].string(), now, posted,
                            error)) {
        // Not an error message: a refused chat line is a normal thing that
        // happens to people typing too fast, and it should not look like the
        // receiver broke.
        Json out = Json::make_object();
        out.set("type", "chat-refused");
        out.set("reason", error);
        queue_text(out.serialize());
        return;
    }
    pending_chat_ = posted;
    has_pending_chat_ = true;
}

void Session::handle_audio(const Json& message) {
    if (!listener_) return;
    if (message.has("enabled")) channel_.audio_enabled = message["enabled"].boolean(channel_.audio_enabled);
    if (message.has("rate")) {
        channel_.requested_audio_rate = static_cast<int>(std::clamp(message["rate"].number(channel_.requested_audio_rate), 0.0, 192000.0));
    }
    if (message.has("bitrate")) {
        channel_.audio_bitrate = static_cast<int>(std::clamp(message["bitrate"].number(channel_.audio_bitrate), 0.0, 2000000.0));
        bitrate_chosen_ = true;
    }
    if (message.has("frames")) {
        channel_.audio_packet_frames = static_cast<int>(std::clamp(message["frames"].number(1), 1.0, 4.0));
    }
    if (message.has("noise_margin")) {
        channel_.audio_noise_margin_db = static_cast<float>(message["noise_margin"].number(12.0));
    }
    apply_channel("");
    send_state("");
}

void Session::handle_dsp(const Json& message) {
    if (!listener_) return;

    if (message.has("agc")) {
        // An unknown name, from a newer or older page, costs only itself: the
        // rest of the message is still applied.
        AgcProfile profile;
        if (agc_profile_from_name(message["agc"].string(), profile)) channel_.agc = profile;
        else send_error("unknown AGC profile '" + message["agc"].string() + "'");
    }
    if (message.has("gain")) channel_.manual_gain_db = static_cast<float>(message["gain"].number(0.0));
    if (message.has("max_gain")) {
        channel_.max_gain_db = static_cast<float>(message["max_gain"].number(channel_.max_gain_db));
    }
    if (message.has("nr")) channel_.noise_reduction = static_cast<float>(message["nr"].number(0.0));
    if (message.has("autonotch")) channel_.auto_notch = message["autonotch"].boolean(false);
    if (message.has("volume")) channel_.volume = static_cast<float>(message["volume"].number(1.0));
    if (message.has("squelch")) channel_.squelch_dbfs = message["squelch"].number(channel_.squelch_dbfs);
    if (message.has("auto_squelch")) channel_.auto_squelch = message["auto_squelch"].boolean(channel_.auto_squelch);
    if (message.has("highpass")) {
        const double hz = message["highpass"].number(0.0);
        channel_.highpass_hz = static_cast<float>(std::isfinite(hz) ? std::clamp(hz, 0.0, 1000.0) : 0.0);
    }
    if (message.has("ctcss_squelch")) {
        // A standard tone or nothing: anything else would keep the audio
        // muted for good without saying why.
        const double hz = message["ctcss_squelch"].number(0.0);
        const auto& tones = CtcssDetector::standard_tones();
        channel_.ctcss_squelch = std::find(tones.begin(), tones.end(), hz) != tones.end() ? hz : 0.0;
    }
    if (message.has("ctcss_filter")) channel_.ctcss_filter = message["ctcss_filter"].boolean(channel_.ctcss_filter);
    if (message.has("deemphasis")) {
        const double us = message["deemphasis"].number(300.0);
        channel_.deemphasis_us = static_cast<float>(std::isfinite(us) ? std::clamp(us, 0.0, 2000.0) : 300.0);
    }
    if (message.has("wfm_deemphasis")) {
        const double us = message["wfm_deemphasis"].number(50.0);
        channel_.wfm_deemphasis_us = static_cast<float>(std::isfinite(us) ? std::clamp(us, 0.0, 200.0) : 50.0);
    }

    if (message.has("notches")) {
        const Json& list = message["notches"];
        std::vector<Notch> notches;
        for (size_t i = 0; i < list.size() && notches.size() < 8; i++) {
            Notch notch;
            notch.center_hz = list[i]["hz"].number(0.0);
            notch.width_hz = list[i]["width"].number(150.0);
            if (notch.center_hz > 0.0 && notch.width_hz > 0.0) notches.push_back(notch);
        }
        channel_.notches = notches;
    }

    apply_channel("");
    send_state("");
}

void Session::queue_text(const std::string& text) { pending_texts_.push_back(text); }

void Session::collect(std::vector<std::string>& texts, std::vector<std::vector<uint8_t>>& binaries) {
    if (state_due_ && steady_ms() - last_state_ms_ >= kStateSpacingMs) flush_state();
    for (auto& text : pending_texts_) texts.push_back(std::move(text));
    pending_texts_.clear();
    if (!listener_) {
        if (!pending_meter_.empty()) binaries.push_back(std::move(pending_meter_));
        pending_meter_.clear();
        return;
    }
    Listener::AudioFormat format;
    const size_t before = binaries.size();
    listener_->drain(binaries, &format);
    // A meter reading goes with audio, in the same write, unless it has
    // waited a fifth of a second for some: a reading that late is still news,
    // and one sent alone costs a TCP segment of its own.
    if (!pending_meter_.empty()) {
        const bool with_audio = std::any_of(binaries.begin() + static_cast<long>(before), binaries.end(),
            [](const std::vector<uint8_t>& message) { return !message.empty() && message[0] == proto::kStreamAudio; });
        if (with_audio || steady_ms() - last_meter_ms_ >= 200) {
            binaries.push_back(std::move(pending_meter_));
            pending_meter_.clear();
        }
    }
    const auto generation = format.generation;
    const double rate = format.rate;
    if (rate > 0 && (generation != last_audio_generation_ || rate != last_reported_rate_)) {
        last_audio_generation_ = generation;
        last_reported_rate_ = rate;
        Json message = Json::make_object();
        message.set("type", "audio-config");
        message.set("generation", static_cast<int>(generation));
        message.set("rate", rate);
        message.set("frame_samples", static_cast<int>(nac::kFrameHop));
        message.set("bitrate", channel_.audio_bitrate);
        texts.push_back(message.serialize());
    }
    size_t control_bytes = 0;
    for (const auto& text : texts) control_bytes += text.size() + (text.size() < 126 ? 2 : text.size() <= 65535 ? 4 : 10);
    for (const auto& binary : binaries) {
        if (!binary.empty() && binary[0] == proto::kStreamMeter) control_bytes += binary.size() + 2;
    }
    if (control_bytes) listener_->account_control_bytes(control_bytes);
}

void Session::tick() {
    if (!listener_) return;
    const int64_t now = steady_ms();
    if (transport_backlog_ > 2048) slow_meter_until_ms_ = now + 3000;
    // Ten JSON meter updates per second compete with audio on a thin link.
    // Keep the latest reading at 2 Hz while congested; tuning replies and
    // audio format announcements still leave immediately through collect().
    if (now < slow_meter_until_ms_ && now - last_meter_ms_ < 500) return;
    last_meter_ms_ = now;

    const ListenerTelemetry telemetry = listener_->telemetry();
    // Another station goes out at once, so its neighbour's name is never
    // shown on it; changes on one station wait for the second to pass.
    const bool new_station = telemetry.rds_station != rds_sent_station_;
    if (telemetry.rds_sequence != rds_sent_sequence_ && (new_station || now - last_rds_ms_ >= 1000)) {
        rds_sent_sequence_ = telemetry.rds_sequence;
        rds_sent_station_ = telemetry.rds_station;
        last_rds_ms_ = now;
        send_rds(listener_->rds());
    }
    if (binary_meter_) {
        proto::Meter meter;
        meter.dbfs = telemetry.signal_dbfs;
        meter.gain_db = telemetry.agc_gain_db;
        meter.squelch_statistic = telemetry.squelch_statistic;
        meter.pll_offset = telemetry.pll_offset_hz;
        meter.waterfall_fps = telemetry.waterfall_lines_per_second;
        meter.audio_bps = std::max(0, telemetry.audio_bitrate);
        meter.waterfall_bps = std::max(0, telemetry.waterfall_bitrate);
        meter.listeners = std::max(0, radio_.total_listeners());
        meter.squelch_open = telemetry.squelch_open;
        meter.pll_locked = telemetry.pll_locked;
        meter.auto_squelch = channel_.auto_squelch;
        meter.sam = channel_.mode == Mode::Sam;
        meter.nfm = ctcss_meter_ && channel_.mode == Mode::Nfm;
        meter.ctcss_hz = static_cast<float>(telemetry.ctcss_hz);
        pending_meter_.resize(proto::kMeterBytes);
        proto::write_meter(pending_meter_.data(), meter);
        return;
    }
    Json meter = Json::make_object();
    meter.set("type", "meter");
    meter.set("dbfs", std::round(telemetry.signal_dbfs * 10.0) / 10.0);
    meter.set("gain_db", std::round(telemetry.agc_gain_db * 10.0) / 10.0);
    meter.set("squelch_open", telemetry.squelch_open);
    if (channel_.auto_squelch) {
        // How far the passband is from looking like noise, so a listener can
        // see the squelch deciding rather than only hear the result.
        meter.set("squelch_statistic", std::round(telemetry.squelch_statistic * 10.0) / 10.0);
    }
    if (channel_.mode == Mode::Nfm) meter.set("ctcss", std::round(telemetry.ctcss_hz * 10.0) / 10.0);
    if (channel_.mode == Mode::Sam) {
        meter.set("pll_locked", telemetry.pll_locked);
        meter.set("pll_offset", std::round(telemetry.pll_offset_hz * 10.0) / 10.0);
    }
    meter.set("audio_bps", telemetry.audio_bitrate);
    meter.set("waterfall_bps", telemetry.waterfall_bitrate);
    meter.set("waterfall_fps", std::round(telemetry.waterfall_lines_per_second * 10.0) / 10.0);
    meter.set("listeners", radio_.total_listeners());
    queue_text(meter.serialize());
}

void Session::send_rds(const RdsState& rds) {
    Json message = Json::make_object();
    message.set("type", "rds");
    // Only what has been received: an empty message means nothing (yet).
    if (rds.pi >= 0) {
        char pi[9];
        std::snprintf(pi, sizeof pi, "%04X", static_cast<unsigned>(rds.pi & 0xffff));
        message.set("pi", std::string(pi));
    }
    if (rds.pty >= 0) message.set("pty", rds.pty);
    if (rds.pi >= 0) message.set("tp", rds.tp);
    if (!rds.ps.empty()) message.set("ps", rds.ps);
    if (!rds.rt.empty()) message.set("rt", rds.rt);
    queue_text(message.serialize());
}

Json Session::describe_band(const Band& band) const {
    const BandInfo info = band.info();
    Json out = Json::make_object();
    out.set("id", info.id);
    out.set("name", info.name);
    out.set("center", info.center_hz);
    out.set("low", info.low_hz);
    out.set("high", info.high_hz);
    out.set("sample_rate", info.sample_rate);
    out.set("sample_low", band.sample_low_hz());
    out.set("sample_high", band.sample_high_hz());
    out.set("max_bandwidth", info.max_bandwidth_hz);
    // Only where it can be had, so a page offers the mode where it works.
    if (band.wfm()) out.set("wfm", true);
    // What the band listens with, for the label beside its name: the source
    // kind, or for a hardware module the module's id ("rx888", "rtlsdr").
    // The kind only: never a device's serial number or a path.
    {
        const auto& configured = band.configured_values();
        const auto value = [&](const char* key, const char* fallback) {
            const auto found = configured.find(key);
            return found != configured.end() ? found->second : std::string(fallback);
        };
        std::string receiver = value("source", "test");
        if (receiver == "module") receiver = value("module", "module");
        const bool plain = receiver.size() <= 32 && std::all_of(receiver.begin(), receiver.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
        out.set("receiver", plain ? receiver : std::string("module"));
    }
    out.set("noise_blanker", info.noise_blanker);
    // One decimal is all this is worth: the estimate is smoothed over seconds
    // and a figure that moves in hundredths implies a precision it lacks.
    out.set("noise_floor", std::round(info.noise_floor_dbfs * 10.0) / 10.0);
    // An empty list is the signal that this receiver is uncalibrated, which
    // the client has to say out loud rather than quote S-units it cannot
    // stand behind.
    Json calibration = Json::make_array();
    for (const auto& point : info.calibration) {
        Json entry = Json::make_object();
        entry.set("hz", point.hz);
        entry.set("offset", point.offset_db);
        calibration.push_back(entry);
    }
    out.set("calibration", calibration);
    // Whether this band keeps an archive, and who may read it. Saying
    // "private" out loud costs nothing: the door is the admin session, not
    // the absence of a sign on it.
    out.set("history", info.history_access);
    if (info.history_access != "off") {
        // How far back the archive actually reaches, so a viewer can offer to
        // scrub through what is there rather than through what was asked for.
        out.set("history_from", static_cast<double>(info.history_oldest_ms));
        out.set("history_to", static_cast<double>(info.history_newest_ms));
    }
    out.set("listeners", info.listeners);
    out.set("running", info.running);
    // Hours only where they are not always, so a receiver without a
    // schedule sends what it sent before.
    out.set("on_air", info.on_air);
    if (info.hours != "always") {
        out.set("hours", info.hours);
        out.set("next_change", static_cast<double>(info.next_change_ms));
    }
    // Bands that take turns on one input say which, so a page can name the
    // band that comes on as another goes off.
    const std::string input = radio_.shared_input(&band);
    if (!input.empty()) out.set("shared_input", input);
    return out;
}

void Session::send_welcome() {
    Json message = Json::make_object();
    message.set("type", "welcome");
    message.set("protocol", 1);
    Json capabilities = Json::make_array();
    capabilities.push_back("meter-v1");
    capabilities.push_back("meter-ctcss");
    capabilities.push_back("nac2");
    capabilities.push_back("nac3");
    capabilities.push_back("wfc3");
    capabilities.push_back("wfc4");
    capabilities.push_back("wfc5");
    capabilities.push_back("audio-discontinuity");
    message.set("capabilities", capabilities);
    message.set("session", static_cast<double>(id_));

    const SiteInfo& site = radio_.site();
    Json site_json = Json::make_object();
    site_json.set("name", site.name);
    site_json.set("operator", site.operator_name);
    site_json.set("location", site.location);
    site_json.set("grid", site.grid_square);
    site_json.set("band_plan", site.band_plan);
    site_json.set("antenna", site.antenna);
    site_json.set("contact", site.contact);
    site_json.set("website", site.website);
    site_json.set("notice", site.notice);
    site_json.set("source_url", site.source_url);
    site_json.set("chat", site.chat);
    message.set("theme", radio_.theme().snapshot());
    message.set("site", site_json);

    Json bands = Json::make_array();
    for (const auto& band : radio_.bands()) bands.push_back(describe_band(*band));
    message.set("bands", bands);
    Json decoders = radio_.listed_decoders_json();
    if (decoders.size() > 0) message.set("decoders", decoders);

    Json modes = Json::make_array();
    for (const char* name : {"usb", "lsb", "cw", "cwl", "am", "sam", "nfm", "dsb", "wfm"}) modes.push_back(name);
    message.set("modes", modes);

    Json agc_profiles = Json::make_array();
    for (const char* name : {"auto", "fast", "medium", "slow", "long", "off"}) agc_profiles.push_back(name);
    message.set("agc_profiles", agc_profiles);

    Json limits = Json::make_object();
    limits.set("max_waterfall_width", 4096);
    limits.set("max_waterfall_fps", 30);
    limits.set("min_audio_bitrate", 8000);
    limits.set("max_audio_bitrate", 128000);
    limits.set("max_users", site.max_users);
    message.set("limits", limits);

    queue_text(message.serialize());
}

void Session::send_state(const std::string& note) {
    state_due_ = true;
    if (!note.empty()) state_note_ = note;
    if (steady_ms() - last_state_ms_ >= kStateSpacingMs) flush_state();
}

void Session::flush_state() {
    const std::string note = std::move(state_note_);
    state_note_.clear();
    state_due_ = false;
    last_state_ms_ = steady_ms();
    Json message = Json::make_object();
    message.set("type", "state");
    Json ack = Json::make_object();
    ack.set("tune", tune_request_id_);
    ack.set("viewport", viewport_request_id_);
    ack.set("dsp", dsp_request_id_);
    message.set("ack", ack);
    message.set("band", band_id_);
    message.set("freq", channel_.frequency_hz);
    message.set("mode", mode_name(channel_.mode));
    message.set("low", channel_.bandwidth_low);
    message.set("high", channel_.bandwidth_high);
    message.set("cw_pitch", channel_.cw_pitch_hz);
    message.set("agc", agc_profile_name(channel_.agc));
    // What that comes to in this mode: Auto's choice, or off for NFM.
    message.set("agc_effective", agc_profile_name(agc_in_effect(channel_)));
    message.set("gain", channel_.manual_gain_db);
    message.set("nr", channel_.noise_reduction);
    message.set("autonotch", channel_.auto_notch);
    message.set("volume", channel_.volume);
    message.set("squelch", channel_.squelch_dbfs);
    message.set("auto_squelch", channel_.auto_squelch);
    message.set("highpass", channel_.highpass_hz);
    message.set("deemphasis", channel_.deemphasis_us);
    message.set("ctcss_filter", channel_.ctcss_filter);
    message.set("ctcss_squelch", channel_.ctcss_squelch);
    message.set("audio_enabled", channel_.audio_enabled);
    message.set("audio_bitrate", channel_.audio_bitrate);
    message.set("audio_codec", channel_.packet_audio ? "nac3" : channel_.compact_audio ? "nac2" : "nac");
    message.set("audio_frames", channel_.packet_audio ? channel_.audio_packet_frames : 1);
    message.set("noise_margin", channel_.audio_noise_margin_db);
    if (listener_) message.set("audio_rate", listener_->actual_audio_rate());
    if (band_) {
        message.set("filter_limit", channel_.mode == Mode::Wfm
            ? std::min(kWfmMaxBandwidthHz / 2, band_->sample_rate() / 2)
            : channel_audio_rate(band_->sample_rate(), channel_.requested_audio_rate, band_->fft_size()) / 2);
    }
    message.set("wfm_deemphasis", channel_.wfm_deemphasis_us);

    Json notches = Json::make_array();
    for (const Notch& notch : channel_.notches) {
        Json entry = Json::make_object();
        entry.set("hz", notch.center_hz);
        entry.set("width", notch.width_hz);
        notches.push_back(entry);
    }
    message.set("notches", notches);

    Json view = Json::make_object();
    view.set("enabled", viewport_.enabled);
    view.set("low", viewport_.low_hz);
    view.set("high", viewport_.high_hz);
    view.set("width", viewport_.width);
    view.set("fps", viewport_.lines_per_second);
    message.set("viewport", view);

    if (!note.empty()) message.set("note", note);

    queue_text(message.serialize());
}

void Session::send_error(const std::string& text) {
    Json message = Json::make_object();
    message.set("type", "error");
    message.set("message", text);
    queue_text(message.serialize());
}

}  // namespace fernsdr

// IQ and real-signal sources.
//
// The deliberate choice here is that hardware support is a pipe, not a driver
// stack. An RX-888 mk2 is fed in through `rx888_stream | fernsdr`, an RTL
// stick through `rtl_sdr -`, a ka9q-radio stream over UDP. That keeps this
// server free of libusb, vendor SDKs and the whole class of build problems
// that makes a receiver hard to stand up on whatever machine is in the shack -
// and it means a new front end never needs a change here.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "../dsp/channelizer.h"
#include "../dsp/fft.h"
#include "../util/config.h"
#include "../util/json.h"

namespace fernsdr {

class ModuleStore;

struct SourceStats {
    uint64_t samples_read = 0;
    uint64_t dropped = 0;  // samples lost to overruns on a live input
    bool connected = false;
};

// Why a restartable source stopped delivering.
enum class SourceFailure {
    None,
    Transient,  // try again after a delay: a device unplugged or busy
    Operator,   // retrying cannot help until the operator changes something
    Disabled,   // the operator switched it off
};

class Source {
public:
    virtual ~Source() = default;

    virtual bool start(std::string& error) = 0;
    // May be called while read() is blocked. Stop accepting input and wake
    // bounded waits; stop() closes resources after the reader has joined.
    virtual void interrupt() {}
    virtual void stop() = 0;

    // Whether this source delivers complex or real samples.
    virtual SignalKind kind() const = 0;

    // Fills `out` with exactly `count` samples, blocking as needed. Call the
    // one matching kind(); the other returns false.
    virtual bool read(cfloat* out, size_t count) { (void)out; (void)count; return false; }
    virtual bool read_real(float* out, size_t count) { (void)out; (void)count; return false; }

    // Nominal sample rate. For a real source the spectrum spans 0 .. rate/2.
    virtual double sample_rate() const = 0;
    // RF frequency at the centre of the stream. Meaningless for a real source,
    // which starts at DC; see `frequency_offset` for upconverters.
    virtual double center_hz() const = 0;

    virtual SourceStats stats() const = 0;
    virtual const char* kind_name() const = 0;

    // True once, after a read, when what that read delivered does not follow
    // on from the read before: a FIFO producer went away and came back. The
    // band then times decoder channels afresh. Called by the band's thread.
    virtual bool take_discontinuity() { return false; }

    // A source that can fail and come back, such as a module whose USB
    // device was unplugged. Its band restarts it after a growing delay
    // instead of stopping.
    virtual bool restartable() const { return false; }
    // After a failed read: why, in words for the operator, and what kind.
    virtual std::string failure() const { return ""; }
    virtual SourceFailure failure_kind() const { return SourceFailure::None; }
    // Clears interrupt() for a new run. Only the thread that starts the band
    // calls this, before the band's thread exists; a source that is started
    // again from its own band thread must not clear an interrupt that
    // arrived meanwhile, or stopping the band would wait on a whole restart.
    virtual void reset_interrupt() {}
    // Called before each start: true when the band is trying again after a
    // failure it has already logged. The source then keeps the routine lines
    // of that attempt, such as a module's own start-up messages, out of the
    // receiver's log. A dongle left unplugged overnight would otherwise fill
    // the log with the same three lines every thirty seconds and push out
    // everything an operator might need from before. A failure that changes
    // is still logged, by the band.
    virtual void set_retrying(bool quietly) { (void)quietly; }
    // Details for the admin panel; null when there are none.
    virtual Json describe() const { return Json(); }
    // Changes settings while running. `result` says what was sent.
    virtual bool apply_live(const Json& settings, Json& result, std::string& error) {
        (void)settings;
        (void)result;
        error = "this input has no settings that change while it runs";
        return false;
    }
    // Takes new settings from a band section for the next start.
    virtual bool reconfigure(const ConfigSection& section, std::string& error) {
        (void)section;
        error = "this input cannot take new settings without restarting the receiver";
        return false;
    }
};

// What building a source may need beyond its own section.
struct SourceContext {
    std::shared_ptr<ModuleStore> modules;  // for source = module; may be null
    // A file being saved from the panel rather than one being started: a
    // module band that contradicts its module is refused, instead of
    // started to fail on its own as it always has (see make_module_source).
    bool strict = false;
};

// Builds a source from a [band:...] config section. `error` explains any
// failure in terms the operator can act on.
std::unique_ptr<Source> make_source(const ConfigSection& section, const SourceContext& context, std::string& error);
inline std::unique_ptr<Source> make_source(const ConfigSection& section, std::string& error) {
    return make_source(section, SourceContext{}, error);
}

// Sample component types. `signal = iq` pairs them into complex samples;
// `signal = real` takes them as they come.
enum class SampleType {
    U8,   // unsigned 8-bit, offset binary (rtl_sdr)
    S8,   // signed 8-bit (hackrf_transfer)
    U16,  // unsigned 16-bit little-endian
    S16,  // signed 16-bit little-endian (rx888_stream, airspy_rx, rx_sdr)
    F32,  // 32-bit float
};

// Accepts both the plain names (u8, s16, f32) and the legacy complex spellings
// (cu8, cs16, cf32), the latter implying signal = iq.
bool sample_type_from_name(const std::string& name, SampleType& type, bool& implies_iq);
const char* sample_type_name(SampleType type);
size_t bytes_per_component(SampleType type);

inline size_t bytes_per_sample(SampleType type, SignalKind kind) {
    return bytes_per_component(type) * (kind == SignalKind::Iq ? 2 : 1);
}

// The sample format a band's config asks for.
struct SourceFormat {
    SampleType type = SampleType::S16;
    SignalKind kind = SignalKind::Iq;
};

// Reads `format` and `signal` from a band section. Defaults to IQ s16, except
// where a legacy complex format name (cs16, cu8, ...) settles it.
bool parse_source_format(const ConfigSection& section, SourceFormat& out, std::string& error);

// Converts `count` interleaved I/Q samples, normalised to +/-1.
void convert_iq(SampleType type, const uint8_t* raw, cfloat* out, size_t count);
// Converts `count` real samples, normalised to +/-1.
void convert_real(SampleType type, const uint8_t* raw, float* out, size_t count);

}  // namespace fernsdr

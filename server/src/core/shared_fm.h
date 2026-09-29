// Broadcast FM demodulated once for everyone on the same station.
//
// Most of a WFM listener's cost comes before the codec: a 200 kHz channel,
// the discriminator at its rate and the decimation to audio. None of it
// depends on who listens, and on a broadcast band most listeners are on a
// handful of stations, so the band keeps one SharedFm per station and
// passband and runs it once a block, before its listeners; each listener
// copies the audio and does only what is its own (squelch, volume, codec).
#pragma once

#include "../dsp/channelizer.h"
#include "../dsp/demod.h"
#include "../dsp/fir_decimator.h"
#include "../dsp/rds.h"

#include <memory>
#include <vector>

namespace fernsdr {

class Band;

// Broadcast FM audio stops at 15 kHz, and the stereo pilot at 19 kHz must
// be gone before it can fold into what is heard; so the audio rate is chosen
// to leave the filter room: over 34 kHz, which puts anything folding into
// 0 to 15 kHz above 19 kHz, where the filter has removed it.
constexpr double kWfmAudioHz = 15000.0;
constexpr double kWfmStopHz = 18500.0;
constexpr double kWfmMinimumAudioRate = 34000.0;

// What makes two listeners' FM the same. The frequency is on a 1 kHz grid:
// a broadcast station is 200 kHz wide, and a listener a few hundred hertz
// off it hears nothing different once the offset is taken out as DC.
struct FmKey {
    double frequency_hz = 0.0;
    double low_hz = -100000.0;
    double high_hz = 100000.0;
    float deemphasis_us = 50.0f;

    static FmKey for_channel(double frequency_hz, double low_hz, double high_hz, float deemphasis_us);
    bool operator==(const FmKey& other) const {
        return frequency_hz == other.frequency_hz && low_hz == other.low_hz && high_hz == other.high_hz &&
               deemphasis_us == other.deemphasis_us;
    }
    bool operator!=(const FmKey& other) const { return !(*this == other); }
};

class SharedFm {
public:
    SharedFm(const Band& band, const FmKey& key);

    const FmKey& key() const { return key_; }
    double audio_rate() const { return decimator_.output_rate(); }

    // Called by the band once a block, after the transform and before any
    // listener runs; the listeners read what it made until the next call.
    void process(const ChannelBlock& block);

    // This block's audio, and the channel's level before demodulation, in
    // dBFS, for the S-meter and the squelch.
    const std::vector<float>& audio() const { return audio_; }
    float level_dbfs() const { return level_dbfs_; }
    // What the station sends by RDS, decoded from the same multiplex; null
    // where the channel is too narrow to carry the 57 kHz subcarrier.
    const RdsState* rds() const { return rds_ ? &rds_->state() : nullptr; }

private:
    FmKey key_;
    std::unique_ptr<Channel> channel_;
    Demodulator demodulator_;
    FirDecimator decimator_;
    std::vector<cfloat> baseband_;
    std::vector<float> discriminated_;
    std::vector<float> audio_;
    std::unique_ptr<RdsDecoder> rds_;
    float level_dbfs_ = -160.0f;
    float dc_state_ = 0.0f;
    float dc_alpha_ = 0.0f;
};

}  // namespace fernsdr

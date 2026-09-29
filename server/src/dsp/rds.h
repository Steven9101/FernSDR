// RDS, the data broadcast FM stations send beside their programme: the
// station's name, a line of text, its identity and what it plays.
//
// It rides on a 57 kHz subcarrier of the FM multiplex (three times the
// stereo pilot) as BPSK of biphase symbols at 1187.5 bits a second, the bits
// differentially coded, in blocks of 16 data bits and a 10-bit check word
// with an offset that says which of the group's four blocks it is (IEC 62106).
// This takes the multiplex as the discriminator gives it, at the channel's
// rate, and follows:
//
//   57 kHz down to baseband and decimation to about 16 kHz, the mixing
//   folded into the taps (BandDecimator)
//   -> a sharp 2.4 kHz channel filter, clear of the stereo signal
//   -> a Costas loop for the carrier's phase
//   -> a filter matched to one biphase chip, with Gardner timing recovery,
//      and the chips paired into bits
//   -> differential decoding -> block sync by check word -> groups
//
// Everything it reports came from the air: the text is kept to what can be
// printed, with the RDS character table's letters mapped to Unicode.
#pragma once

#include "fir_decimator.h"

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace fernsdr {

struct RdsState {
    // Changes whenever anything below does, so a reader can tell.
    uint64_t sequence = 0;
    bool synced = false;
    int pi = -1;   // programme identification, -1 until one is seen
    int pty = -1;  // programme type code, 0 to 31
    bool tp = false;
    std::string ps;  // programme service name, up to 8 characters, UTF-8
    std::string rt;  // radiotext, up to 64 characters, UTF-8
};

class RdsDecoder {
public:
    // `rate` is the multiplex's sample rate; at least 120 kHz.
    explicit RdsDecoder(double rate);

    void process(const float* mpx, size_t count);
    const RdsState& state() const { return state_; }
    // Blocks received with a good check word, and those without, for tests.
    uint64_t good_blocks() const { return good_blocks_; }
    uint64_t bad_blocks() const { return bad_blocks_; }

    // The block layer on its own, for tests: one differentially decoded bit.
    void take_bit(int bit);

    // The last matched-filter outputs timing recovery can look back on: a
    // power of two, so the ring's index is a mask.
    static constexpr size_t kMatchedRing = 64;

private:
    void take_symbol(float value);
    void take_chip(float chip);
    void take_block(uint16_t data, int offset);
    void take_group();

    double rate_;
    // Down to baseband.
    BandDecimator band_;
    FirDecimator channel_i_, channel_q_;
    std::vector<std::complex<float>> base_;
    std::vector<float> base_i_, base_q_;
    double base_rate_ = 0.0;
    // Costas loop: e^(-j phase) of the carrier as it is followed.
    std::complex<double> carrier_{1.0, 0.0};
    double carrier_freq_ = 0.0;
    double costas_alpha_ = 0.0, costas_beta_ = 0.0;
    // Matched filter over one chip (half a bit).
    std::vector<float> history_;
    size_t history_at_ = 0;
    float chip_sum_ = 0.0f;
    double samples_per_chip_ = 0.0;
    size_t chip_samples_ = 0;
    // Pairing chips into bits: which chip parity ends a bit, and how clear.
    float previous_chip_ = 0.0f;
    uint64_t chips_ = 0;
    int pairing_ = 0;
    float pairing_energy_[2] = {0.0f, 0.0f};
    int pairing_doubts_ = 0;
    // Timing recovery: where the next chip is sampled, in filter outputs.
    double mu_ = 0.0;
    double next_ = 0.0;
    uint64_t filtered_ = 0;
    std::vector<float> matched_;  // the last few filter outputs
    float last_bit_sample_ = 0.0f;
    // Differential decoding.
    int last_symbol_ = 0;
    // Blocks.
    uint32_t register_ = 0;
    int bits_in_register_ = 0;
    bool block_synced_ = false;
    int expected_offset_ = -1;
    int bits_to_next_block_ = 0;
    int bad_in_a_row_ = 0;
    uint16_t group_[4] = {0, 0, 0, 0};
    int group_have_ = 0;  // which blocks of the group are in, as bits
    uint64_t good_blocks_ = 0, bad_blocks_ = 0;
    // Text being put together.
    char ps_[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    int ps_segments_ = 0;   // which segments have come, as bits
    int ps_confirmed_ = 0;  // which came the same as the time before
    int ps_run_ = 0;        // the next segment of an in-order run
    char rt_[64];
    int rt_segments_ = 0;
    int rt_end_ = -1;
    int rt_flag_ = -1;
    // Which pass each radiotext segment came in, and which have come the
    // same twice since the text last started afresh.
    int rt_pass_ = 0;
    int rt_last_segment_ = -1;
    int rt_slot_pass_[16] = {};
    uint32_t rt_confirmed_ = 0;
    // bad_blocks_ when the last segment came, to tell a pass was broken.
    uint64_t rt_bad_at_ = 0;
    uint64_t ps_bad_at_ = 0;
    int last_pi_ = -1;
    int pi_confirmations_ = 0;
    RdsState state_;
};

// The RDS character table (IEC 62106 annex E) to UTF-8; what is not a
// letter or sign there becomes a space.
std::string rds_to_utf8(const char* text, size_t length);

}  // namespace fernsdr

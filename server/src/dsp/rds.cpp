#include "rds.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fernsdr {

namespace {

constexpr double kSubcarrierHz = 57000.0;
constexpr double kBitRate = 1187.5;
constexpr double kTwoPi = 6.283185307179586;

// Offset words, by block position: A, B, C, C', D.
constexpr uint16_t kOffsets[5] = {0x0fc, 0x198, 0x168, 0x350, 0x1b4};
constexpr int kA = 0, kB = 1, kC = 2, kCPrime = 3, kD = 4;

// The check word a block's 16 data bits call for: the remainder of
// data x^10 by x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1.
uint16_t check_word(uint16_t data) {
    uint16_t reg = 0;
    for (int i = 15; i >= 0; i--) {
        const int feedback = ((reg >> 9) & 1) ^ ((data >> i) & 1);
        reg = static_cast<uint16_t>((reg << 1) & 0x3ff);
        if (feedback) reg ^= 0x1b9;
    }
    return reg;
}

// Which offset a 26-bit block carries, or -1 when its check word is wrong.
int offset_of(uint32_t block) {
    const uint16_t data = static_cast<uint16_t>(block >> 10);
    const uint16_t check = static_cast<uint16_t>(block & 0x3ff);
    const uint16_t wanted = check_word(data);
    for (int o = 0; o < 5; o++) {
        if ((check ^ kOffsets[o]) == wanted) return o;
    }
    return -1;
}

int next_offset(int offset) {
    switch (offset) {
        case kA: return kB;
        case kB: return kC;
        case kC:
        case kCPrime: return kD;
        default: return kA;
    }
}

// Linear interpolation between the filter outputs either side of `at`, in
// a ring of kMatchedRing.
float sample_at(const std::vector<float>& ring, uint64_t newest, double at) {
    const size_t size = RdsDecoder::kMatchedRing;
    const double floor_at = std::floor(at);
    const auto i = static_cast<uint64_t>(floor_at);
    if (i + size <= newest + 1 || i + 1 > newest) return 0.0f;
    const float a = ring[i & (size - 1)];
    const float b = ring[(i + 1) & (size - 1)];
    const float t = static_cast<float>(at - floor_at);
    return a + (b - a) * t;
}

}  // namespace

std::string rds_to_utf8(const char* text, size_t length) {
    // The letters of the table's upper half, from 0x80, as UTF-8; an empty
    // entry is a sign this does not show, and becomes a space.
    static const char* const kUpper[128] = {
        "á", "à", "é", "è", "í", "ì", "ó", "ò", "ú", "ù", "Ñ", "Ç", "Ş", "ß", "¡", "Ĳ",
        "â", "ä", "ê", "ë", "î", "ï", "ô", "ö", "û", "ü", "ñ", "ç", "ş", "ğ", "ı", "ĳ",
        "ª", "α", "©", "‰", "Ğ", "ě", "ň", "ő", "π", "€", "£", "$", "←", "↑", "→", "↓",
        "º", "¹", "²", "³", "±", "İ", "ń", "ű", "µ", "¿", "÷", "°", "¼", "½", "¾", "§",
        "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò", "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Đ", "Ŀ",
        "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö", "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
        "Ã", "Å", "Æ", "Œ", "ŷ", "Ý", "Õ", "Ø", "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ŧ", "ð",
        "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø", "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ŧ", "",
    };
    std::string out;
    for (size_t i = 0; i < length; i++) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
        else if (c >= 0x80 && kUpper[c - 0x80][0] != '\0') out += kUpper[c - 0x80];
        else out += ' ';
    }
    return out;
}

RdsDecoder::RdsDecoder(double rate) : rate_(rate) {
    const size_t factor = std::max<size_t>(1, static_cast<size_t>(rate / 16000.0));
    // The data needs 2.4 kHz either side of the carrier. Selecting it takes
    // two filters. The first, at the multiplex's rate, only has to keep
    // what would fold onto the channel filter's pass band and transition
    // 60 dB down: from the new rate less 4 kHz on. That leaves it a wide
    // transition and few taps.
    const double out_rate = rate / static_cast<double>(factor);
    band_.configure(rate, factor, kSubcarrierHz, 2400.0, out_rate - 4000.0, 60.0);
    base_rate_ = out_rate;
    // The second, at the new rate, is the sharp one: the stereo difference
    // signal reaches 53 kHz, 4 kHz from the subcarrier and often ten times
    // its level, and it put the carrier and timing loops off when only the
    // chip filter stood in its way. Sharp is cheap here, 16 times fewer
    // samples than above.
    channel_i_.configure(out_rate, 1, 2400.0, 4000.0, 60.0);
    channel_q_.configure(out_rate, 1, 2400.0, 4000.0, 60.0);
    // A second-order loop about 15 Hz wide: enough for a receiver's crystal
    // error on 57 kHz, narrow enough to hold through the data's phase flips.
    const double wn = kTwoPi * 15.0 / base_rate_;
    costas_alpha_ = 2.0 * 0.707 * wn;
    costas_beta_ = wn * wn;
    samples_per_chip_ = base_rate_ / (2.0 * kBitRate);
    chip_samples_ = std::max<size_t>(1, static_cast<size_t>(std::lround(samples_per_chip_)));
    history_.assign(chip_samples_, 0.0f);
    matched_.assign(kMatchedRing, 0.0f);
    next_ = samples_per_chip_ * 2.0;
    std::memset(rt_, ' ', sizeof rt_);
}

void RdsDecoder::process(const float* mpx, size_t count) {
    base_.resize(count / band_.factor() + 1);
    const size_t produced = band_.process(mpx, count, base_.data());
    base_i_.resize(produced);
    base_q_.resize(produced);
    for (size_t n = 0; n < produced; n++) {
        base_i_[n] = base_[n].real();
        base_q_[n] = base_[n].imag();
    }
    channel_i_.process(base_i_.data(), produced, base_i_.data());
    channel_q_.process(base_q_.data(), produced, base_q_.data());
    // Costas: turn the carrier's phase away, and steer by the product of the
    // two arms, which BPSK's flips do not change the sign of. The loop is
    // steered once every kCostasRun samples, a thousand times a second,
    // where its 15 Hz hardly notice the wait; within a run the samples do
    // not depend on each other's error, so no sample waits on a division.
    constexpr size_t kCostasRun = 16;
    for (size_t start = 0; start < produced; start += kCostasRun) {
        const size_t end = std::min(produced, start + kCostasRun);
        // The run's step, e^(-j freq): under 0.02 rad, where the sine's and
        // cosine's series to the cube and the fourth power are exact to
        // 1e-10, with no library call.
        const double f = carrier_freq_, f2 = f * f;
        const double wr = 1.0 - f2 * (0.5 - f2 / 24.0), wi = -f * (1.0 - f2 / 6.0);
        double pr = carrier_.real(), pi = carrier_.imag();
        float products = 0.0f, power = 1e-20f;
        for (size_t n = start; n < end; n++) {
            const float cr = static_cast<float>(pr), ci = static_cast<float>(pi);
            const float i = base_i_[n] * cr - base_q_[n] * ci;
            const float q = base_i_[n] * ci + base_q_[n] * cr;
            products += i * q;
            power += i * i + q * q;
            const double next = pr * wr - pi * wi;
            pi = pr * wi + pi * wr;
            pr = next;
            take_symbol(i);
        }
        const double length = static_cast<double>(end - start);
        const double error = std::clamp(static_cast<double>(products / power), -0.5, 0.5);
        carrier_freq_ += costas_beta_ * length * error;
        carrier_freq_ = std::clamp(carrier_freq_, -kTwoPi * 50.0 / base_rate_, kTwoPi * 50.0 / base_rate_);
        // The proportional term, as one small turn, and the phasor's
        // length set back to one.
        const double p = -costas_alpha_ * length * error;
        const double tr = 1.0 - 0.5 * p * p, ti = p;
        const double re = pr * tr - pi * ti, im = pr * ti + pi * tr;
        const double fix = 1.5 - 0.5 * (re * re + im * im);
        carrier_ = std::complex<double>(re * fix, im * fix);
    }
}

void RdsDecoder::take_symbol(float value) {
    // Each chip, half a bit, integrated over its length. Timing is recovered
    // on chips rather than bits: a filter matched to a whole biphase bit
    // peaks half a bit off too, wherever two bits' polarities agree, and a
    // timing loop on it settles there as readily as on the bit.
    // A running sum over the chip, taken afresh once round the ring so
    // rounding cannot build up.
    chip_sum_ += value - history_[history_at_];
    history_[history_at_] = value;
    if (++history_at_ == history_.size()) {
        history_at_ = 0;
        chip_sum_ = 0.0f;
        for (const float v : history_) chip_sum_ += v;
    }
    matched_[filtered_ & (kMatchedRing - 1)] = chip_sum_;
    const uint64_t newest = filtered_;
    filtered_++;
    // Gardner: sample at the chip's end and halfway back to the last one;
    // at a change of sign the midpoint is zero when the timing is right.
    while (next_ + 1.0 <= static_cast<double>(newest)) {
        const float now = sample_at(matched_, newest, next_);
        const float middle = sample_at(matched_, newest, next_ - samples_per_chip_ / 2.0);
        const float scale = std::max(1e-12f, 0.5f * (now * now + last_bit_sample_ * last_bit_sample_));
        const double error = std::clamp(static_cast<double>(middle * (last_bit_sample_ - now) / scale), -1.0, 1.0);
        next_ += samples_per_chip_ + 0.05 * error;
        take_chip(now);
        last_bit_sample_ = now;
    }
}

void RdsDecoder::take_chip(float chip) {
    // Within a bit the two chips are always opposite; across a boundary only
    // when the bits differ. So of the two ways to pair chips, the right one
    // has the larger differences on average: follow it.
    const float difference = previous_chip_ - chip;
    const int parity = static_cast<int>(chips_ & 1);
    pairing_energy_[parity] = 0.99f * pairing_energy_[parity] + 0.01f * std::fabs(difference);
    chips_++;
    previous_chip_ = chip;
    // Compared as pairings, not as this chip against the last: the counter
    // must not be reset by every other chip.
    const int other = pairing_ ^ 1;
    if (pairing_energy_[other] > 1.25f * pairing_energy_[pairing_]) {
        if (++pairing_doubts_ > 64) {
            pairing_ = other;
            pairing_doubts_ = 0;
        }
    } else {
        pairing_doubts_ = 0;
    }
    if (parity != pairing_) return;
    // Differential: a bit is 1 where the symbol changed.
    const int symbol = difference >= 0.0f ? 1 : -1;
    if (last_symbol_ != 0) take_bit(symbol != last_symbol_ ? 1 : 0);
    last_symbol_ = symbol;
}

void RdsDecoder::take_bit(int bit) {
    register_ = ((register_ << 1) | static_cast<uint32_t>(bit & 1)) & 0x3ffffff;
    if (bits_in_register_ < 26) bits_in_register_++;
    if (!block_synced_) {
        if (bits_in_register_ < 26) return;
        // Looking for two blocks in a row, 26 bits apart, in their order.
        const int offset = offset_of(register_);
        bits_to_next_block_--;
        if (offset < 0) {
            if (bits_to_next_block_ < -26) expected_offset_ = -1;
            return;
        }
        if (bits_to_next_block_ == 0 && expected_offset_ >= 0 &&
            (offset == expected_offset_ || (expected_offset_ == kC && offset == kCPrime))) {
            block_synced_ = true;
            bad_in_a_row_ = 0;
            group_have_ = 0;
            state_.synced = true;
        }
        expected_offset_ = next_offset(offset);
        bits_to_next_block_ = 26;
        if (block_synced_) {
            good_blocks_++;
            take_block(static_cast<uint16_t>(register_ >> 10), offset);
        }
        return;
    }
    if (--bits_to_next_block_ > 0) return;
    bits_to_next_block_ = 26;
    const int offset = offset_of(register_);
    const bool expected = offset == expected_offset_ || (expected_offset_ == kC && offset == kCPrime);
    if (expected) {
        good_blocks_++;
        bad_in_a_row_ = 0;
        take_block(static_cast<uint16_t>(register_ >> 10), offset);
    } else {
        bad_blocks_++;
        // The group this block belonged to is incomplete now.
        if (expected_offset_ == kA) group_have_ = 0;
        else group_have_ &= ~(1 << (expected_offset_ == kD ? 3 : expected_offset_));
        if (++bad_in_a_row_ >= 10) {
            // Lost: look for the blocks again, bit by bit. What was half
            // put together is dropped: across a gap this long the station
            // may have changed its text, and the halves would not match.
            block_synced_ = false;
            expected_offset_ = -1;
            state_.synced = false;
            std::memset(rt_, ' ', sizeof rt_);
            rt_segments_ = 0;
            rt_end_ = -1;
            rt_confirmed_ = 0;
            ps_segments_ = 0;
            ps_confirmed_ = 0;
            ps_run_ = 0;
        }
    }
    expected_offset_ = next_offset(expected_offset_);
}

void RdsDecoder::take_block(uint16_t data, int offset) {
    const int position = offset == kA ? 0 : offset == kB ? 1 : offset == kD ? 3 : 2;
    if (position == 0) group_have_ = 0;
    group_[position] = data;
    group_have_ |= 1 << position;
    // C' carries the PI again, where a B group has no C.
    if (offset == kCPrime) group_have_ |= 1 << 4;
    if (position == 3) {
        take_group();
        group_have_ = 0;
    }
}

void RdsDecoder::take_group() {
    if (!(group_have_ & 2)) return;
    const uint16_t b = group_[1];
    const int type = b >> 12;
    const bool version_b = (b >> 11) & 1;
    // The PI, taken once it has come the same twice.
    int pi = -1;
    if (group_have_ & 1) pi = group_[0];
    else if (version_b && (group_have_ & 16)) pi = group_[2];
    if (pi >= 0) {
        pi_confirmations_ = pi == last_pi_ ? pi_confirmations_ + 1 : 0;
        last_pi_ = pi;
    }
    const bool have_c = (group_have_ & 4) && !version_b;
    const bool have_d = group_have_ & 8;
    bool ps_ready = false;
    if (type == 0 && have_d) {
        // Two ways to a name. Many stations scroll a slogan through the
        // eight characters, a segment at a time in order, so a run of all
        // four in order is one step of it and shown at once; collected in
        // any order, the segments of two steps would mix. A name that stays
        // is shown too once every segment has come the same twice, which
        // lost blocks and any order cannot stop.
        const int segment = b & 3;
        const char first = static_cast<char>(group_[3] >> 8), second = static_cast<char>(group_[3] & 0xff);
        const int bit = 1 << segment;
        // Any change puts every confirmation in doubt: a segment confirmed
        // before it may be from the name before, when its own change was
        // in a lost group.
        if ((ps_segments_ & bit) && ps_[2 * segment] == first && ps_[2 * segment + 1] == second) {
            ps_confirmed_ |= bit;
        } else {
            ps_confirmed_ = 0;
        }
        ps_[2 * segment] = first;
        ps_[2 * segment + 1] = second;
        ps_segments_ |= bit;
        // A run is broken by any block lost within it too: across a lost
        // stretch, its halves may be from two steps.
        const bool unbroken = bad_blocks_ == ps_bad_at_;
        ps_bad_at_ = bad_blocks_;
        ps_run_ = segment == ps_run_ && unbroken ? ps_run_ + 1 : segment == 0 ? 1 : 0;
        if (ps_run_ == 4 || ps_confirmed_ == 15) {
            ps_ready = true;
            ps_run_ = 0;
        }
    } else if (type == 2) {
        const int flag = (b >> 4) & 1;
        if (rt_flag_ >= 0 && flag != rt_flag_) {
            // The A/B flag flipped: a new text follows.
            std::memset(rt_, ' ', sizeof rt_);
            rt_segments_ = 0;
            rt_end_ = -1;
            rt_confirmed_ = 0;
        }
        rt_flag_ = flag;
        const int segment = b & 15;
        char chars[4];
        int count = 0;
        if (!version_b && have_c && have_d) {
            chars[0] = static_cast<char>(group_[2] >> 8);
            chars[1] = static_cast<char>(group_[2] & 0xff);
            chars[2] = static_cast<char>(group_[3] >> 8);
            chars[3] = static_cast<char>(group_[3] & 0xff);
            count = 4;
        } else if (version_b && have_d) {
            chars[0] = static_cast<char>(group_[3] >> 8);
            chars[1] = static_cast<char>(group_[3] & 0xff);
            count = 2;
        }
        // A station that changes its text without flipping the flag, as
        // many do, shows it by a segment that differs from what came
        // before, or one past the old text's end: start the text afresh.
        // Some stations pad past the end with spaces; those are not a change.
        bool changed = false;
        const bool beyond = count > 0 && rt_end_ >= 0 && segment * count > rt_end_;
        if (beyond) {
            for (int k = 0; k < count; k++) changed = changed || (chars[k] != ' ' && chars[k] != 0x0d);
            if (!changed) count = 0;
        }
        const bool known = count > 0 && (rt_segments_ & (1 << segment));
        if (known) {
            for (int k = 0; k < count && !changed; k++) {
                const int at = segment * count + k;
                if (at >= 64 || (rt_end_ >= 0 && at > rt_end_)) break;
                const bool end_here = rt_end_ == at;
                changed = end_here ? chars[k] != 0x0d : chars[k] == 0x0d || chars[k] != rt_[at];
            }
        }
        if (changed) {
            std::memset(rt_, ' ', sizeof rt_);
            rt_segments_ = 0;
            rt_end_ = -1;
            rt_confirmed_ = 0;
        }
        for (int k = 0; k < count; k++) {
            const int at = segment * count + k;
            if (at >= 64) break;
            if (chars[k] == 0x0d) {
                rt_end_ = at;
                break;
            }
            rt_[at] = chars[k];
        }
        if (count > 0) {
            if (known && !changed) rt_confirmed_ |= 1u << segment;
            rt_segments_ |= 1 << segment;
            // Segments come round in order; a number no higher than the
            // last starts the next pass, and so does a lost block, after
            // which the next segment may be from another round.
            if (segment <= rt_last_segment_ || bad_blocks_ != rt_bad_at_) rt_pass_++;
            rt_bad_at_ = bad_blocks_;
            rt_last_segment_ = segment;
            rt_slot_pass_[segment] = rt_pass_;
        }
        // The text is whole when every segment up to its end is in.
        const int per = count > 0 ? count : 4;
        const int last = rt_end_ >= 0 ? (rt_end_ - 1) / per : 15;
        const int wanted = last < 0 ? 1 : (1 << (last + 1)) - 1;
        if (count > 0 && (rt_segments_ & wanted) == wanted) {
            const int length = rt_end_ >= 0 ? rt_end_ : (version_b ? 32 : 64);
            std::string text = rds_to_utf8(rt_, static_cast<size_t>(length));
            while (!text.empty() && text.back() == ' ') text.pop_back();
            // Whole from one unbroken pass, it is one text. Put together
            // over several, the holes of one may have been filled by the
            // next text, where the station changed it in a lost group, and
            // a segment the two texts share would not show the change: so
            // then every segment must have come the same twice, as for the
            // station's name.
            bool one_pass = true;
            for (int k = 0; k <= last && k < 16; k++) one_pass = one_pass && rt_slot_pass_[k] == rt_pass_;
            const bool settled = one_pass || (rt_confirmed_ & static_cast<uint32_t>(wanted)) == static_cast<uint32_t>(wanted);
            if (settled && text != state_.rt) {
                state_.rt = text;
                state_.sequence++;
            }
        }
    }
    const int pty = (b >> 5) & 31;
    const bool tp = (b >> 10) & 1;
    if (pty != state_.pty || tp != state_.tp) {
        state_.pty = pty;
        state_.tp = tp;
        state_.sequence++;
    }
    if (pi_confirmations_ >= 1 && last_pi_ != state_.pi) {
        state_.pi = last_pi_;
        state_.sequence++;
    }
    if (ps_ready) {
        const std::string ps = rds_to_utf8(ps_, 8);
        if (ps != state_.ps) {
            state_.ps = ps;
            state_.sequence++;
        }
    }
}

}  // namespace fernsdr

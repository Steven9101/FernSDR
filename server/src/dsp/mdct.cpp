#include "mdct.h"

#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>

namespace fernsdr {

namespace {
constexpr double kPi = 3.14159265358979323846;

std::shared_ptr<const Mdct::Twiddles> shared_twiddles(size_t m) {
    static std::mutex mutex;
    static std::map<size_t, std::weak_ptr<const Mdct::Twiddles>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    std::weak_ptr<const Mdct::Twiddles>& slot = cache[m];
    if (std::shared_ptr<const Mdct::Twiddles> existing = slot.lock()) return existing;
    // See docs/CODEC.md: after pulling the 2*pi*p*k/(M/2) term out of the
    // DCT-IV kernel, the residual phase splits evenly between input and
    // output, leaving (index + 1/8) / M on each side.
    auto made = std::make_shared<Mdct::Twiddles>();
    made->re.resize(m / 2);
    made->im.resize(m / 2);
    for (size_t i = 0; i < m / 2; i++) {
        double angle = -kPi * (static_cast<double>(i) + 0.125) / static_cast<double>(m);
        made->re[i] = static_cast<float>(std::cos(angle));
        made->im[i] = static_cast<float>(std::sin(angle));
    }
    slot = made;
    return made;
}

// Per thread, grown to the longest transform the thread has run: the fold,
// and the DCT-IV's complex work arrays. A transform never calls another, so
// one set serves every plan on the thread.
struct Scratch {
    std::vector<float> fold;
    std::vector<float> re;
    std::vector<float> im;
};

Scratch& scratch(size_t m) {
    thread_local Scratch s;
    if (s.fold.size() < m) {
        s.fold.resize(m);
        s.re.resize(m / 2);
        s.im.resize(m / 2);
    }
    return s;
}

}  // namespace

std::vector<float> make_sine_window(size_t length) {
    std::vector<float> w(length);
    for (size_t n = 0; n < length; n++) {
        w[n] = static_cast<float>(std::sin(kPi / static_cast<double>(length) * (static_cast<double>(n) + 0.5)));
    }
    return w;
}

Mdct::Mdct(size_t half) : m_(half), quarter_(half / 2), fft_(half / 2) {
    if (half < 4 || (half & (half - 1)) != 0) throw std::invalid_argument("Mdct half must be a power of two >= 4");
    twiddles_ = shared_twiddles(half);
}

// DCT-IV: out[k] = sum_n in[n] * cos(pi/M * (n + 0.5) * (k + 0.5))
//
// The complex products are written out: std::complex<float> multiplication
// checks every product for NaN and keeps a library call for that case, which
// stops these loops from vectorising.
void Mdct::dct4(const float* in, float* out, float* re, float* im) const {
    const size_t p = quarter_;
    const float* twiddle_re = twiddles_->re.data();
    const float* twiddle_im = twiddles_->im.data();
    for (size_t i = 0; i < p; i++) {
        const float a = in[2 * i], b = in[m_ - 1 - 2 * i];
        re[i] = a * twiddle_re[i] - b * twiddle_im[i];
        im[i] = a * twiddle_im[i] + b * twiddle_re[i];
    }
    fft_.forward(re, im);
    for (size_t i = 0; i < p; i++) {
        out[2 * i] = re[i] * twiddle_re[i] - im[i] * twiddle_im[i];
        out[m_ - 1 - 2 * i] = -(re[i] * twiddle_im[i] + im[i] * twiddle_re[i]);
    }
}

void Mdct::forward(const float* input, float* output) const {
    const size_t q = m_ / 2;
    // Quarters a=[0,q) b=[q,2q) c=[2q,3q) d=[3q,4q) of the 2M-sample window.
    const float* a = input;
    const float* b = input + q;
    const float* c = input + 2 * q;
    const float* d = input + 3 * q;
    Scratch& work = scratch(m_);
    float* fold = work.fold.data();
    for (size_t i = 0; i < q; i++) {
        fold[i] = -c[q - 1 - i] - d[i];
        fold[q + i] = a[i] - b[q - 1 - i];
    }
    dct4(fold, output, work.re.data(), work.im.data());
}

void Mdct::inverse(const float* input, float* output) const {
    Scratch& work = scratch(m_);
    float* unfold = work.fold.data();
    dct4(input, unfold, work.re.data(), work.im.data());

    const size_t q = m_ / 2;
    const float scale = 2.0f / static_cast<float>(m_);
    // Transpose of the forward fold, which is what the direct IMDCT sum
    // reduces to once the DCT-IV is factored out.
    for (size_t i = 0; i < q; i++) {
        output[i] = scale * unfold[q + i];
        output[q + i] = -scale * unfold[m_ - 1 - i];
        output[m_ + i] = -scale * unfold[q - 1 - i];
        output[m_ + q + i] = -scale * unfold[i];
    }
}

}  // namespace fernsdr

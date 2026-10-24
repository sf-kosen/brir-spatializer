#include "brir/headphone_eq.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <numbers>

namespace brir {

namespace {

struct Biquad {
    double b0, b1, b2, a1, a2;

    std::complex<double> response(double frequency, double sampleRate) const
    {
        std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * frequency / sampleRate);
        return (b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z);
    }
};

// RBJ cookbook coefficients, normalised by a0.
Biquad design(const EqBand& band, double sampleRate)
{
    double a = std::pow(10.0, band.gainDb / 40.0);
    double w0 = 2.0 * std::numbers::pi * band.frequency / sampleRate;
    double cs = std::cos(w0), sn = std::sin(w0);
    double alpha = sn / (2.0 * band.q);
    double b0, b1, b2, a0, a1, a2;
    if (band.type == EqBand::Peak) {
        b0 = 1 + alpha * a, b1 = -2 * cs, b2 = 1 - alpha * a;
        a0 = 1 + alpha / a, a1 = -2 * cs, a2 = 1 - alpha / a;
    } else {
        double sq = 2 * std::sqrt(a) * alpha;
        if (band.type == EqBand::LowShelf) {
            b0 = a * ((a + 1) - (a - 1) * cs + sq), b1 = 2 * a * ((a - 1) - (a + 1) * cs);
            b2 = a * ((a + 1) - (a - 1) * cs - sq);
            a0 = (a + 1) + (a - 1) * cs + sq, a1 = -2 * ((a - 1) + (a + 1) * cs), a2 = (a + 1) + (a - 1) * cs - sq;
        } else {
            b0 = a * ((a + 1) + (a - 1) * cs + sq), b1 = -2 * a * ((a - 1) + (a + 1) * cs);
            b2 = a * ((a + 1) + (a - 1) * cs - sq);
            a0 = (a + 1) - (a - 1) * cs + sq, a1 = 2 * ((a - 1) - (a + 1) * cs), a2 = (a + 1) - (a - 1) * cs - sq;
        }
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

} // namespace

HeadphoneEq::HeadphoneEq(double sampleRate) : sampleRate_(sampleRate) {}

HeadphoneEq::~HeadphoneEq()
{
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
}

void HeadphoneEq::setProfile(std::span<const EqBand> bands, float preampDb)
{
    auto profile = std::make_unique<Profile>();
    profile->preamp = bands.empty() ? 1.0f : std::pow(10.0f, preampDb / 20.0f);
    for (const EqBand& band : bands) {
        if (profile->sections.size() == kMaxSections) break;
        Biquad c = design(band, sampleRate_);
        profile->sections.push_back({c.b0, c.b1, c.b2, c.a1, c.a2});
    }

    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    latest_ = profile.get();
    delete pending_.exchange(profile.release(), std::memory_order_acq_rel); // never picked up
}

void HeadphoneEq::process(float* const* io, int frames) noexcept
{
    // Swap only while the retire slot is free, so the old profile is never
    // deleted here; setProfile() frees it.
    if (pending_.load(std::memory_order_relaxed) && !retired_.load(std::memory_order_acquire)) {
        Profile* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
        retired_.store(active_.release(), std::memory_order_release);
        active_.reset(next);
        std::memset(state_, 0, sizeof(state_));
    }
    if (!active_ || active_->sections.empty()) return;

    const Profile& p = *active_;
    for (int ch = 0; ch < 2; ++ch) {
        float* x = io[ch];
        auto& st = state_[ch];
        for (int i = 0; i < frames; ++i) {
            double v = x[i] * p.preamp;
            for (std::size_t s = 0; s < p.sections.size(); ++s) {
                const Coefficients& c = p.sections[s];
                double y = c.b0 * v + st[s][0];
                st[s][0] = c.b1 * v - c.a1 * y + st[s][1];
                st[s][1] = c.b2 * v - c.a2 * y;
                v = y;
            }
            x[i] = static_cast<float>(v);
        }
    }
}

float HeadphoneEq::responseDb(double frequency) const
{
    if (!latest_) return 0.0f;
    std::complex<double> h = latest_->preamp;
    for (const Coefficients& c : latest_->sections)
        h *= Biquad{c.b0, c.b1, c.b2, c.a1, c.a2}.response(frequency, sampleRate_);
    return static_cast<float>(20.0 * std::log10(std::abs(h)));
}

namespace {

// Magnitude response in dB on a log grid over 20 Hz - 20 kHz.
template <typename Fn>
void sweepDb(std::span<const EqBand> bands, double sampleRate, Fn&& fn)
{
    std::vector<Biquad> sections;
    for (const EqBand& band : bands) sections.push_back(design(band, sampleRate));
    for (double f = 20.0; f <= 20000.0; f *= 1.01) {
        std::complex<double> h = 1.0;
        for (const Biquad& c : sections) h *= c.response(f, sampleRate);
        fn(20.0 * std::log10(std::abs(h)));
    }
}

} // namespace

float HeadphoneEq::peakGainDb(std::span<const EqBand> bands, double sampleRate)
{
    double peak = -1e9;
    sweepDb(bands, sampleRate, [&](double db) { peak = std::max(peak, db); });
    return static_cast<float>(peak);
}

float HeadphoneEq::averageGainDb(std::span<const EqBand> bands, double sampleRate)
{
    double power = 0.0;
    int count = 0;
    sweepDb(bands, sampleRate, [&](double db) { power += std::pow(10.0, db / 10.0), ++count; });
    return static_cast<float>(10.0 * std::log10(power / count));
}

} // namespace brir

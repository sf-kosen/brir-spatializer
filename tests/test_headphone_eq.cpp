#include "brir/headphone_eq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace {

// Steady-state gain of the EQ for a sine, in dB (same on both channels).
double measuredGainDb(brir::HeadphoneEq& eq, double frequency, double rate = 48000.0)
{
    const int n = 48000;
    std::vector<float> l(n), r(n);
    for (int i = 0; i < n; ++i) l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] =
        0.1f * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * i / rate));
    for (int pos = 0; pos < n; pos += 256) {
        float* io[2] = {l.data() + pos, r.data() + pos};
        eq.process(io, std::min(256, n - pos));
    }
    double sum = 0.0;
    for (int i = n / 2; i < n; ++i) sum += static_cast<double>(l[static_cast<std::size_t>(i)]) * l[static_cast<std::size_t>(i)];
    double rms = std::sqrt(sum / (n / 2));
    return 20.0 * std::log10(rms / (0.1 / std::sqrt(2.0)));
}

} // namespace

TEST_CASE("headphone EQ is transparent without a profile")
{
    brir::HeadphoneEq eq(48000.0);
    CHECK(measuredGainDb(eq, 1000.0) == doctest::Approx(0.0).epsilon(0.01).scale(1.0));
    eq.setProfile({}, -6.0f); // empty profile: off, preamp ignored
    CHECK(measuredGainDb(eq, 1000.0) == doctest::Approx(0.0).epsilon(0.01).scale(1.0));
}

TEST_CASE("headphone EQ applies the profile it reports")
{
    const std::array<brir::EqBand, 3> bands{{
        {brir::EqBand::LowShelf, 100.0f, -8.0f, 0.7f},
        {brir::EqBand::Peak, 3000.0f, 4.0f, 1.0f},
        {brir::EqBand::HighShelf, 10000.0f, 2.0f, 0.7f},
    }};
    brir::HeadphoneEq eq(48000.0);
    eq.setProfile(bands, -3.0f);

    CHECK(eq.responseDb(3000.0) == doctest::Approx(4.0 - 3.0).epsilon(0.05).scale(1.0));
    CHECK(eq.responseDb(30.0) == doctest::Approx(-8.0 - 3.0).epsilon(0.3).scale(1.0));
    for (double f : {50.0, 300.0, 1000.0, 3000.0, 8000.0, 15000.0}) {
        CAPTURE(f);
        CHECK(measuredGainDb(eq, f) == doctest::Approx(eq.responseDb(f)).epsilon(0.05).scale(1.0));
    }
}

TEST_CASE("headphone EQ peak gain covers the largest boost")
{
    const std::array<brir::EqBand, 2> bands{{
        {brir::EqBand::LowShelf, 120.0f, 8.0f, 0.7f},
        {brir::EqBand::Peak, 3000.0f, -4.0f, 1.0f},
    }};
    CHECK(brir::HeadphoneEq::peakGainDb(bands, 48000.0) == doctest::Approx(8.0).epsilon(0.02).scale(1.0));
}

TEST_CASE("averageGainDb is the pink-noise level change")
{
    const std::array<brir::EqBand, 1> flat = {{{brir::EqBand::Peak, 1000.0f, 0.0f, 1.0f}}};
    CHECK(brir::HeadphoneEq::averageGainDb(flat, 48000.0) == doctest::Approx(0.0).scale(1.0));
    // A +6 dB shelf above 632 Hz covers half the 20 Hz - 20 kHz decades:
    // 10*log10((1 + 4) / 2) = +4 dB.
    const std::array<brir::EqBand, 1> shelf = {{{brir::EqBand::HighShelf, 632.0f, 6.0206f, 0.707f}}};
    CHECK(brir::HeadphoneEq::averageGainDb(shelf, 48000.0) == doctest::Approx(3.98).epsilon(0.05).scale(1.0));
}

#include "brir/upmixer.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <numeric>
#include <random>
#include <vector>

using brir::Upmixer;

namespace {

constexpr double kRate = 48000.0;
constexpr int kLength = 96000;
constexpr int kSkip = 9600; // let the covariance estimates settle

using Feeds = std::array<std::vector<float>, Upmixer::kFeeds>;

std::vector<float> noise(unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, 0.1f);
    std::vector<float> s(kLength);
    for (float& v : s) v = dist(rng);
    return s;
}

Feeds run(Upmixer& upmixer, const std::vector<float>& left, const std::vector<float>& right, int block = 300)
{
    Feeds feeds;
    for (auto& f : feeds) f.assign(kLength, 0.0f);
    for (int pos = 0; pos < kLength; pos += block) {
        int n = std::min(block, kLength - pos);
        const float* in[2] = {left.data() + pos, right.data() + pos};
        float* out[Upmixer::kFeeds];
        for (int f = 0; f < Upmixer::kFeeds; ++f) out[f] = feeds[static_cast<std::size_t>(f)].data() + pos;
        upmixer.process(in, out, n);
    }
    return feeds;
}

double energy(const std::vector<float>& s)
{
    double e = 0.0;
    for (std::size_t i = kSkip; i < s.size(); ++i) e += static_cast<double>(s[i]) * s[i];
    return e;
}

// Share of total output energy in each feed.
std::array<double, Upmixer::kFeeds> shares(const Feeds& feeds)
{
    std::array<double, Upmixer::kFeeds> e{};
    double total = 0.0;
    for (int f = 0; f < Upmixer::kFeeds; ++f) total += e[static_cast<std::size_t>(f)] = energy(feeds[static_cast<std::size_t>(f)]);
    for (double& v : e) v /= total;
    return e;
}

} // namespace

TEST_CASE("disabled upmixer passes stereo through with one frame of latency")
{
    Upmixer upmixer(kRate);
    upmixer.setEnabled(false);
    CHECK(upmixer.latencySamples() == 512);
    std::vector<float> l = noise(1), r = noise(2);
    Feeds out = run(upmixer, l, r, 333);
    int d = upmixer.latencySamples();
    for (int i = d; i < kLength; ++i) {
        REQUIRE(out[Upmixer::L][static_cast<std::size_t>(i)] == doctest::Approx(l[static_cast<std::size_t>(i - d)]).epsilon(1e-4));
        REQUIRE(out[Upmixer::R][static_cast<std::size_t>(i)] == doctest::Approx(r[static_cast<std::size_t>(i - d)]).epsilon(1e-4));
    }
    CHECK(energy(out[Upmixer::C]) < 1e-9);
    CHECK(energy(out[Upmixer::Ls]) < 1e-9);
}

TEST_CASE("frame size scales with sample rate to keep latency constant")
{
    CHECK(Upmixer(44100.0).frameSize() == 512);
    CHECK(Upmixer(96000.0).frameSize() == 1024);
    CHECK(Upmixer(192000.0).frameSize() == 2048);
}

TEST_CASE("sources are steered by position and phase")
{
    std::vector<float> s = noise(3), silent(kLength, 0.0f);

    SUBCASE("hard left stays left")
    {
        Upmixer upmixer(kRate);
        auto e = shares(run(upmixer, s, silent));
        CHECK(e[Upmixer::L] > 0.99);
    }
    SUBCASE("centred mono goes to the centre channel")
    {
        Upmixer upmixer(kRate);
        auto e = shares(run(upmixer, s, s));
        CHECK(e[Upmixer::C] > 0.97);
    }
    SUBCASE("half-left source sits between L and C")
    {
        Upmixer upmixer(kRate);
        float pan = 0.25f * static_cast<float>(std::numbers::pi) / 2.0f; // a quarter of the way from left to right
        std::vector<float> l(kLength), r(kLength);
        for (int i = 0; i < kLength; ++i) {
            l[static_cast<std::size_t>(i)] = s[static_cast<std::size_t>(i)] * std::cos(pan);
            r[static_cast<std::size_t>(i)] = s[static_cast<std::size_t>(i)] * std::sin(pan);
        }
        auto e = shares(run(upmixer, l, r));
        CHECK(e[Upmixer::L] + e[Upmixer::C] > 0.97);
        CHECK(e[Upmixer::L] == doctest::Approx(0.5).epsilon(0.1));
        CHECK(e[Upmixer::R] < 0.01);
    }
    SUBCASE("out-of-phase mono goes to the surrounds")
    {
        Upmixer upmixer(kRate);
        std::vector<float> inverted(kLength);
        for (int i = 0; i < kLength; ++i) inverted[static_cast<std::size_t>(i)] = -s[static_cast<std::size_t>(i)];
        auto e = shares(run(upmixer, s, inverted));
        CHECK(e[Upmixer::Ls] + e[Upmixer::Rs] > 0.95);
    }
    SUBCASE("centre amount 0 keeps a phantom centre")
    {
        Upmixer upmixer(kRate);
        upmixer.setCenterAmount(0.0f);
        auto e = shares(run(upmixer, s, s));
        CHECK(e[Upmixer::C] < 0.01);
        CHECK(e[Upmixer::L] == doctest::Approx(0.5).epsilon(0.05));
    }
}

TEST_CASE("decorrelated ambience is split between front and surrounds")
{
    Upmixer upmixer(kRate);
    upmixer.setSurroundAmount(0.6f);
    auto e = shares(run(upmixer, noise(4), noise(5)));
    double surround = e[Upmixer::Ls] + e[Upmixer::Rs];
    CHECK(surround > 0.45);
    CHECK(surround < 0.65);
    CHECK(e[Upmixer::C] < 0.05);
}

TEST_CASE("total energy is preserved for a mixed scene")
{
    Upmixer upmixer(kRate);
    std::vector<float> dialog = noise(6), ambL = noise(7), ambR = noise(8), effect = noise(9);
    std::vector<float> l(kLength), r(kLength);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kLength); ++i) {
        l[i] = dialog[i] + 0.5f * ambL[i] + 0.4f * effect[i];
        r[i] = dialog[i] + 0.5f * ambR[i] - 0.4f * effect[i];
    }
    Feeds out = run(upmixer, l, r);
    double in = energy(l) + energy(r);
    double total = 0.0;
    for (const auto& f : out) total += energy(f);
    CHECK(10.0 * std::log10(total / in) == doctest::Approx(0.0).epsilon(0.25).scale(1.0));
}

TEST_CASE("out-of-phase sound reaches the two surround arrays decorrelated")
{
    // A single signal sent identically to mirror-image arrays would image inside
    // the head; the comb-filter decorrelation must keep the arrays dissimilar.
    Upmixer upmixer(kRate);
    std::vector<float> s = noise(10), inverted(kLength);
    for (int i = 0; i < kLength; ++i) inverted[static_cast<std::size_t>(i)] = -s[static_cast<std::size_t>(i)];
    Feeds out = run(upmixer, s, inverted);

    const auto& ls = out[Upmixer::Ls];
    const auto& rs = out[Upmixer::Rs];
    double cross = 0.0;
    for (std::size_t i = kSkip; i < ls.size(); ++i) cross += static_cast<double>(ls[i]) * rs[i];
    double correlation = cross / std::sqrt(energy(ls) * energy(rs));
    CHECK(std::abs(correlation) < 0.2);
    CHECK(energy(ls) == doctest::Approx(energy(rs)).epsilon(0.1));
}

#include "brir/limiter.h"
#include "brir/renderer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

namespace {

using Stereo = std::vector<std::vector<float>>;

Stereo sine(int frames, float amplitude, double frequency = 1000.0, double rate = 48000.0)
{
    Stereo s(2, std::vector<float>(static_cast<std::size_t>(frames)));
    for (int i = 0; i < frames; ++i) {
        float v = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * i / rate));
        s[0][static_cast<std::size_t>(i)] = v;
        s[1][static_cast<std::size_t>(i)] = -0.5f * v;
    }
    return s;
}

// Runs a whole signal through in blocks, in place.
template <typename Fn>
void inBlocks(Stereo& s, int block, Fn fn)
{
    int total = static_cast<int>(s[0].size());
    for (int pos = 0; pos < total; pos += block) {
        int n = std::min(block, total - pos);
        float* io[2] = {s[0].data() + pos, s[1].data() + pos};
        fn(io, n);
    }
}

std::unique_ptr<brir::Convolver> identityConvolver(float gain = 1.0f)
{
    brir::IrMatrix irs{{{gain}, {}}, {{}, {gain}}};
    return std::make_unique<brir::Convolver>(2, 2, irs);
}

} // namespace

TEST_CASE("limiter holds the ceiling and is transparent below it")
{
    brir::Limiter limiter(2, 48000.0, -1.0f);
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);

    Stereo hot = sine(48000, 2.0f);
    inBlocks(hot, 480, [&](float* const* io, int n) { limiter.process(io, n); });
    float peak = 0.0f;
    for (const auto& ch : hot)
        for (float v : ch) peak = std::max(peak, std::abs(v));
    CHECK(peak <= ceiling + 1e-6f);
    CHECK(peak > ceiling * 0.95f);

    brir::Limiter fresh(2, 48000.0, -1.0f);
    Stereo quiet = sine(4800, 0.25f);
    Stereo original = quiet;
    inBlocks(quiet, 100, [&](float* const* io, int n) { fresh.process(io, n); });
    int delay = fresh.latencySamples();
    for (std::size_t i = static_cast<std::size_t>(delay); i < quiet[0].size(); ++i)
        REQUIRE(quiet[0][i] == doctest::Approx(original[0][i - static_cast<std::size_t>(delay)]).epsilon(1e-6));
}

TEST_CASE("renderer output is the convolved input, delayed by its reported latency")
{
    brir::Renderer renderer(48000.0, 512);
    REQUIRE(renderer.setConvolver(identityConvolver(0.5f)));

    Stereo signal = sine(9600, 0.5f);
    Stereo original = signal;
    inBlocks(signal, 480, [&](float* const* io, int n) { renderer.process(io, io, n); });

    int delay = renderer.latencySamples();
    CHECK(delay == 128 + 72);
    for (std::size_t i = static_cast<std::size_t>(delay); i < signal[0].size(); ++i) {
        REQUIRE(signal[0][i] == doctest::Approx(0.5f * original[0][i - static_cast<std::size_t>(delay)]).epsilon(1e-4));
        REQUIRE(signal[1][i] == doctest::Approx(0.5f * original[1][i - static_cast<std::size_t>(delay)]).epsilon(1e-4));
    }
}

TEST_CASE("bypass is time-aligned with the processed path")
{
    brir::Renderer renderer(48000.0, 512);
    REQUIRE(renderer.setConvolver(identityConvolver()));
    Stereo warmup = sine(4800, 0.3f);
    inBlocks(warmup, 480, [&](float* const* io, int n) { renderer.process(io, io, n); });

    renderer.setBypass(true);
    Stereo signal = sine(9600, 0.3f);
    Stereo original = signal;
    inBlocks(signal, 480, [&](float* const* io, int n) { renderer.process(io, io, n); });

    // Identity IR: processed and bypassed paths are equal, so the fade between
    // them must be seamless.
    int delay = renderer.latencySamples();
    for (std::size_t i = static_cast<std::size_t>(delay); i < signal[0].size(); ++i)
        REQUIRE(signal[0][i] == doctest::Approx(original[0][i - static_cast<std::size_t>(delay)]).epsilon(1e-4));
}

TEST_CASE("swapping convolvers crossfades and retires the old one")
{
    brir::Renderer renderer(48000.0, 512);
    REQUIRE(renderer.setConvolver(identityConvolver(1.0f)));
    Stereo signal = sine(4800, 0.2f);
    inBlocks(signal, 480, [&](float* const* io, int n) { renderer.process(io, io, n); });

    REQUIRE(renderer.setConvolver(identityConvolver(0.5f)));
    CHECK_FALSE(renderer.setConvolver(identityConvolver(0.25f))); // previous swap still pending

    Stereo after = sine(9600, 0.2f);
    Stereo original = after;
    inBlocks(after, 480, [&](float* const* io, int n) { renderer.process(io, io, n); });
    renderer.releaseRetired();

    // Settled at the new gain, with no step larger than the signal allows.
    int delay = renderer.latencySamples();
    std::size_t last = after[0].size() - 1;
    CHECK(after[0][last] == doctest::Approx(0.5f * original[0][last - static_cast<std::size_t>(delay)]).epsilon(1e-3));
    float maxStep = 0.0f;
    for (std::size_t i = 1; i < after[0].size(); ++i) maxStep = std::max(maxStep, std::abs(after[0][i] - after[0][i - 1]));
    CHECK(maxStep < 0.2f * 2.0f * static_cast<float>(std::numbers::pi) * 1000.0f / 48000.0f * 1.05f);
    CHECK(renderer.setConvolver(identityConvolver(0.25f)));
}

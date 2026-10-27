#include "brir/leveler.h"
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

// Feeds L -> left ear and R -> right ear with a plain gain; other feeds unused.
// Pair it with a disabled upmixer so L/R pass straight through.
std::unique_ptr<brir::Convolver> identityConvolver(float gain = 1.0f)
{
    brir::IrMatrix irs(brir::Renderer::kFeeds, std::vector<std::vector<float>>(2));
    irs[brir::Upmixer::L][0] = {gain};
    irs[brir::Upmixer::R][1] = {gain};
    return std::make_unique<brir::Convolver>(brir::Renderer::kFeeds, 2, irs);
}

std::unique_ptr<brir::Renderer> passthroughRenderer()
{
    auto renderer = std::make_unique<brir::Renderer>(48000.0, 512);
    renderer->upmixer().setEnabled(false);
    return renderer;
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

TEST_CASE("leveler lowers a loud passage to the threshold, holds, then recovers")
{
    brir::Leveler leveler(2, 48000.0, -2.0f, 30.0f, 1000.0f, 4.0f);
    const float threshold = std::pow(10.0f, -2.0f / 20.0f);
    auto peakOf = [](const Stereo& s, std::size_t from, std::size_t to) {
        float peak = 0.0f;
        for (const auto& ch : s)
            for (std::size_t i = from; i < to; ++i) peak = std::max(peak, std::abs(ch[i]));
        return peak;
    };

    // 2 s at +6 dBFS: settles at threshold / 2 (about -8 dB) and stays there.
    Stereo loud = sine(96000, 2.0f);
    inBlocks(loud, 480, [&](float* const* io, int n) { leveler.process(io, n); });
    CHECK(leveler.currentGainDb() == doctest::Approx(20.0f * std::log10(threshold / 2.0f)).epsilon(0.01));
    CHECK(peakOf(loud, 48000, 96000) <= threshold * 1.001f);

    // Quiet afterwards: held for the hold time, then up at 4 dB/s.
    Stereo quiet = sine(48000 * 4, 0.1f);
    float afterHold = 0.0f;
    inBlocks(quiet, 480, [&, done = 0](float* const* io, int n) mutable {
        leveler.process(io, n);
        done += n;
        if (done == 43200) afterHold = leveler.currentGainDb(); // 0.9 s in: still held
    });
    CHECK(afterHold == doctest::Approx(20.0f * std::log10(threshold / 2.0f)).epsilon(0.01));
    CHECK(leveler.currentGainDb() == doctest::Approx(0.0f).epsilon(1e-6)); // 8 dB back within 3 s

    // Material below the threshold passes bit-exact.
    brir::Leveler fresh(2, 48000.0);
    Stereo below = sine(4800, 0.7f);
    Stereo original = below;
    inBlocks(below, 100, [&](float* const* io, int n) { fresh.process(io, n); });
    CHECK(below == original);
}

TEST_CASE("disabled leveler recovers to unity without a jump")
{
    brir::Leveler leveler(2, 48000.0, -2.0f, 30.0f, 1000.0f, 4.0f);
    Stereo loud = sine(48000, 2.0f);
    inBlocks(loud, 480, [&](float* const* io, int n) { leveler.process(io, n); });
    REQUIRE(leveler.currentGainDb() < -7.0f);
    leveler.setEnabled(false);
    Stereo still = sine(480, 2.0f);
    inBlocks(still, 480, [&](float* const* io, int n) { leveler.process(io, n); });
    CHECK(leveler.currentGainDb() < -7.0f); // 10 ms later: only 0.04 dB up
    Stereo rest = sine(48000 * 3, 2.0f);
    inBlocks(rest, 480, [&](float* const* io, int n) { leveler.process(io, n); });
    CHECK(leveler.currentGainDb() == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("renderer output is the convolved input, delayed by its reported latency")
{
    auto rendererPtr = passthroughRenderer();
    brir::Renderer& renderer = *rendererPtr;
    REQUIRE(renderer.setConvolver(identityConvolver(0.5f)));

    Stereo signal = sine(9600, 0.5f);
    Stereo original = signal;
    inBlocks(signal, 480, [&](float* const* io, int n) { renderer.process(io, io, n); });

    int delay = renderer.latencySamples();
    CHECK(delay == 512 + 128 + 72);
    for (std::size_t i = static_cast<std::size_t>(delay); i < signal[0].size(); ++i) {
        REQUIRE(signal[0][i] == doctest::Approx(0.5f * original[0][i - static_cast<std::size_t>(delay)]).epsilon(1e-4));
        REQUIRE(signal[1][i] == doctest::Approx(0.5f * original[1][i - static_cast<std::size_t>(delay)]).epsilon(1e-4));
    }
}

TEST_CASE("bypass is time-aligned with the processed path")
{
    auto rendererPtr = passthroughRenderer();
    brir::Renderer& renderer = *rendererPtr;
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
    auto rendererPtr = passthroughRenderer();
    brir::Renderer& renderer = *rendererPtr;
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

TEST_CASE("renderer upmixes before convolving")
{
    // Only the centre feed reaches the ears: a centred source must come through,
    // a hard-left one must not.
    brir::IrMatrix irs(brir::Renderer::kFeeds, std::vector<std::vector<float>>(2));
    irs[brir::Upmixer::C][0] = {1.0f};
    irs[brir::Upmixer::C][1] = {1.0f};
    brir::Renderer renderer(48000.0, 512);
    REQUIRE(renderer.setConvolver(std::make_unique<brir::Convolver>(brir::Renderer::kFeeds, 2, irs)));

    auto measure = [&](float leftGain, float rightGain) {
        Stereo s = sine(48000, 0.2f, 500.0);
        for (auto& v : s[1]) v = 0.0f;
        for (std::size_t i = 0; i < s[0].size(); ++i) {
            float v = s[0][i];
            s[0][i] = v * leftGain;
            s[1][i] = v * rightGain;
        }
        inBlocks(s, 480, [&](float* const* io, int n) { renderer.process(io, io, n); });
        double e = 0.0;
        for (std::size_t i = 24000; i < s[0].size(); ++i) e += s[0][i] * s[0][i];
        return e;
    };
    double centred = measure(1.0f, 1.0f);
    double left = measure(1.0f, 0.0f);
    CHECK(centred > 100.0 * left);
}

TEST_CASE("bass management moves the lows to LFE and keeps the sum flat")
{
    // Upmix off: the left channel arrives on feed L. Measures the steady-state
    // RMS at the left ear relative to the input, with or without the LFE feed
    // reaching it.
    auto measure = [](double frequency, bool lfeAudible, float subGainDb) {
        brir::IrMatrix irs(brir::Renderer::kFeeds, std::vector<std::vector<float>>(2));
        irs[brir::Upmixer::L][0] = {1.0f};
        if (lfeAudible) irs[brir::Renderer::kLfe][0] = {1.0f};
        auto renderer = passthroughRenderer();
        renderer->setBassManagement(true, subGainDb);
        REQUIRE(renderer->setConvolver(std::make_unique<brir::Convolver>(brir::Renderer::kFeeds, 2, irs)));
        Stereo s = sine(48000, 0.2f, frequency);
        std::fill(s[1].begin(), s[1].end(), 0.0f); // the LFE feed sums both sides
        inBlocks(s, 480, [&](float* const* io, int n) { renderer->process(io, io, n); });
        double e = 0.0;
        for (std::size_t i = 24000; i < s[0].size(); ++i) e += s[0][i] * s[0][i];
        double rms = std::sqrt(e / 24000.0);
        return 20.0 * std::log10(rms / (0.2 / std::sqrt(2.0)));
    };
    CHECK(measure(40.0, true, 0.0f) == doctest::Approx(0.0).epsilon(0.05).scale(1.0));
    CHECK(measure(80.0, true, 0.0f) == doctest::Approx(0.0).epsilon(0.05).scale(1.0));
    CHECK(measure(1000.0, true, 0.0f) == doctest::Approx(0.0).epsilon(0.05).scale(1.0));
    CHECK(measure(30.0, false, 0.0f) < -20.0);    // mains no longer carry deep bass
    CHECK(measure(1000.0, false, 0.0f) > -0.1);   // nor lose the mid range
    CHECK(measure(30.0, true, 6.0f) == doctest::Approx(6.0).epsilon(0.05).scale(1.0));
}

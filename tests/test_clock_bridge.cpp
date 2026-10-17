#include "brir/clock_bridge.h"
#include "brir/spsc_ring.h"

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using brir::ClockBridge;
using brir::ClockBridgeConfig;
using brir::SpscRing;

namespace {

constexpr double kPi = std::numbers::pi;

// Generates an interleaved stereo sine at the producer's rate, continuing across calls.
struct SineSource {
    double frequency;
    double sampleRate;
    long long index = 0;

    std::vector<float> next(int frames)
    {
        std::vector<float> data(static_cast<std::size_t>(frames) * 2);
        for (int f = 0; f < frames; ++f, ++index) {
            float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * frequency * index / sampleRate));
            data[static_cast<std::size_t>(f) * 2] = v;
            data[static_cast<std::size_t>(f) * 2 + 1] = -v;
        }
        return data;
    }
};

struct StereoCapture {
    std::vector<float> left, right;

    void read(ClockBridge& bridge, int frames)
    {
        std::vector<float> l(static_cast<std::size_t>(frames)), r(static_cast<std::size_t>(frames));
        float* out[2] = {l.data(), r.data()};
        bridge.read(out, frames);
        left.insert(left.end(), l.begin(), l.end());
        right.insert(right.end(), r.begin(), r.end());
    }
};

// Least-squares fit of a*cos + b*sin + c at a known frequency; returns the
// residual RMS relative to the fitted amplitude, in dB.
double sineResidualDb(const std::vector<float>& signal, std::size_t start, std::size_t count, double cyclesPerSample)
{
    double s[3][3] = {}, r[3] = {};
    for (std::size_t i = start; i < start + count; ++i) {
        double w = 2.0 * kPi * cyclesPerSample * static_cast<double>(i);
        double basis[3] = {std::cos(w), std::sin(w), 1.0};
        for (int a = 0; a < 3; ++a) {
            r[a] += basis[a] * signal[i];
            for (int b = 0; b < 3; ++b) s[a][b] += basis[a] * basis[b];
        }
    }
    // Solve the 3x3 normal equations by Gaussian elimination.
    for (int col = 0; col < 3; ++col) {
        for (int row = col + 1; row < 3; ++row) {
            double factor = s[row][col] / s[col][col];
            for (int k = col; k < 3; ++k) s[row][k] -= factor * s[col][k];
            r[row] -= factor * r[col];
        }
    }
    double coef[3];
    for (int row = 2; row >= 0; --row) {
        double sum = r[row];
        for (int k = row + 1; k < 3; ++k) sum -= s[row][k] * coef[k];
        coef[row] = sum / s[row][row];
    }

    double residual = 0.0;
    for (std::size_t i = start; i < start + count; ++i) {
        double w = 2.0 * kPi * cyclesPerSample * static_cast<double>(i);
        double e = signal[i] - (coef[0] * std::cos(w) + coef[1] * std::sin(w) + coef[2]);
        residual += e * e;
    }
    double amplitude = std::hypot(coef[0], coef[1]);
    return 20.0 * std::log10(std::sqrt(residual / static_cast<double>(count)) / (amplitude / std::sqrt(2.0)));
}

// Streams a sine through a bridge with fixed drift: producer writes inFrames,
// consumer reads outFrames per period.
std::vector<float> resampleSine(double inRate, double outRate, double driftPpm, double frequency, int periods)
{
    ClockBridgeConfig config;
    config.inputRate = inRate;
    config.outputRate = outRate;
    ClockBridge bridge(config);
    bridge.setFixedDriftPpm(driftPpm);

    SineSource source{frequency, inRate};
    StereoCapture capture;
    int inFrames = static_cast<int>(inRate / 100.0);
    int outFrames = static_cast<int>(outRate / 100.0);
    for (int p = 0; p < 3; ++p) bridge.write(source.next(inFrames).data(), inFrames);
    for (int p = 0; p < periods; ++p) {
        bridge.write(source.next(inFrames).data(), inFrames);
        capture.read(bridge, outFrames);
    }
    CHECK(bridge.stats().underruns == 0);
    return capture.left;
}

struct DriftSimulation {
    double driftPpm;
    int burstFrames = 480;   // capture delivery size
    int callbackFrames = 128; // output callback size
    double seconds = 200.0;
    double pauseStart = -1.0; // producer silent between pauseStart and pauseEnd
    double pauseEnd = -1.0;
};

brir::ClockBridgeStats simulateDrift(const DriftSimulation& sim, std::vector<double>* driftTrace = nullptr)
{
    ClockBridge bridge(ClockBridgeConfig{});
    SineSource source{440.0, 48000.0};
    std::vector<float> l(static_cast<std::size_t>(sim.callbackFrames)), r(static_cast<std::size_t>(sim.callbackFrames));
    float* out[2] = {l.data(), r.data()};

    // Timestamps carry scheduling jitter, as real ones do.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> jitter(-0.3e-3, 0.3e-3);
    const double producerRate = 48000.0 * (1.0 + sim.driftPpm * 1e-6);

    long long produced = 0;
    long long callbacks = static_cast<long long>(sim.seconds * 48000.0 / sim.callbackFrames);
    for (long long c = 0; c < callbacks; ++c) {
        double t = static_cast<double>(c * sim.callbackFrames) / 48000.0;
        bool paused = t >= sim.pauseStart && t < sim.pauseEnd;
        double due = t * producerRate;
        while (!paused && produced + sim.burstFrames <= due) {
            double firstFrameTime = static_cast<double>(produced) / producerRate + jitter(rng);
            bridge.write(source.next(sim.burstFrames).data(), sim.burstFrames, firstFrameTime);
            produced += sim.burstFrames;
        }
        if (paused) produced = static_cast<long long>(due);
        bridge.read(out, sim.callbackFrames, t + jitter(rng));
        if (driftTrace) driftTrace->push_back(bridge.stats().driftPpm);
    }
    return bridge.stats();
}

} // namespace

TEST_CASE("SpscRing wraps and reports capacity")
{
    SpscRing ring(2, 5);
    CHECK(ring.capacity() == 8);
    float in[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    float out[16] = {};
    CHECK(ring.write(in, 6) == 6);
    CHECK(ring.read(out, 4) == 4);
    CHECK(ring.write(in, 6) == 6); // wraps
    CHECK(ring.write(in, 1) == 0); // full
    CHECK(ring.available() == 8);
    CHECK(ring.read(out, 8) == 8);
    CHECK(out[0] == 9.0f);
    CHECK(out[4] == 1.0f);
    CHECK(out[15] == 12.0f);
}

TEST_CASE("resampler is clean at a fixed drift")
{
    constexpr double ratio = 1.0 + 100e-6;
    std::vector<float> out = resampleSine(48000.0, 48000.0, 100.0, 1000.0, 200);
    CHECK(sineResidualDb(out, 9600, 48000, 1000.0 * ratio / 48000.0) < -90.0);

    out = resampleSine(48000.0, 48000.0, 100.0, 15000.0, 200);
    CHECK(sineResidualDb(out, 9600, 48000, 15000.0 * ratio / 48000.0) < -80.0);
}

TEST_CASE("resampler converts between nominal rates")
{
    std::vector<float> out = resampleSine(44100.0, 48000.0, 0.0, 1000.0, 200);
    CHECK(sineResidualDb(out, 9600, 48000, 1000.0 / 48000.0) < -90.0);

    out = resampleSine(48000.0, 44100.0, 0.0, 15000.0, 200);
    CHECK(sineResidualDb(out, 8820, 44100, 15000.0 / 44100.0) < -80.0);
}

TEST_CASE("controller locks onto clock drift without underruns")
{
    for (double ppm : {200.0, -300.0, 0.0}) {
        CAPTURE(ppm);
        std::vector<double> trace;
        brir::ClockBridgeStats stats = simulateDrift({ppm}, &trace);
        CHECK(stats.underruns == 0);
        CHECK(stats.resyncs == 0);
        CHECK(stats.running);
        CHECK(std::abs(stats.fillMs - stats.targetMs) < 1.0);

        // Once locked, the estimate must be accurate on average and steady.
        // Timestamp jitter leaves a small wobble; 100 ppm is 0.17 cents, far
        // below the few cents at which pitch changes become audible.
        double lo = 1e9, hi = -1e9, sum = 0.0;
        std::size_t half = trace.size() / 2;
        for (std::size_t i = half; i < trace.size(); ++i) {
            lo = std::min(lo, trace[i]);
            hi = std::max(hi, trace[i]);
            sum += trace[i];
        }
        CHECK(std::abs(sum / static_cast<double>(trace.size() - half) - ppm) < 3.0);
        CHECK(hi - lo < 100.0);
    }
}

TEST_CASE("recovers from an input dropout")
{
    DriftSimulation sim{100.0};
    sim.seconds = 60.0;
    sim.pauseStart = 20.0;
    sim.pauseEnd = 20.2;
    brir::ClockBridgeStats stats = simulateDrift(sim);
    CHECK(stats.underruns == 1);
    CHECK(stats.running);
}

TEST_CASE("target latency follows the delivery pattern")
{
    // Gap between timeline and buffer is about one burst plus one callback;
    // the target adds the 3 ms safety margin on top.
    struct Case {
        int burst;
        double expectedMs;
    };
    for (Case c : {Case{480, 3.0 + 10.0 + 2.67}, Case{96, 3.0 + 2.0 + 2.67}}) {
        CAPTURE(c.burst);
        DriftSimulation sim{150.0};
        sim.burstFrames = c.burst;
        sim.seconds = 120.0;
        brir::ClockBridgeStats stats = simulateDrift(sim);
        CHECK(stats.underruns == 0);
        CHECK(stats.targetMs == doctest::Approx(c.expectedMs).epsilon(0.1));
    }
}

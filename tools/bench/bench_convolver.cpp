// Measures convolver cost for the target workload: 5 speaker feeds (L C R Ls Rs)
// x 2 ears, 2.5 s cinema BRIRs at 48 kHz, 128-sample callbacks.
//
//   1. Inline: total CPU cost as a fraction of real time.
//   2. Threaded, paced at real time: audio-thread callback times and late blocks.

#include "brir/convolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kInputs = 5;
constexpr int kOutputs = 2;
constexpr int kIrLength = kSampleRate * 5 / 2;
constexpr int kCallback = 128;

using Clock = std::chrono::steady_clock;

brir::IrMatrix makeIrs()
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    brir::IrMatrix irs(kInputs, std::vector<std::vector<float>>(kOutputs));
    for (auto& row : irs) {
        for (auto& ir : row) {
            ir.resize(kIrLength);
            for (int i = 0; i < kIrLength; ++i) ir[i] = 0.1f * dist(rng) * std::exp(-i / (0.4f * kSampleRate));
        }
    }
    return irs;
}

struct Buffers {
    std::vector<std::vector<float>> in, out;
    std::vector<const float*> inPtrs;
    std::vector<float*> outPtrs;

    Buffers()
        : in(kInputs, std::vector<float>(kCallback)), out(kOutputs, std::vector<float>(kCallback))
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
        for (auto& ch : in)
            for (float& s : ch) s = dist(rng);
        for (auto& ch : in) inPtrs.push_back(ch.data());
        for (auto& ch : out) outPtrs.push_back(ch.data());
    }
};

double toMicros(Clock::duration d)
{
    return std::chrono::duration<double, std::micro>(d).count();
}

void benchInline(const brir::IrMatrix& irs)
{
    brir::ConvolverConfig config;
    config.threaded = false;
    brir::Convolver conv(kInputs, kOutputs, irs, config);
    Buffers buffers;

    constexpr int seconds = 10;
    constexpr int callbacks = seconds * kSampleRate / kCallback;
    double worst = 0.0;
    auto start = Clock::now();
    for (int i = 0; i < callbacks; ++i) {
        auto t0 = Clock::now();
        conv.process(buffers.inPtrs.data(), buffers.outPtrs.data(), kCallback);
        worst = std::max(worst, toMicros(Clock::now() - t0));
    }
    double total = std::chrono::duration<double>(Clock::now() - start).count();

    std::printf("[inline]   stages=%d  CPU=%.2f%% of real time  worst callback=%.0f us\n",
                conv.numStages(), 100.0 * total / seconds, worst);
}

void benchThreadedRealtime(const brir::IrMatrix& irs)
{
    brir::Convolver conv(kInputs, kOutputs, irs, {});
    Buffers buffers;

    constexpr int seconds = 5;
    constexpr int callbacks = seconds * kSampleRate / kCallback;
    const auto period = std::chrono::nanoseconds(1'000'000'000LL * kCallback / kSampleRate);
    std::vector<double> times;
    times.reserve(callbacks);

    auto deadline = Clock::now();
    for (int i = 0; i < callbacks; ++i) {
        deadline += period;
        auto t0 = Clock::now();
        conv.process(buffers.inPtrs.data(), buffers.outPtrs.data(), kCallback);
        times.push_back(toMicros(Clock::now() - t0));
        while (Clock::now() < deadline) {
        }
    }

    std::sort(times.begin(), times.end());
    double budget = toMicros(period);
    std::printf("[threaded] audio thread: median=%.0f us  p99.9=%.0f us  max=%.0f us  (budget %.0f us)  late blocks=%llu\n",
                times[times.size() / 2], times[times.size() * 999 / 1000], times.back(), budget,
                static_cast<unsigned long long>(conv.lateBlockCount()));
}

} // namespace

int main()
{
    std::printf("%d in x %d out, IR %d samples (%.1f s), callback %d, latency %d samples\n", kInputs, kOutputs,
                kIrLength, kIrLength / static_cast<double>(kSampleRate), kCallback, 128);
    brir::IrMatrix irs = makeIrs();
    benchInline(irs);
    benchThreadedRealtime(irs);
    return 0;
}

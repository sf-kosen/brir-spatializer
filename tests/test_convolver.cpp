#include "brir/convolver.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <vector>

using brir::Convolver;
using brir::ConvolverConfig;
using brir::IrMatrix;

namespace {

using Signal = std::vector<float>;
using Planar = std::vector<Signal>;

Signal noise(std::size_t length, unsigned seed, float decaySamples = 0.0f)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    Signal s(length);
    for (std::size_t i = 0; i < length; ++i) {
        float envelope = decaySamples > 0.0f ? std::exp(-static_cast<float>(i) / decaySamples) : 1.0f;
        s[i] = dist(rng) * envelope;
    }
    return s;
}

// Reference: out[o][t] = sum_i (x[i] * h[i][o])[t], truncated to the input length.
Planar directConvolve(const Planar& x, const IrMatrix& h, int numOutputs)
{
    std::size_t length = x.front().size();
    Planar y(static_cast<std::size_t>(numOutputs), Signal(length, 0.0f));
    for (std::size_t in = 0; in < x.size(); ++in) {
        for (int out = 0; out < numOutputs; ++out) {
            const Signal& ir = h[in][static_cast<std::size_t>(out)];
            Signal& dst = y[static_cast<std::size_t>(out)];
            for (std::size_t t = 0; t < length; ++t) {
                double sum = 0.0;
                std::size_t kMax = std::min(ir.size(), t + 1);
                for (std::size_t k = 0; k < kMax; ++k) sum += static_cast<double>(ir[k]) * x[in][t - k];
                dst[t] += static_cast<float>(sum);
            }
        }
    }
    return y;
}

// Feeds x through the convolver with a repeating pattern of callback sizes.
Planar run(Convolver& conv, const Planar& x, const std::vector<int>& callbackSizes, bool waitEachCallback = false)
{
    std::size_t length = x.front().size();
    Planar y(static_cast<std::size_t>(conv.numOutputs()), Signal(length, 0.0f));
    std::vector<const float*> in(x.size());
    std::vector<float*> out(y.size());
    std::size_t position = 0;
    std::size_t call = 0;
    while (position < length) {
        int n = static_cast<int>(std::min<std::size_t>(callbackSizes[call++ % callbackSizes.size()], length - position));
        for (std::size_t i = 0; i < x.size(); ++i) in[i] = x[i].data() + position;
        for (std::size_t o = 0; o < y.size(); ++o) out[o] = y[o].data() + position;
        conv.process(in.data(), out.data(), n);
        if (waitEachCallback) conv.waitForWorkers();
        position += static_cast<std::size_t>(n);
    }
    return y;
}

// Largest error between actual[t] and reference[t - latency], relative to the reference peak.
double relativeError(const Planar& actual, const Planar& reference, int latency)
{
    double peak = 0.0;
    double error = 0.0;
    for (std::size_t o = 0; o < actual.size(); ++o) {
        for (std::size_t t = 0; t < actual[o].size(); ++t) {
            double expected = t >= static_cast<std::size_t>(latency) ? reference[o][t - latency] : 0.0;
            peak = std::max(peak, std::abs(expected));
            error = std::max(error, std::abs(actual[o][t] - expected));
        }
    }
    return peak > 0.0 ? error / peak : error;
}

ConvolverConfig inlineConfig(int head = 128, int growth = 8, int maxBlock = 8192)
{
    ConvolverConfig config;
    config.headBlockSize = head;
    config.growthFactor = growth;
    config.maxBlockSize = maxBlock;
    config.threaded = false;
    return config;
}

} // namespace

TEST_CASE("matches direct convolution for any callback size")
{
    Planar x{noise(20000, 1)};
    IrMatrix h{{noise(5000, 2, 1500.0f)}};
    Planar reference = directConvolve(x, h, 1);

    for (int callback : {1, 17, 128, 480, 1000, 4096}) {
        CAPTURE(callback);
        Convolver conv(1, 1, h, inlineConfig());
        CHECK(conv.numStages() == 2);
        Planar y = run(conv, x, {callback});
        CHECK(relativeError(y, reference, conv.latencySamples()) < 1e-5);
    }
}

TEST_CASE("handles irregular callback sizes")
{
    Planar x{noise(20000, 3)};
    IrMatrix h{{noise(5000, 4, 1500.0f)}};
    Convolver conv(1, 1, h, inlineConfig());
    Planar y = run(conv, x, {7, 300, 1, 129, 64, 1024});
    CHECK(relativeError(y, directConvolve(x, h, 1), conv.latencySamples()) < 1e-5);
}

TEST_CASE("multi-stage partitioning is exact")
{
    // Stages: 32 @ 0, 128 @ 256, 512 @ 1024, 2048 @ 4096.
    Planar x{noise(30000, 5)};
    IrMatrix h{{noise(12000, 6, 4000.0f)}};
    Convolver conv(1, 1, h, inlineConfig(32, 4, 2048));
    CHECK(conv.numStages() == 4);
    Planar y = run(conv, x, {100});
    CHECK(relativeError(y, directConvolve(x, h, 1), conv.latencySamples()) < 1e-5);
}

TEST_CASE("sums an input/output matrix with missing paths and mixed lengths")
{
    Planar x{noise(15000, 7), noise(15000, 8), noise(15000, 9)};
    IrMatrix h{
        {noise(6000, 10, 2000.0f), noise(300, 11)},
        {Signal{}, noise(9000, 12, 3000.0f)},
        {noise(2500, 13, 800.0f), Signal{}},
    };
    Convolver conv(3, 2, h, inlineConfig(64));
    Planar y = run(conv, x, {256});
    CHECK(relativeError(y, directConvolve(x, h, 2), conv.latencySamples()) < 1e-5);
}

TEST_CASE("latency is exactly the head block size")
{
    for (int head : {16, 64, 128, 256}) {
        CAPTURE(head);
        Signal ir(4000, 0.0f);
        ir[0] = 1.0f;
        ir[3000] = 0.5f; // lands in a tail stage
        Convolver conv(1, 1, IrMatrix{{ir}}, inlineConfig(head));
        CHECK(conv.latencySamples() == head);

        Signal impulse(8000, 0.0f);
        impulse[10] = 1.0f;
        Planar y = run(conv, Planar{impulse}, {480});
        for (std::size_t t = 0; t < y[0].size(); ++t) {
            float expected = t == 10u + head ? 1.0f : (t == 3010u + head ? 0.5f : 0.0f);
            REQUIRE(std::abs(y[0][t] - expected) < 1e-5f);
        }
    }
}

TEST_CASE("processes in place")
{
    Planar x{noise(10000, 14)};
    IrMatrix h{{noise(4000, 15, 1000.0f)}};
    Planar reference = directConvolve(x, h, 1);

    Convolver conv(1, 1, h, inlineConfig());
    Signal buffer = x[0];
    for (std::size_t position = 0; position < buffer.size(); position += 333) {
        int n = static_cast<int>(std::min<std::size_t>(333, buffer.size() - position));
        float* channel = buffer.data() + position;
        conv.process(&channel, &channel, n);
    }
    CHECK(relativeError(Planar{buffer}, reference, conv.latencySamples()) < 1e-5);
}

TEST_CASE("threaded tail stages produce the same result when given time")
{
    Planar x{noise(40000, 16), noise(40000, 17)};
    IrMatrix h{
        {noise(20000, 18, 6000.0f), noise(20000, 19, 6000.0f)},
        {noise(20000, 20, 6000.0f), noise(20000, 21, 6000.0f)},
    };
    ConvolverConfig config = inlineConfig(64, 4, 4096);
    config.threaded = true;
    Convolver conv(2, 2, h, config);
    Planar y = run(conv, x, {256}, true);
    CHECK(conv.lateBlockCount() == 0);
    CHECK(relativeError(y, directConvolve(x, h, 2), conv.latencySamples()) < 1e-5);
}

TEST_CASE("rejects invalid configuration")
{
    IrMatrix h{{Signal(100, 0.1f)}};
    CHECK_THROWS_AS(Convolver(1, 1, h, inlineConfig(100)), std::invalid_argument);
    CHECK_THROWS_AS(Convolver(2, 1, h, inlineConfig()), std::invalid_argument);
    CHECK_THROWS_AS(Convolver(1, 2, h, inlineConfig()), std::invalid_argument);
}

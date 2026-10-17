#pragma once

#include <vector>

namespace brir {

// Linked-channel lookahead peak limiter. The gain starts falling a lookahead
// period before a peak arrives, so peaks are caught without hard clipping; a
// final clamp guarantees the ceiling.
class Limiter {
public:
    Limiter(int channels, double sampleRate, float ceilingDb = -1.0f, float lookaheadMs = 1.5f,
            float releaseMs = 100.0f);

    int latencySamples() const noexcept { return lookahead_; }
    float currentGainDb() const noexcept;

    void process(float* const* io, int frames) noexcept;

private:
    int channels_;
    int lookahead_;
    float ceiling_;
    float attackCoef_;
    float releaseCoef_;
    float gain_ = 1.0f;
    std::vector<std::vector<float>> delay_; // per channel, lookahead_ samples
    std::vector<float> required_;           // gain each delayed sample needs
    int position_ = 0;
};

} // namespace brir

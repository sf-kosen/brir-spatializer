#pragma once

#include "brir/convolver.h"
#include "brir/limiter.h"
#include "brir/upmixer.h"

#include <atomic>
#include <memory>
#include <vector>

namespace brir {

// Real-time binaural renderer: stereo in -> upmix to speaker feeds (L C R Ls Rs)
// -> BRIR convolution (feeds x ears) -> output gain -> limiter -> stereo out.
//
// A new convolver can be installed at any time from the message thread; the
// audio thread crossfades to it. Bypass returns the unprocessed stereo, delayed
// to stay time-aligned.
class Renderer {
public:
    Renderer(double sampleRate, int maxBlockFrames);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    static constexpr int kFeeds = Upmixer::kFeeds;

    // Message thread. The convolver must have kFeeds inputs and 2 outputs.
    // Returns false if a previous swap is still in progress.
    bool setConvolver(std::unique_ptr<Convolver> convolver);
    // Message thread. Frees convolvers the audio thread has finished with.
    void releaseRetired();

    void setBypass(bool bypass) noexcept { bypass_.store(bypass, std::memory_order_relaxed); }
    Upmixer& upmixer() noexcept { return upmixer_; }
    void setOutputGainDb(float db) noexcept;

    // Audio thread. Planar stereo; frames <= maxBlockFrames. Output may alias input.
    void process(const float* const* input, float* const* output, int frames) noexcept;

    // Delay from input to output (upmix frame + convolver head block + limiter lookahead).
    int latencySamples() const noexcept;
    float limiterGainDb() const noexcept { return limiterGainDb_.load(std::memory_order_relaxed); }
    bool hasConvolver() const noexcept { return hasConvolver_.load(std::memory_order_relaxed); }
    // Convolver tail blocks that missed their deadline (audible as dropouts).
    std::uint64_t lateBlocks() const noexcept { return lateBlocks_.load(std::memory_order_relaxed); }

private:
    void delayDry(const float* const* input, int frames) noexcept;

    double sampleRate_;
    int maxBlock_;
    int convolverLatency_ = 0;
    int dryDelaySamples_ = 0; // upmix latency, plus the convolver's once installed

    std::unique_ptr<Convolver> current_;
    std::unique_ptr<Convolver> fadingOut_;
    std::atomic<Convolver*> pending_{nullptr};
    std::atomic<Convolver*> retired_{nullptr};
    int swapFadeTotal_;
    int swapFadeRemaining_ = 0;

    std::atomic<bool> bypass_{false};
    std::atomic<float> outputGain_{1.0f};
    float wetMix_ = 1.0f; // smoothed: 1 = processed, 0 = bypassed
    float wetStep_;
    float gainSmoothed_ = 1.0f;
    float gainCoef_;

    // Dry path is delayed by the upmix + convolver latency so bypass stays time-aligned.
    std::vector<std::vector<float>> dryDelay_;
    int dryDelayPos_ = 0;

    Upmixer upmixer_;
    Limiter limiter_;
    std::vector<std::vector<float>> dry_, wet_, fade_, feeds_;
    std::vector<const float*> feedPtrs_;
    std::atomic<float> limiterGainDb_{0.0f};
    std::atomic<bool> hasConvolver_{false};
    std::atomic<std::uint64_t> lateBlocks_{0};
    std::uint64_t lateBase_ = 0; // late count of convolvers already retired
};

} // namespace brir

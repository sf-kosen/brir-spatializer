#pragma once

#include "brir/convolver.h"
#include "brir/limiter.h"

#include <atomic>
#include <memory>
#include <vector>

namespace brir {

// Real-time binaural renderer: stereo in -> speaker feeds -> BRIR convolution
// -> output gain -> limiter -> stereo out.
//
// Speaker feeds are currently the stereo input itself (2 feeds); the upmixer
// slots in here later. A new convolver can be installed at any time from the
// message thread; the audio thread crossfades to it.
class Renderer {
public:
    Renderer(double sampleRate, int maxBlockFrames);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    static constexpr int kFeeds = 2;

    // Message thread. The convolver must have kFeeds inputs and 2 outputs.
    // Returns false if a previous swap is still in progress.
    bool setConvolver(std::unique_ptr<Convolver> convolver);
    // Message thread. Frees convolvers the audio thread has finished with.
    void releaseRetired();

    void setBypass(bool bypass) noexcept { bypass_.store(bypass, std::memory_order_relaxed); }
    void setOutputGainDb(float db) noexcept;

    // Audio thread. Planar stereo; frames <= maxBlockFrames. Output may alias input.
    void process(const float* const* input, float* const* output, int frames) noexcept;

    // Delay from input to output (convolver head block + limiter lookahead).
    int latencySamples() const noexcept;
    float limiterGainDb() const noexcept { return limiterGainDb_.load(std::memory_order_relaxed); }
    bool hasConvolver() const noexcept { return hasConvolver_.load(std::memory_order_relaxed); }

private:
    void delayDry(const float* const* input, int frames) noexcept;

    double sampleRate_;
    int maxBlock_;
    int convolverLatency_ = 0;

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

    // Dry path is delayed by the convolver latency so bypass stays time-aligned.
    std::vector<std::vector<float>> dryDelay_;
    int dryDelayPos_ = 0;

    Limiter limiter_;
    std::vector<std::vector<float>> dry_, wet_, fade_;
    std::atomic<float> limiterGainDb_{0.0f};
    std::atomic<bool> hasConvolver_{false};
};

} // namespace brir

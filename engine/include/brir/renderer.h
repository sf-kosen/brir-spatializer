#pragma once

#include "brir/convolver.h"
#include "brir/headphone_eq.h"
#include "brir/leveler.h"
#include "brir/limiter.h"
#include "brir/upmixer.h"

#include <atomic>
#include <memory>
#include <vector>

namespace brir {

// Real-time binaural renderer: stereo in -> upmix to speaker feeds (L C R Ls Rs)
// -> bass management (optional: lows of all feeds -> LFE) -> BRIR convolution
// (feeds x ears) -> output gain -> headphone EQ -> leveler (slow automatic
// headroom) -> limiter -> stereo out.
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

    // Convolver inputs: the upmix feeds plus the subwoofer feed.
    static constexpr int kLfe = Upmixer::kFeeds;
    static constexpr int kFeeds = Upmixer::kFeeds + 1;
    static constexpr const char* kFeedNames[kFeeds] = {"L", "C", "R", "Ls", "Rs", "LFE"};

    // Message thread. The convolver must have kFeeds inputs and 2 outputs.
    // Returns false if a previous swap is still in progress.
    bool setConvolver(std::unique_ptr<Convolver> convolver);
    // Message thread. Frees convolvers the audio thread has finished with.
    void releaseRetired();

    void setBypass(bool bypass) noexcept { bypass_.store(bypass, std::memory_order_relaxed); }
    Upmixer& upmixer() noexcept { return upmixer_; }
    // Applies to the processed and the bypassed sound alike (it corrects the
    // playback device, not the content).
    HeadphoneEq& headphoneEq() noexcept { return headphoneEq_; }
    void setOutputGainDb(float db) noexcept;
    // Bass management as in a cinema: below the crossover (80 Hz, Linkwitz-Riley
    // 24 dB/oct) every feed's sound moves to the LFE feed (the subwoofers), at
    // subGainDb relative to the mains. Off by default: all feeds run full range
    // and LFE is silent.
    void setBassManagement(bool enabled, float subGainDb) noexcept;
    // Automatic headroom for loud masters (see Leveler). On by default.
    void setLevelerEnabled(bool enabled) noexcept { levelerEnabled_.store(enabled, std::memory_order_relaxed); }

    // Audio thread. Planar stereo; frames <= maxBlockFrames. Output may alias input.
    void process(const float* const* input, float* const* output, int frames) noexcept;

    // Delay from input to output (upmix frame + convolver head block + limiter lookahead).
    int latencySamples() const noexcept;
    float limiterGainDb() const noexcept { return limiterGainDb_.load(std::memory_order_relaxed); }
    float levelerGainDb() const noexcept { return levelerGainDb_.load(std::memory_order_relaxed); }
    bool hasConvolver() const noexcept { return hasConvolver_.load(std::memory_order_relaxed); }
    // Convolver tail blocks that missed their deadline (audible as dropouts).
    std::uint64_t lateBlocks() const noexcept { return lateBlocks_.load(std::memory_order_relaxed); }

private:
    void delayDry(const float* const* input, int frames) noexcept;
    void manageBass(int frames) noexcept;

    struct Biquad {
        float b0, b1, b2, a1, a2;
    };
    static Biquad butterworth(double frequency, double sampleRate, bool highpass);

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
    HeadphoneEq headphoneEq_;
    Leveler leveler_;
    std::atomic<bool> levelerEnabled_{true};
    Limiter limiter_;
    // Crossover: two cascaded Butterworth sections each for low- and high-pass,
    // state per upmix feed (transposed direct form II).
    Biquad lowpass_, highpass_;
    float lowState_[Upmixer::kFeeds][2][2] = {};
    float highState_[Upmixer::kFeeds][2][2] = {};
    std::atomic<bool> bassManaged_{false};
    std::atomic<float> subGain_{1.0f};
    float bassMix_ = 0.0f; // smoothed: 1 = bass managed
    float subGainSmoothed_ = 1.0f;

    std::vector<std::vector<float>> dry_, wet_, fade_, feeds_;
    std::vector<const float*> feedPtrs_;
    std::atomic<float> limiterGainDb_{0.0f};
    std::atomic<float> levelerGainDb_{0.0f};
    std::atomic<bool> hasConvolver_{false};
    std::atomic<std::uint64_t> lateBlocks_{0};
    std::uint64_t lateBase_ = 0; // late count of convolvers already retired
};

} // namespace brir

#pragma once

#include "brir/fft.h"

#include <atomic>
#include <memory>
#include <vector>

namespace brir {

// Stereo -> L, C, R, Ls, Rs upmix in the STFT domain.
//
// Per frequency bin, the smoothed channel covariance splits each channel into a
// primary part (correlated between L and R) and an ambient part (uncorrelated,
// equal power on both sides):
//   - primary sound is steered to L/C/R by its level ratio (constant power),
//     moving to C only in proportion to coherence^4, so dialogue goes to C while
//     partially correlated music keeps its stereo image;
//     primary sound that is out of phase between L and R goes to the surrounds,
//     as in matrix-encoded (Lt/Rt) film soundtracks;
//   - ambience goes partly to the surrounds, keeping each side's own phase so
//     it stays decorrelated;
//   - the surrounds' share of primary sound is a single signal; fed identically
//     to two mirror-image arrays it would reach both ears identically and sit
//     inside the head, so above ~150 Hz it is decorrelated with complementary
//     comb filters (Lauridsen): Ls gets (M + M delayed) / sqrt2, Rs (M - M delayed) / sqrt2.
// Energy is preserved per bin. Latency is exactly one STFT frame.
class Upmixer {
public:
    enum Feed { L, C, R, Ls, Rs, kFeeds };

    explicit Upmixer(double sampleRate);
    ~Upmixer();

    Upmixer(const Upmixer&) = delete;
    Upmixer& operator=(const Upmixer&) = delete;

    int latencySamples() const noexcept { return frameSize_; }
    int frameSize() const noexcept { return frameSize_; }

    // Thread-safe controls.
    // 0: phantom center stays between L and R; 1: it moves fully to C.
    void setCenterAmount(float amount) noexcept { centerAmount_.store(amount, std::memory_order_relaxed); }
    // Share of ambient energy sent to the surrounds.
    void setSurroundAmount(float amount) noexcept { surroundAmount_.store(amount, std::memory_order_relaxed); }
    // When disabled, L and R pass straight through (same latency) and C/Ls/Rs are silent.
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }

    // Audio thread. feeds must not alias stereo.
    void process(const float* const* stereo, float* const* feeds, int frames) noexcept;

private:
    // Internal STFT outputs: the five feeds plus the surround primary parts that
    // get decorrelated in the time domain.
    enum Internal { DecorrelateL = kFeeds, DecorrelateR, kInternal };

    void processFrame() noexcept;
    void decorrelate() noexcept;

    int frameSize_;
    int hop_;
    int bins_;
    float smoothing_;
    float coherenceBias_;
    RealFft fft_;
    std::vector<float> window_; // sqrt-Hann, used for analysis and synthesis

    AlignedBuffer inL_, inR_;        // last frameSize_ input samples
    AlignedBuffer specL_, specR_;    // ordered spectra
    AlignedBuffer feedSpec_[kInternal]; // ordered output spectra
    AlignedBuffer timeOut_;
    std::vector<float> overlap_[kInternal]; // overlap-add accumulators
    std::vector<float> ready_[kInternal];   // completed hop being emitted
    std::vector<float> decorrelateWeight_;  // per bin: share of surround primary that is decorrelated
    std::vector<float> decorrelateDelay_[2];
    int decorrelateDelayPos_ = 0;
    std::vector<float> powL_, powR_, crossRe_, crossIm_;
    int pos_ = 0; // position within the current hop

    std::atomic<float> centerAmount_{1.0f};
    std::atomic<float> surroundAmount_{0.6f};
    std::atomic<bool> enabled_{true};
};

} // namespace brir

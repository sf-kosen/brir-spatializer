#pragma once

#include "brir/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace brir {

struct ClockBridgeConfig {
    int channels = 2;
    double inputRate = 48000.0;
    double outputRate = 48000.0;
    // The target adapts to the producer's delivery pattern: it is the worst
    // observed gap between the producer timeline and the data actually buffered,
    // plus this margin. Each underrun widens the margin by latencyStepMs.
    double safetyMarginMs = 3.0;
    double latencyStepMs = 2.0;
    // Before (re)starting playback, observe the producer for at least this long
    // and this many deliveries, so the delivery gap estimate covers its worst case.
    double minPrimingMs = 100.0;
    int minPrimingDeliveries = 10;
    // Rare scheduling delays are not seen while priming. Start this much above
    // the observed target; it decays as slowly as any target reduction, giving
    // time for the true worst case to be observed.
    double startupCushionMs = 2.0;
    double maxTargetLatencyMs = 80.0;
    // How fast the tracked worst-case gap is allowed to shrink again.
    double gapReleaseMsPerSecond = 0.02;
    // Bandwidth of the drift control loop. Low values keep the pitch steady.
    double loopBandwidthHz = 0.02;
    double maxDriftPpm = 1000.0;
    double fadeInMs = 5.0;
    // read() is processed in chunks of at most this many frames.
    int maxChunkFrames = 2048;
};

struct ClockBridgeStats {
    double fillMs = 0.0;
    double targetMs = 0.0;
    double driftPpm = 0.0;
    double bufferedMs = 0.0; // input actually buffered after the last read
    double gapMs = 0.0;      // tracked worst gap between timeline and buffer
    double marginMs = 0.0;
    std::uint64_t underruns = 0;
    std::uint64_t overflows = 0;
    std::uint64_t resyncs = 0;
    bool running = false;
};

// Moves audio from a producer clock domain (e.g. a capture device) to a consumer
// clock domain (the output device). A variable-ratio windowed-sinc resampler
// absorbs both the nominal rate difference and the clock drift; a PI controller
// steers the drift estimate so the buffered amount stays at the target.
//
// The controller does not look at the raw FIFO level: with bursty delivery that
// level is a sawtooth whose sampling by the consumer aliases into slow beats.
// Instead the producer stamps each block with the time of its first frame, and
// the fill is measured against the producer position extrapolated to the read
// time. Timestamps are seconds on std::chrono::steady_clock (QPC on Windows).
class ClockBridge {
public:
    explicit ClockBridge(const ClockBridgeConfig& config);

    static double now() noexcept;

    // Producer thread. firstFrameTime: when the block's first frame was captured.
    void write(const float* interleaved, int frames, double firstFrameTime) noexcept;
    void writeSilence(int frames, double firstFrameTime) noexcept;
    // Assumes the block's last frame was captured just now.
    void write(const float* interleaved, int frames) noexcept;

    // Consumer thread. Planar output; outputs silence while (re)buffering.
    void read(float* const* output, int frames, double time) noexcept;
    void read(float* const* output, int frames) noexcept { read(output, frames, now()); }

    // Any thread.
    ClockBridgeStats stats() const noexcept;

    // Delay added by the resampler kernel, in input frames.
    int resamplerLatencyFrames() const noexcept { return kHalfTaps; }

    // Pins the drift to a fixed value and disables the controller. Call before
    // streaming starts; intended for tests and diagnostics.
    void setFixedDriftPpm(double ppm) noexcept;

private:
    static constexpr int kTaps = 64;
    static constexpr int kHalfTaps = kTaps / 2;
    static constexpr int kPhases = 256;

    void buildKernel();
    void publishMark(int written, double firstFrameTime) noexcept;
    struct FillSample {
        double timeline = 0.0; // fill measured against the producer timeline
        double buffered = 0.0; // input actually buffered ahead of the read point
        double sinceMark = 0.0;
        std::uint32_t markId = 0;
    };

    FillSample sampleFill(double time) const noexcept;
    bool trackGap(const FillSample& sample, double consumed, double dt) noexcept;
    double desiredTarget() const noexcept;
    void updateController(int frames, double time) noexcept;
    void publishDiagnostics() noexcept;
    bool renderChunk(float* const* output, int offset, int n) noexcept;
    double bufferedFrames() const noexcept;
    double timelineFill(double time, double* sinceMark = nullptr, std::uint32_t* markId = nullptr) const noexcept;
    void resetResampler() noexcept;
    void discardInput(int frames) noexcept;
    int pullInput(float* interleaved, int frames) noexcept;

    ClockBridgeConfig config_;
    SpscRing ring_;
    double baseRatio_; // input frames per output frame at zero drift

    // Producer position (frames written before the block) at the time of the
    // block's first frame, published with a seqlock.
    std::uint64_t produced_ = 0; // producer only
    std::atomic<std::uint32_t> markSeq_{0};
    std::atomic<std::uint64_t> markPosition_{0};
    std::atomic<double> markTime_{0.0};
    std::uint64_t consumed_ = 0; // frames pulled from the ring, consumer only

    std::vector<float> kernel_; // (kPhases + 1) rows of kTaps
    std::vector<float> coefficients_;
    std::vector<std::vector<float>> history_; // per channel, consumer only
    std::vector<float> scratch_;
    int historyCount_ = 0;
    double position_ = 0.0; // read point inside history_, in input frames

    // Controller state, consumer only.
    double kp_ = 0.0;
    double ki_ = 0.0;
    double integral_ = 0.0;
    double drift_ = 0.0;
    double filteredFill_ = 0.0;
    double targetFrames_ = 0.0;
    double marginFrames_ = 0.0;
    double peakGap_ = 0.0; // worst recent (timeline - buffered after this read), frames
    double peakGapAtMark_ = 0.0;
    std::uint32_t lastMarkId_ = 0;
    bool fixedDrift_ = false;
    bool running_ = false;
    double primingSeconds_ = 0.0;
    bool pendingMarginCheck_ = false; // an underrun awaits classification
    double underrunMarkTime_ = 0.0;
    std::uint32_t underrunMarkId_ = 0;
    int primingDeliveries_ = 0;
    int fadeRemaining_ = 0;
    int fadeTotal_ = 0;

    std::atomic<double> statFillMs_{0.0};
    std::atomic<double> statBufferedMs_{0.0};
    std::atomic<double> statGapMs_{0.0};
    std::atomic<double> statMarginMs_{0.0};
    std::atomic<double> statTargetMs_{0.0};
    std::atomic<double> statDriftPpm_{0.0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> overflows_{0};
    std::atomic<std::uint64_t> resyncs_{0};
    std::atomic<bool> statRunning_{false};
};

} // namespace brir

#include "brir/clock_bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace brir {

namespace {

constexpr double kKaiserBeta = 9.0;
constexpr double kDamping = 1.0;
// The fill measurement is low-passed this many times above the loop bandwidth,
// which smooths out bursty capture delivery without destabilising the loop.
constexpr double kMeasurementBandwidthRatio = 5.0;
// Excess buffering beyond which we drop input instead of slowly steering it away.
constexpr double kResyncThresholdMs = 40.0;
// With no producer data for this long the timeline extrapolation is meaningless
// (input stalled); the controller holds its state until data flows again.
constexpr double kStallSeconds = 0.040;
// Longest pause between deliveries that still counts as normal delivery when
// judging an underrun.
constexpr double kNormalDeliveryPauseSeconds = 0.080;
// Time constants for moving the target, so gap estimate updates arrive as ramps
// the loop can follow rather than steps that kick the drift estimate. Raising
// the target protects against underruns and is rare once settled, so it is
// faster; lowering it only trims latency and may be slow.
constexpr double kTargetRiseSeconds = 10.0;
constexpr double kTargetFallSeconds = 30.0;

double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

} // namespace

ClockBridge::ClockBridge(const ClockBridgeConfig& config)
    : config_(config),
      ring_(config.channels,
            static_cast<int>(std::ceil((config.maxTargetLatencyMs + kResyncThresholdMs + 100.0) * config.inputRate / 1000.0))),
      baseRatio_(config.inputRate / config.outputRate)
{
    if (config.channels <= 0 || config.inputRate <= 0.0 || config.outputRate <= 0.0 || config.maxChunkFrames <= 0)
        throw std::invalid_argument("Invalid ClockBridge configuration.");

    buildKernel();
    coefficients_.resize(kTaps);

    double maxRatio = baseRatio_ * (1.0 + config.maxDriftPpm * 1e-6);
    int historyCapacity = kTaps + static_cast<int>(std::ceil(config.maxChunkFrames * maxRatio)) + 4;
    history_.assign(static_cast<std::size_t>(config.channels), std::vector<float>(static_cast<std::size_t>(historyCapacity)));
    scratch_.resize(static_cast<std::size_t>(historyCapacity) * config.channels);
    resetResampler();

    double omega = 2.0 * std::numbers::pi * config.loopBandwidthHz;
    kp_ = 2.0 * kDamping * omega / config.inputRate;
    ki_ = omega * omega / config.inputRate;
    marginFrames_ = config.safetyMarginMs * config.inputRate / 1000.0;
    targetFrames_ = marginFrames_;
    fadeTotal_ = std::max(1, static_cast<int>(config.fadeInMs * config.outputRate / 1000.0));
    statTargetMs_.store(config.safetyMarginMs);
}

// Windowed-sinc kernel sampled at kPhases fractional offsets. Row p holds the
// taps for a read point p / kPhases past an integer input index. The cutoff sits
// just below the lower of the two Nyquist frequencies.
void ClockBridge::buildKernel()
{
    double cutoff = 0.5 * std::min(1.0, 1.0 / baseRatio_) * 0.92; // cycles per input sample
    kernel_.assign(static_cast<std::size_t>(kPhases + 1) * kTaps, 0.0f);
    double windowNorm = besselI0(kKaiserBeta);

    for (int p = 0; p <= kPhases; ++p) {
        double frac = static_cast<double>(p) / kPhases;
        std::vector<double> row(kTaps);
        double sum = 0.0;
        for (int j = 0; j < kTaps; ++j) {
            double d = (j - kHalfTaps + 1) - frac;
            double ratio = d / kHalfTaps;
            double window = std::abs(ratio) <= 1.0 ? besselI0(kKaiserBeta * std::sqrt(1.0 - ratio * ratio)) / windowNorm : 0.0;
            double x = 2.0 * cutoff * d;
            double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            row[static_cast<std::size_t>(j)] = 2.0 * cutoff * sinc * window;
            sum += row[static_cast<std::size_t>(j)];
        }
        // Unity DC gain for every phase, so gain doesn't ripple with the fraction.
        for (int j = 0; j < kTaps; ++j)
            kernel_[static_cast<std::size_t>(p) * kTaps + j] = static_cast<float>(row[static_cast<std::size_t>(j)] / sum);
    }
}

double ClockBridge::now() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ClockBridge::publishMark(int written, double firstFrameTime) noexcept
{
    std::uint32_t seq = markSeq_.load(std::memory_order_relaxed);
    markSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    markPosition_.store(produced_, std::memory_order_relaxed);
    markTime_.store(firstFrameTime, std::memory_order_relaxed);
    markSeq_.store(seq + 2, std::memory_order_release);
    produced_ += static_cast<std::uint64_t>(written);
}

void ClockBridge::write(const float* interleaved, int frames, double firstFrameTime) noexcept
{
    int written = ring_.write(interleaved, frames);
    if (written < frames) overflows_.fetch_add(1, std::memory_order_relaxed);
    publishMark(written, firstFrameTime);
}

void ClockBridge::writeSilence(int frames, double firstFrameTime) noexcept
{
    int written = ring_.writeSilence(frames);
    if (written < frames) overflows_.fetch_add(1, std::memory_order_relaxed);
    publishMark(written, firstFrameTime);
}

void ClockBridge::write(const float* interleaved, int frames) noexcept
{
    write(interleaved, frames, now() - frames / config_.inputRate);
}

int ClockBridge::pullInput(float* interleaved, int frames) noexcept
{
    int n = ring_.read(interleaved, frames);
    consumed_ += static_cast<std::uint64_t>(n);
    return n;
}

// Fill measured against the producer timeline: the producer position
// extrapolated to `time`, minus the consumer's read point.
double ClockBridge::timelineFill(double time, double* sinceMark, std::uint32_t* markId) const noexcept
{
    std::uint32_t before, after;
    std::uint64_t position;
    double markTime;
    do {
        before = markSeq_.load(std::memory_order_acquire);
        position = markPosition_.load(std::memory_order_relaxed);
        markTime = markTime_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        after = markSeq_.load(std::memory_order_relaxed);
    } while ((before & 1u) != 0 || before != after);

    if (sinceMark) *sinceMark = before == 0 ? 0.0 : time - markTime;
    if (markId) *markId = before;
    if (before == 0) return bufferedFrames(); // nothing written yet
    double producerPosition = static_cast<double>(position) + (time - markTime) * config_.inputRate;
    double readPoint = static_cast<double>(consumed_) - (historyCount_ - (position_ + kHalfTaps));
    return producerPosition - readPoint;
}

void ClockBridge::setFixedDriftPpm(double ppm) noexcept
{
    fixedDrift_ = true;
    drift_ = ppm * 1e-6;
}

void ClockBridge::resetResampler() noexcept
{
    for (auto& channel : history_) std::fill(channel.begin(), channel.end(), 0.0f);
    historyCount_ = kTaps;
    position_ = kHalfTaps - 1;
}

// Input frames buffered ahead of the current read point.
double ClockBridge::bufferedFrames() const noexcept
{
    return ring_.available() + (historyCount_ - (position_ + kHalfTaps));
}

void ClockBridge::discardInput(int frames) noexcept
{
    int chunk = static_cast<int>(scratch_.size()) / config_.channels;
    while (frames > 0) {
        int n = pullInput(scratch_.data(), std::min(frames, chunk));
        if (n <= 0) break;
        frames -= n;
    }
}

// The timeline runs ahead of the data actually delivered by up to one delivery
// burst plus its latency. Tracks the worst recent gap, as seen just after a read
// consumes its input; aiming the timeline fill at gap + margin makes the real
// buffer bottom out around the safety margin. Returns false while stalled.
bool ClockBridge::trackGap(const FillSample& sample, double consumed, double dt) noexcept
{
    if (sample.sinceMark > kStallSeconds) {
        // Input stalled: forget the gap growth since the last delivery.
        peakGap_ = peakGapAtMark_;
        return false;
    }
    if (sample.markId != lastMarkId_) {
        lastMarkId_ = sample.markId;
        ++primingDeliveries_;
        peakGapAtMark_ = peakGap_;
    }
    double gap = sample.timeline - (sample.buffered - consumed);
    double release = config_.gapReleaseMsPerSecond * config_.inputRate / 1000.0 * dt;
    peakGap_ = std::max(gap, peakGap_ - release);
    return true;
}

double ClockBridge::desiredTarget() const noexcept
{
    double maxTarget = config_.maxTargetLatencyMs * config_.inputRate / 1000.0;
    return std::min(maxTarget, marginFrames_ + peakGap_);
}

ClockBridge::FillSample ClockBridge::sampleFill(double time) const noexcept
{
    FillSample sample;
    sample.timeline = timelineFill(time, &sample.sinceMark, &sample.markId);
    sample.buffered = bufferedFrames();
    return sample;
}

void ClockBridge::updateController(int frames, double time) noexcept
{
    FillSample sample = sampleFill(time);
    double dt = frames / config_.outputRate;
    double consumed = frames * baseRatio_ * (1.0 + drift_);
    if (!trackGap(sample, consumed, dt)) return;

    double alpha = 1.0 - std::exp(-2.0 * std::numbers::pi * kMeasurementBandwidthRatio * config_.loopBandwidthHz * dt);
    filteredFill_ += alpha * (sample.timeline - filteredFill_);

    double desired = desiredTarget();
    double smoothing = desired > targetFrames_ ? kTargetRiseSeconds : kTargetFallSeconds;
    targetFrames_ += (1.0 - std::exp(-dt / smoothing)) * (desired - targetFrames_);
    double fill = sample.timeline;
    double actual = sample.buffered;

    if (!fixedDrift_) {
        double error = filteredFill_ - targetFrames_;
        double limit = config_.maxDriftPpm * 1e-6;
        double candidate = kp_ * error + ki_ * (integral_ + error * dt);
        // Conditional integration: stop integrating while saturated.
        if (std::abs(candidate) < limit) integral_ += error * dt;
        drift_ = std::clamp(kp_ * error + ki_ * integral_, -limit, limit);
    }

    double resyncFrames = kResyncThresholdMs * config_.inputRate / 1000.0;
    if (!fixedDrift_ && fill > targetFrames_ + resyncFrames) {
        discardInput(static_cast<int>(std::min(fill - targetFrames_, actual - consumed)));
        filteredFill_ = timelineFill(time);
        resyncs_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ClockBridge::renderChunk(float* const* output, int offset, int n) noexcept
{
    const double ratio = baseRatio_ * (1.0 + drift_);
    const int channels = config_.channels;

    // Pull just enough input to cover the last output frame's kernel.
    int lastIndex = static_cast<int>(std::floor(position_ + (n - 1) * ratio)) + kHalfTaps;
    int needed = lastIndex + 1 - historyCount_;
    if (needed > 0) {
        if (ring_.available() < needed) return false;
        pullInput(scratch_.data(), needed);
        for (int f = 0; f < needed; ++f)
            for (int ch = 0; ch < channels; ++ch)
                history_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(historyCount_ + f)] =
                    scratch_[static_cast<std::size_t>(f) * channels + ch];
        historyCount_ += needed;
    }

    for (int k = 0; k < n; ++k) {
        double x = position_ + k * ratio;
        int index = static_cast<int>(std::floor(x));
        double phase = (x - index) * kPhases;
        int row = std::min(static_cast<int>(phase), kPhases - 1);
        float t = static_cast<float>(phase - row);
        const float* a = kernel_.data() + static_cast<std::size_t>(row) * kTaps;
        const float* b = a + kTaps;
        for (int j = 0; j < kTaps; ++j) coefficients_[static_cast<std::size_t>(j)] = a[j] + (b[j] - a[j]) * t;

        int start = index - kHalfTaps + 1;
        for (int ch = 0; ch < channels; ++ch) {
            const float* src = history_[static_cast<std::size_t>(ch)].data() + start;
            float sum = 0.0f;
            for (int j = 0; j < kTaps; ++j) sum += src[j] * coefficients_[static_cast<std::size_t>(j)];
            output[ch][offset + k] = sum;
        }
    }

    position_ += n * ratio;
    int drop = static_cast<int>(std::floor(position_)) - (kHalfTaps - 1);
    if (drop > 0) {
        for (auto& channel : history_)
            std::copy(channel.begin() + drop, channel.begin() + historyCount_, channel.begin());
        historyCount_ -= drop;
        position_ -= drop;
    }
    return true;
}

void ClockBridge::read(float* const* output, int frames, double time) noexcept
{
    if (frames <= 0) return;

    if (!running_) {
        // While (re)buffering, learn the delivery gap and set the target from it
        // directly. Start once the timeline fill reaches the target, after having
        // watched the producer for minPrimingMs and minPrimingDeliveries.
        FillSample sample = sampleFill(time);
        double dt = frames / config_.outputRate;
        double consumed = frames * baseRatio_;
        if (pendingMarginCheck_ && sample.markId != underrunMarkId_) {
            // Data is flowing again. A short pause since the last delivery means
            // the buffer ran dry during normal delivery: widen the margin. A long
            // one means the producer had stopped (e.g. playback paused); more
            // margin would only add latency for good.
            pendingMarginCheck_ = false;
            double pause = (time - sample.sinceMark) - underrunMarkTime_;
            if (pause <= kNormalDeliveryPauseSeconds) {
                double maxTarget = config_.maxTargetLatencyMs * config_.inputRate / 1000.0;
                marginFrames_ = std::min(maxTarget, marginFrames_ + config_.latencyStepMs * config_.inputRate / 1000.0);
            }
        }
        trackGap(sample, consumed, dt);
        targetFrames_ = desiredTarget() + config_.startupCushionMs * config_.inputRate / 1000.0;
        primingSeconds_ += dt;
        bool ready = primingSeconds_ * 1000.0 >= config_.minPrimingMs &&
                     primingDeliveries_ >= config_.minPrimingDeliveries && sample.timeline >= targetFrames_ &&
                     sample.buffered >= consumed + marginFrames_;
        if (ready) {
            // Everything that arrived while observing is still buffered; start
            // at the target instead of slowly draining the surplus.
            double surplus = std::min(sample.timeline - targetFrames_, sample.buffered - consumed - marginFrames_);
            if (surplus >= 1.0) discardInput(static_cast<int>(surplus));
            running_ = true;
            fadeRemaining_ = fadeTotal_;
            filteredFill_ = timelineFill(time);
        } else {
            for (int ch = 0; ch < config_.channels; ++ch) std::fill_n(output[ch], frames, 0.0f);
            statRunning_.store(false, std::memory_order_relaxed);
            statFillMs_.store(sample.timeline * 1000.0 / config_.inputRate, std::memory_order_relaxed);
            statTargetMs_.store(targetFrames_ * 1000.0 / config_.inputRate, std::memory_order_relaxed);
            publishDiagnostics();
            return;
        }
    }

    updateController(frames, time);

    for (int offset = 0; offset < frames;) {
        int n = std::min(config_.maxChunkFrames, frames - offset);
        if (!renderChunk(output, offset, n)) {
            for (int ch = 0; ch < config_.channels; ++ch) std::fill_n(output[ch] + offset, frames - offset, 0.0f);
            running_ = false;
            primingSeconds_ = 0.0;
            primingDeliveries_ = 0;
            resetResampler();
            underruns_.fetch_add(1, std::memory_order_relaxed);
            // Whether this means "buffer too small" is decided when data arrives
            // again: see the priming branch.
            double sinceMark = 0.0;
            timelineFill(time, &sinceMark, &underrunMarkId_);
            underrunMarkTime_ = time - sinceMark;
            pendingMarginCheck_ = true;
            break;
        }
        offset += n;
    }

    if (fadeRemaining_ > 0) {
        int fadeStart = fadeTotal_ - fadeRemaining_;
        int n = std::min(frames, fadeRemaining_);
        for (int ch = 0; ch < config_.channels; ++ch)
            for (int i = 0; i < n; ++i)
                output[ch][i] *= static_cast<float>(fadeStart + i) / static_cast<float>(fadeTotal_);
        fadeRemaining_ -= n;
    }

    statRunning_.store(running_, std::memory_order_relaxed);
    statFillMs_.store(filteredFill_ * 1000.0 / config_.inputRate, std::memory_order_relaxed);
    statTargetMs_.store(targetFrames_ * 1000.0 / config_.inputRate, std::memory_order_relaxed);
    statDriftPpm_.store(drift_ * 1e6, std::memory_order_relaxed);
    publishDiagnostics();
}

void ClockBridge::publishDiagnostics() noexcept
{
    double toMs = 1000.0 / config_.inputRate;
    statBufferedMs_.store(bufferedFrames() * toMs, std::memory_order_relaxed);
    statGapMs_.store(peakGap_ * toMs, std::memory_order_relaxed);
    statMarginMs_.store(marginFrames_ * toMs, std::memory_order_relaxed);
}

ClockBridgeStats ClockBridge::stats() const noexcept
{
    ClockBridgeStats s;
    s.fillMs = statFillMs_.load(std::memory_order_relaxed);
    s.targetMs = statTargetMs_.load(std::memory_order_relaxed);
    s.driftPpm = statDriftPpm_.load(std::memory_order_relaxed);
    s.bufferedMs = statBufferedMs_.load(std::memory_order_relaxed);
    s.gapMs = statGapMs_.load(std::memory_order_relaxed);
    s.marginMs = statMarginMs_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overflows = overflows_.load(std::memory_order_relaxed);
    s.resyncs = resyncs_.load(std::memory_order_relaxed);
    s.running = statRunning_.load(std::memory_order_relaxed);
    return s;
}

} // namespace brir

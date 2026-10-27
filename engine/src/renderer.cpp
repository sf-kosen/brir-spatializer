#include "brir/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace brir {

namespace {
constexpr double kSwapFadeMs = 50.0;
constexpr double kBypassFadeMs = 20.0;
constexpr double kGainSmoothingMs = 20.0;
constexpr int kMaxDryDelay = 8192; // longest supported convolver head block
constexpr double kCrossoverHz = 80.0;
} // namespace

// RBJ cookbook low/high-pass with Q = 1/sqrt2 (Butterworth); two in series make
// a Linkwitz-Riley crossover whose outputs sum to an allpass.
Renderer::Biquad Renderer::butterworth(double frequency, double sampleRate, bool highpass)
{
    double w = 2.0 * std::numbers::pi * frequency / sampleRate;
    double alpha = std::sin(w) / std::sqrt(2.0);
    double c = std::cos(w);
    double a0 = 1.0 + alpha;
    double b1 = highpass ? -(1.0 + c) : 1.0 - c;
    double b0 = highpass ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0), static_cast<float>(b0 / a0),
            static_cast<float>(-2.0 * c / a0), static_cast<float>((1.0 - alpha) / a0)};
}

Renderer::Renderer(double sampleRate, int maxBlockFrames)
    : sampleRate_(sampleRate),
      maxBlock_(maxBlockFrames),
      swapFadeTotal_(std::max(1, static_cast<int>(kSwapFadeMs * sampleRate / 1000.0))),
      wetStep_(static_cast<float>(1000.0 / (kBypassFadeMs * sampleRate))),
      gainCoef_(static_cast<float>(1.0 - std::exp(-1000.0 / (kGainSmoothingMs * sampleRate)))),
      upmixer_(sampleRate),
      headphoneEq_(sampleRate),
      leveler_(2, sampleRate),
      limiter_(2, sampleRate),
      lowpass_(butterworth(kCrossoverHz, sampleRate, false)),
      highpass_(butterworth(kCrossoverHz, sampleRate, true))
{
    for (auto* buffers : {&dry_, &wet_, &fade_})
        buffers->assign(2, std::vector<float>(static_cast<std::size_t>(maxBlockFrames), 0.0f));
    feeds_.assign(kFeeds, std::vector<float>(static_cast<std::size_t>(maxBlockFrames), 0.0f));
    for (auto& feed : feeds_) feedPtrs_.push_back(feed.data());
    dryDelaySamples_ = upmixer_.latencySamples();
    dryDelay_.assign(2, std::vector<float>(kMaxDryDelay, 0.0f));
}

Renderer::~Renderer()
{
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
}

bool Renderer::setConvolver(std::unique_ptr<Convolver> convolver)
{
    if (!convolver || convolver->numInputs() != kFeeds || convolver->numOutputs() != 2)
        throw std::invalid_argument("Renderer needs a convolver with one input per upmix feed and 2 ears.");
    if (convolver->latencySamples() + upmixer_.latencySamples() > kMaxDryDelay)
        throw std::invalid_argument("Convolver head block too large.");
    releaseRetired();
    Convolver* expected = nullptr;
    Convolver* raw = convolver.get();
    if (!pending_.compare_exchange_strong(expected, raw)) return false;
    convolver.release();
    return true;
}

void Renderer::releaseRetired()
{
    delete retired_.exchange(nullptr);
}

void Renderer::setOutputGainDb(float db) noexcept
{
    outputGain_.store(std::pow(10.0f, db / 20.0f), std::memory_order_relaxed);
}

void Renderer::setBassManagement(bool enabled, float subGainDb) noexcept
{
    subGain_.store(std::pow(10.0f, subGainDb / 20.0f), std::memory_order_relaxed);
    bassManaged_.store(enabled, std::memory_order_relaxed);
}

int Renderer::latencySamples() const noexcept
{
    return upmixer_.latencySamples() + convolverLatency_ + limiter_.latencySamples();
}

void Renderer::delayDry(const float* const* input, int frames) noexcept
{
    for (int i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            float& slot = dryDelay_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(dryDelayPos_)];
            dry_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] = slot;
            slot = input[ch][i];
        }
        dryDelayPos_ = dryDelayPos_ + 1 == dryDelaySamples_ ? 0 : dryDelayPos_ + 1;
    }
}

void Renderer::manageBass(int frames) noexcept
{
    float* lfe = feeds_[kLfe].data();
    std::fill_n(lfe, frames, 0.0f);
    const float mixTarget = bassManaged_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    const float gainTarget = subGain_.load(std::memory_order_relaxed);
    // Filters always run so switching doesn't start from cold state.
    auto run = [](const Biquad& q, float (&z)[2], float x) {
        float y = q.b0 * x + z[0];
        z[0] = q.b1 * x - q.a1 * y + z[1];
        z[1] = q.b2 * x - q.a2 * y;
        return y;
    };
    for (int i = 0; i < frames; ++i) {
        bassMix_ = mixTarget > bassMix_ ? std::min(mixTarget, bassMix_ + wetStep_) : std::max(mixTarget, bassMix_ - wetStep_);
        subGainSmoothed_ += (gainTarget - subGainSmoothed_) * gainCoef_;
        float low = 0.0f;
        for (int f = 0; f < Upmixer::kFeeds; ++f) {
            float& x = feeds_[static_cast<std::size_t>(f)][static_cast<std::size_t>(i)];
            float lp = run(lowpass_, lowState_[f][1], run(lowpass_, lowState_[f][0], x));
            float hp = run(highpass_, highState_[f][1], run(highpass_, highState_[f][0], x));
            low += lp;
            x += (hp - x) * bassMix_;
        }
        lfe[i] = low * bassMix_ * subGainSmoothed_;
    }
}

void Renderer::process(const float* const* input, float* const* output, int frames) noexcept
{
    frames = std::min(frames, maxBlock_);

    // Adopt a newly installed convolver once the previous swap has settled.
    if (!fadingOut_ && retired_.load(std::memory_order_acquire) == nullptr) {
        if (Convolver* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            fadingOut_ = std::move(current_);
            current_.reset(next);
            swapFadeRemaining_ = fadingOut_ ? swapFadeTotal_ : 0;
            if (current_->latencySamples() != convolverLatency_) {
                // Only happens on the first install (all convolvers share a head size).
                convolverLatency_ = current_->latencySamples();
                dryDelaySamples_ = upmixer_.latencySamples() + convolverLatency_;
                for (auto& line : dryDelay_) std::fill(line.begin(), line.end(), 0.0f);
                dryDelayPos_ = 0;
            }
            hasConvolver_.store(true, std::memory_order_relaxed);
        }
    }

    delayDry(input, frames);

    float* feeds[Upmixer::kFeeds];
    for (int f = 0; f < Upmixer::kFeeds; ++f) feeds[f] = feeds_[static_cast<std::size_t>(f)].data();
    upmixer_.process(input, feeds, frames);
    manageBass(frames);

    float* wet[2] = {wet_[0].data(), wet_[1].data()};
    if (current_) {
        current_->process(feedPtrs_.data(), wet, frames);
    } else {
        for (int ch = 0; ch < 2; ++ch) std::memcpy(wet[ch], dry_[static_cast<std::size_t>(ch)].data(), sizeof(float) * frames);
    }

    if (fadingOut_) {
        float* old[2] = {fade_[0].data(), fade_[1].data()};
        fadingOut_->process(feedPtrs_.data(), old, frames);
        for (int i = 0; i < frames; ++i) {
            float t = 1.0f - static_cast<float>(std::max(swapFadeRemaining_ - i, 0)) / static_cast<float>(swapFadeTotal_);
            for (int ch = 0; ch < 2; ++ch) wet[ch][i] = old[ch][i] + (wet[ch][i] - old[ch][i]) * t;
        }
        swapFadeRemaining_ -= frames;
        if (swapFadeRemaining_ <= 0) {
            lateBase_ += fadingOut_->lateBlockCount();
            retired_.store(fadingOut_.release(), std::memory_order_release);
        }
    }

    const float wetTarget = bypass_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    const float gainTarget = outputGain_.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        wetMix_ = wetTarget > wetMix_ ? std::min(wetTarget, wetMix_ + wetStep_) : std::max(wetTarget, wetMix_ - wetStep_);
        gainSmoothed_ += (gainTarget - gainSmoothed_) * gainCoef_;
        for (int ch = 0; ch < 2; ++ch) {
            // Output gain is a master volume: it applies to bypass too, so A/B
            // comparisons stay level-matched.
            float d = dry_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)];
            float w = wet[ch][i];
            output[ch][i] = (d + (w - d) * wetMix_) * gainSmoothed_;
        }
    }

    std::uint64_t late = lateBase_ + (current_ ? current_->lateBlockCount() : 0) +
                         (fadingOut_ ? fadingOut_->lateBlockCount() : 0);
    lateBlocks_.store(late, std::memory_order_relaxed);

    headphoneEq_.process(output, frames);
    leveler_.setEnabled(levelerEnabled_.load(std::memory_order_relaxed));
    leveler_.process(output, frames);
    levelerGainDb_.store(leveler_.currentGainDb(), std::memory_order_relaxed);
    limiter_.process(output, frames);
    limiterGainDb_.store(limiter_.currentGainDb(), std::memory_order_relaxed);
}

} // namespace brir

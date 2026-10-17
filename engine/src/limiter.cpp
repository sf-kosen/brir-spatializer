#include "brir/limiter.h"

#include <algorithm>
#include <cmath>

namespace brir {

Limiter::Limiter(int channels, double sampleRate, float ceilingDb, float lookaheadMs, float releaseMs)
    : channels_(channels),
      lookahead_(std::max(1, static_cast<int>(std::lround(lookaheadMs * sampleRate / 1000.0)))),
      ceiling_(std::pow(10.0f, ceilingDb / 20.0f)),
      // Reaches ~98% of a reduction within the lookahead.
      attackCoef_(1.0f - std::exp(-4.0f / static_cast<float>(std::max(1, static_cast<int>(lookaheadMs * sampleRate / 1000.0))))),
      releaseCoef_(1.0f - std::exp(-1.0f / static_cast<float>(releaseMs * sampleRate / 1000.0))),
      delay_(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(lookahead_), 0.0f)),
      required_(static_cast<std::size_t>(lookahead_), 1.0f)
{
}

float Limiter::currentGainDb() const noexcept
{
    return 20.0f * std::log10(std::max(gain_, 1e-6f));
}

void Limiter::process(float* const* io, int frames) noexcept
{
    for (int i = 0; i < frames; ++i) {
        float peak = 0.0f;
        for (int ch = 0; ch < channels_; ++ch) peak = std::max(peak, std::abs(io[ch][i]));
        required_[static_cast<std::size_t>(position_)] = peak > ceiling_ ? ceiling_ / peak : 1.0f;

        // Smallest gain needed by anything inside the lookahead window.
        float target = *std::min_element(required_.begin(), required_.end());
        gain_ += (target - gain_) * (target < gain_ ? attackCoef_ : releaseCoef_);

        for (int ch = 0; ch < channels_; ++ch) {
            float& slot = delay_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(position_)];
            float delayed = slot;
            slot = io[ch][i];
            io[ch][i] = std::clamp(delayed * gain_, -ceiling_, ceiling_);
        }
        position_ = position_ + 1 == lookahead_ ? 0 : position_ + 1;
    }
}

} // namespace brir

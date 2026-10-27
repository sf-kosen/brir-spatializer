#include "brir/leveler.h"

#include <algorithm>
#include <cmath>

namespace brir {

Leveler::Leveler(int channels, double sampleRate, float thresholdDb, float attackMs, float holdMs,
                 float releaseDbPerSecond)
    : channels_(channels),
      threshold_(std::pow(10.0f, thresholdDb / 20.0f)),
      attackCoef_(static_cast<float>(1.0 - std::exp(-1000.0 / (attackMs * sampleRate)))),
      holdSamples_(static_cast<int>(holdMs * sampleRate / 1000.0)),
      releaseStep_(static_cast<float>(std::pow(10.0, releaseDbPerSecond / (20.0 * sampleRate))))
{
}

float Leveler::currentGainDb() const noexcept
{
    return 20.0f * std::log10(std::max(gain_, 1e-6f));
}

void Leveler::process(float* const* io, int frames) noexcept
{
    for (int i = 0; i < frames; ++i) {
        float peak = 0.0f;
        for (int ch = 0; ch < channels_; ++ch) peak = std::max(peak, std::abs(io[ch][i]));
        float required = enabled_ && peak > threshold_ ? threshold_ / peak : 1.0f;

        if (required <= held_) {
            held_ = required;
            holdLeft_ = holdSamples_;
        } else if (holdLeft_ > 0 && enabled_) {
            --holdLeft_;
        } else {
            held_ = std::min(required, held_ * releaseStep_);
        }

        // Down smoothly (a one-pole), up at the release rate held_ already has.
        gain_ = held_ < gain_ ? gain_ + (held_ - gain_) * attackCoef_ : held_;

        for (int ch = 0; ch < channels_; ++ch) io[ch][i] *= gain_;
    }
}

} // namespace brir

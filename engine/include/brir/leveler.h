#pragma once

#include <vector>

namespace brir {

// Slow automatic headroom ahead of the limiter. Loud masters (peaks near
// 0 dBFS, made hotter by the room and the headphone EQ) would otherwise keep
// the fast limiter working on every beat; this instead lowers the whole
// programme by just enough that its peaks sit at the threshold, holds that
// gain while the loud passage lasts and recovers slowly afterwards. It only
// ever reduces gain, has no latency, and leaves material whose peaks stay below
// the threshold untouched. Overs during its attack are left to the limiter.
//
// Hold and release are long on purpose: it behaves like setting the volume
// once per loud stretch rather than like a compressor. On a -10 LUFS film PV a
// 1 s / 4 dB/s setting narrowed the loudness range from 5.7 to 4.0 LU; 8 s /
// 0.5 dB/s kept 5.3 LU with the limiter active in under 1% of blocks.
class Leveler {
public:
    Leveler(int channels, double sampleRate, float thresholdDb = -2.0f, float attackMs = 30.0f,
            float holdMs = 8000.0f, float releaseDbPerSecond = 0.5f);

    // Disabling lets the gain recover at the release rate (no jump).
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    float currentGainDb() const noexcept;

    void process(float* const* io, int frames) noexcept;

private:
    int channels_;
    float threshold_;
    float attackCoef_;
    int holdSamples_;
    float releaseStep_; // per-sample gain factor while recovering
    bool enabled_ = true;
    float held_ = 1.0f;  // lowest gain needed recently (the target)
    int holdLeft_ = 0;
    float gain_ = 1.0f;  // applied gain, follows held_
};

} // namespace brir

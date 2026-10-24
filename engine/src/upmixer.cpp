#include "brir/upmixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace brir {

namespace {

constexpr double kFrameSeconds = 512.0 / 48000.0; // ~10.7 ms at any rate
constexpr double kSmoothingSeconds = 0.04;
constexpr float kEps = 1e-12f;
constexpr double kDecorrelateDelaySeconds = 0.010;
constexpr double kDecorrelateFromHz = 100.0; // fades in up to twice this

int frameSizeFor(double sampleRate)
{
    int size = 256;
    while (size < kFrameSeconds * sampleRate * 0.99) size *= 2;
    return size;
}

} // namespace

Upmixer::Upmixer(double sampleRate)
    : frameSize_(frameSizeFor(sampleRate)),
      hop_(frameSize_ / 4),
      bins_(frameSize_ / 2 + 1),
      smoothing_(static_cast<float>(std::exp(-frameSize_ / 4.0 / (kSmoothingSeconds * sampleRate)))),
      // Effective number of independent averages: exponential smoothing gives
      // (1 + a) / (1 - a) frames, halved because 75%-overlapped frames correlate.
      coherenceBias_(2.0f * (1.0f - smoothing_) / (1.0f + smoothing_)),
      fft_(frameSize_),
      window_(static_cast<std::size_t>(frameSize_)),
      inL_(static_cast<std::size_t>(frameSize_)),
      inR_(static_cast<std::size_t>(frameSize_)),
      specL_(static_cast<std::size_t>(frameSize_)),
      specR_(static_cast<std::size_t>(frameSize_)),
      timeOut_(static_cast<std::size_t>(frameSize_)),
      powL_(static_cast<std::size_t>(bins_), 0.0f),
      powR_(static_cast<std::size_t>(bins_), 0.0f),
      crossRe_(static_cast<std::size_t>(bins_), 0.0f),
      crossIm_(static_cast<std::size_t>(bins_), 0.0f)
{
    for (int i = 0; i < frameSize_; ++i)
        window_[static_cast<std::size_t>(i)] =
            static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / frameSize_)));
    for (int f = 0; f < kInternal; ++f) {
        feedSpec_[f] = AlignedBuffer(static_cast<std::size_t>(frameSize_));
        overlap_[f].assign(static_cast<std::size_t>(frameSize_), 0.0f);
        ready_[f].assign(static_cast<std::size_t>(hop_), 0.0f);
    }
    decorrelateWeight_.resize(static_cast<std::size_t>(bins_));
    for (int k = 0; k < bins_; ++k) {
        double hz = k * sampleRate / frameSize_;
        decorrelateWeight_[static_cast<std::size_t>(k)] =
            static_cast<float>(std::clamp((hz - kDecorrelateFromHz) / kDecorrelateFromHz, 0.0, 1.0));
    }
    int delay = std::max(1, static_cast<int>(std::lround(kDecorrelateDelaySeconds * sampleRate)));
    for (auto& line : decorrelateDelay_) line.assign(static_cast<std::size_t>(delay), 0.0f);
}

Upmixer::~Upmixer() = default;

void Upmixer::process(const float* const* stereo, float* const* feeds, int frames) noexcept
{
    int index = 0;
    while (index < frames) {
        int n = std::min(frames - index, hop_ - pos_);
        // Emit the previous hop's output, then take in new input (in that order,
        // so the frame computed below is emitted during the next hop: latency = frameSize).
        for (int f = 0; f < kFeeds; ++f) std::memcpy(feeds[f] + index, ready_[f].data() + pos_, sizeof(float) * n);
        std::memcpy(inL_.data() + frameSize_ - hop_ + pos_, stereo[0] + index, sizeof(float) * n);
        std::memcpy(inR_.data() + frameSize_ - hop_ + pos_, stereo[1] + index, sizeof(float) * n);
        pos_ += n;
        index += n;
        if (pos_ == hop_) {
            processFrame();
            pos_ = 0;
        }
    }
}

void Upmixer::processFrame() noexcept
{
    const int n = frameSize_;
    float* tmp = timeOut_.data();

    for (int i = 0; i < n; ++i) tmp[i] = inL_.data()[i] * window_[static_cast<std::size_t>(i)];
    fft_.forwardOrdered(tmp, specL_.data());
    for (int i = 0; i < n; ++i) tmp[i] = inR_.data()[i] * window_[static_cast<std::size_t>(i)];
    fft_.forwardOrdered(tmp, specR_.data());

    // Slide the input window by one hop.
    std::memmove(inL_.data(), inL_.data() + hop_, sizeof(float) * (n - hop_));
    std::memmove(inR_.data(), inR_.data() + hop_, sizeof(float) * (n - hop_));

    const bool enabled = enabled_.load(std::memory_order_relaxed);
    const float center = std::clamp(centerAmount_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float sigma = std::clamp(surroundAmount_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float ambFront = std::sqrt(1.0f - sigma);
    const float ambBack = std::sqrt(sigma);
    const float a = smoothing_;
    const float halfPi = static_cast<float>(std::numbers::pi / 2.0);

    const float* sl = specL_.data();
    const float* sr = specR_.data();
    float* out[kInternal];
    for (int f = 0; f < kInternal; ++f) out[f] = feedSpec_[f].data();

    for (int k = 0; k < bins_; ++k) {
        // Ordered layout: [DC, Nyquist, re1, im1, ...]; DC and Nyquist are real.
        int re, im;
        if (k == 0) re = 0, im = -1;
        else if (k == bins_ - 1) re = 1, im = -1;
        else re = 2 * k, im = 2 * k + 1;
        float lr = sl[re], li = im >= 0 ? sl[im] : 0.0f;
        float rr = sr[re], ri = im >= 0 ? sr[im] : 0.0f;

        std::size_t b = static_cast<std::size_t>(k);
        powL_[b] = a * powL_[b] + (1.0f - a) * (lr * lr + li * li);
        powR_[b] = a * powR_[b] + (1.0f - a) * (rr * rr + ri * ri);
        // L * conj(R)
        crossRe_[b] = a * crossRe_[b] + (1.0f - a) * (lr * rr + li * ri);
        crossIm_[b] = a * crossIm_[b] + (1.0f - a) * (li * rr - lr * ri);

        float o[kInternal][2] = {};
        if (!enabled) {
            o[L][0] = lr, o[L][1] = li;
            o[R][0] = rr, o[R][1] = ri;
        } else {
            float pL = powL_[b], pR = powR_[b];
            // Short-term averages of uncorrelated signals still show some
            // coherence (expected |coherence|^2 ~ 1 / averages); remove that
            // bias so ambience isn't mistaken for a centred source.
            float rawMag2 = crossRe_[b] * crossRe_[b] + crossIm_[b] * crossIm_[b];
            float coherence2 = rawMag2 / (pL * pR + kEps);
            float corrected2 = std::clamp((coherence2 - coherenceBias_) / (1.0f - coherenceBias_), 0.0f, 1.0f);
            float cMag = std::sqrt(corrected2 * pL * pR);
            float rawMag = std::sqrt(rawMag2);
            float diff = pL - pR;
            float pn = std::max(0.0f, 0.5f * (pL + pR - std::sqrt(diff * diff + 4.0f * cMag * cMag)));
            float psL = std::max(0.0f, pL - pn), psR = std::max(0.0f, pR - pn);

            float mpL = std::sqrt(psL / (pL + kEps)), maL = std::sqrt(pn / (pL + kEps));
            float mpR = std::sqrt(psR / (pR + kEps)), maR = std::sqrt(pn / (pR + kEps));

            // Primary as one source: phase-align R to L, keep the primary energy.
            float ur = rawMag > kEps ? crossRe_[b] / rawMag : 1.0f;
            float ui = rawMag > kEps ? crossIm_[b] / rawMag : 0.0f;
            float sRe = mpL * lr + mpR * (rr * ur - ri * ui);
            float sIm = mpL * li + mpR * (rr * ui + ri * ur);
            float sEnergy = mpL * mpL * (lr * lr + li * li) + mpR * mpR * (rr * rr + ri * ri);
            float sMag = std::sqrt(sRe * sRe + sIm * sIm);
            float scale = sMag > kEps ? std::sqrt(sEnergy) / sMag : 0.0f;
            sRe *= scale;
            sIm *= scale;

            // Direction from the primary level ratio: 0 = left, 0.5 = centre, 1 = right.
            float position = std::atan2(std::sqrt(psR), std::sqrt(psL)) / halfPi;
            // Out-of-phase primary goes to the surrounds; phase only matters
            // when both sides carry the source.
            float balance = 2.0f * std::sqrt(psL * psR) / (psL + psR + kEps);
            float cosPhase = rawMag > kEps ? crossRe_[b] / rawMag : 1.0f;
            float effectiveCos = cosPhase * balance + (1.0f - balance);
            float front = std::sqrt(std::clamp(0.5f * (1.0f + effectiveCos), 0.0f, 1.0f));
            float back = std::sqrt(std::max(0.0f, 1.0f - front * front));

            // L/C/R gains: stereo pan blended (by energy) with L-C-R pairwise pan.
            float stL = std::cos(position * halfPi), stR = std::sin(position * halfPi);
            float lcrL = 0.0f, lcrC, lcrR = 0.0f;
            if (position <= 0.5f) {
                float t = position * 2.0f;
                lcrL = std::cos(t * halfPi);
                lcrC = std::sin(t * halfPi);
            } else {
                float t = (position - 0.5f) * 2.0f;
                lcrC = std::cos(t * halfPi);
                lcrR = std::sin(t * halfPi);
            }
            // The primary part is panned across L-C-R regardless of coherence:
            // the ambience is already split off, and pairwise panning keeps the
            // stereo width while centred music plays from C as in a cinema. A
            // phantom centre from L/R alone colours the binaural render (mids
            // up, presence down) because the calibration assumes C carries it.
            float toCentre = center;
            float gL = std::sqrt((1.0f - toCentre) * stL * stL + toCentre * lcrL * lcrL);
            float gC = std::sqrt(toCentre) * lcrC;
            float gR = std::sqrt((1.0f - toCentre) * stR * stR + toCentre * lcrR * lcrR);

            float w = decorrelateWeight_[b];
            float gains[kInternal] = {gL * front,     gC * front,     gR * front,         stL * back * (1.0f - w),
                                      stR * back * (1.0f - w), stL * back * w, stR * back * w};
            for (int f = 0; f < kInternal; ++f) o[f][0] = gains[f] * sRe, o[f][1] = gains[f] * sIm;

            float aLr = maL * lr, aLi = maL * li, aRr = maR * rr, aRi = maR * ri;
            o[L][0] += ambFront * aLr, o[L][1] += ambFront * aLi;
            o[R][0] += ambFront * aRr, o[R][1] += ambFront * aRi;
            o[Ls][0] += ambBack * aLr, o[Ls][1] += ambBack * aLi;
            o[Rs][0] += ambBack * aRr, o[Rs][1] += ambBack * aRi;

            // Primary and ambient estimates are scaled copies of the same input,
            // so where they meet in one channel they add coherently. Keep the
            // bin's total output energy equal to its input energy.
            float inEnergy = lr * lr + li * li + rr * rr + ri * ri;
            // (The decorrelated parts keep their energy: (M +- M delayed)/sqrt2.)
            float outEnergy = 0.0f;
            for (int f = 0; f < kInternal; ++f) outEnergy += o[f][0] * o[f][0] + o[f][1] * o[f][1];
            if (outEnergy > kEps) {
                float g = std::clamp(std::sqrt(inEnergy / outEnergy), 0.5f, 2.0f);
                for (int f = 0; f < kInternal; ++f) o[f][0] *= g, o[f][1] *= g;
            }
        }

        for (int f = 0; f < kInternal; ++f) {
            out[f][re] = o[f][0];
            if (im >= 0) out[f][im] = o[f][1];
        }
    }

    // Synthesis: sqrt-Hann again (Hann overall), which sums to 2 at 75% overlap.
    const float norm = 0.5f / static_cast<float>(n);
    for (int f = 0; f < kInternal; ++f) {
        fft_.inverseOrdered(out[f], tmp);
        float* acc = overlap_[f].data();
        for (int i = 0; i < n; ++i) acc[i] += tmp[i] * window_[static_cast<std::size_t>(i)] * norm;
        std::memcpy(ready_[f].data(), acc, sizeof(float) * hop_);
        std::memmove(acc, acc + hop_, sizeof(float) * (n - hop_));
        std::fill(acc + n - hop_, acc + n, 0.0f);
    }
    decorrelate();
}

// Complementary comb filters on the surround primary: Ls += (M + M(t - D)) / sqrt2,
// Rs += (M - M(t - D)) / sqrt2. The two arrays become decorrelated while their
// combined spectrum stays flat.
void Upmixer::decorrelate() noexcept
{
    constexpr float kHalfSqrt2 = 0.70710678f;
    const int length = static_cast<int>(decorrelateDelay_[0].size());
    const float* mL = ready_[DecorrelateL].data();
    const float* mR = ready_[DecorrelateR].data();
    float* ls = ready_[Ls].data();
    float* rs = ready_[Rs].data();
    for (int i = 0; i < hop_; ++i) {
        std::size_t p = static_cast<std::size_t>(decorrelateDelayPos_);
        float delayedL = decorrelateDelay_[0][p], delayedR = decorrelateDelay_[1][p];
        decorrelateDelay_[0][p] = mL[i];
        decorrelateDelay_[1][p] = mR[i];
        ls[i] += (mL[i] + delayedL) * kHalfSqrt2;
        rs[i] += (mR[i] - delayedR) * kHalfSqrt2;
        decorrelateDelayPos_ = decorrelateDelayPos_ + 1 == length ? 0 : decorrelateDelayPos_ + 1;
    }
}

} // namespace brir

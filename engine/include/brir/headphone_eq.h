#pragma once

#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace brir {

// One parametric EQ band (RBJ cookbook shapes).
struct EqBand {
    enum Type { Peak, LowShelf, HighShelf };
    Type type;
    float frequency;
    float gainDb;
    float q;
};

// Stereo cascade of biquads correcting the playback headphones' response.
// Zero latency. setProfile() and responseDb() belong to one (message) thread;
// the audio thread picks a new profile up at the start of its next block and
// never frees memory.
class HeadphoneEq {
public:
    explicit HeadphoneEq(double sampleRate);
    ~HeadphoneEq();

    HeadphoneEq(const HeadphoneEq&) = delete;
    HeadphoneEq& operator=(const HeadphoneEq&) = delete;

    // Empty bands switch the correction off. Not real-time safe (allocates).
    void setProfile(std::span<const EqBand> bands, float preampDb);

    // Audio thread. Planar stereo, in place.
    void process(float* const* io, int frames) noexcept;

    // Magnitude response in dB of the most recently set profile.
    float responseDb(double frequency) const;

    // Highest gain of the bands (without preamp) over 20 Hz - 20 kHz, for
    // choosing a preamp that keeps the cascade from boosting.
    static float peakGainDb(std::span<const EqBand> bands, double sampleRate);

    // Power-averaged gain of the bands over 20 Hz - 20 kHz on a log axis (the
    // level change for pink noise), for choosing a loudness-matched preamp.
    static float averageGainDb(std::span<const EqBand> bands, double sampleRate);

private:
    struct Coefficients {
        double b0, b1, b2, a1, a2;
    };
    struct Profile {
        std::vector<Coefficients> sections;
        float preamp = 1.0f;
    };

    static constexpr std::size_t kMaxSections = 32;

    double sampleRate_;
    std::atomic<Profile*> pending_{nullptr}; // set, not yet picked up
    std::atomic<Profile*> retired_{nullptr}; // dropped by the audio thread, freed by setProfile()
    std::unique_ptr<Profile> active_;        // audio thread
    const Profile* latest_ = nullptr;        // message thread, for responseDb()
    double state_[2][kMaxSections][2] = {};  // per channel and section (transposed direct form II)
};

} // namespace brir

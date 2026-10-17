#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace brir {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // padding from alignas is intentional (no false sharing)
#endif

// Lock-free single-producer / single-consumer FIFO of interleaved audio frames.
class SpscRing {
public:
    SpscRing(int channels, int minCapacityFrames);

    int channels() const noexcept { return channels_; }
    int capacity() const noexcept { return capacity_; }

    // Producer side. Return the number of frames actually written.
    int write(const float* interleaved, int frames) noexcept;
    int writeSilence(int frames) noexcept;

    // Consumer side. Returns the number of frames actually read.
    int read(float* interleaved, int frames) noexcept;

    int available() const noexcept;

private:
    template <typename CopyFn>
    int writeImpl(int frames, CopyFn copy) noexcept;

    int channels_;
    int capacity_; // power of two, in frames
    std::vector<float> data_;
    alignas(64) std::atomic<std::uint64_t> writePos_{0};
    alignas(64) std::atomic<std::uint64_t> readPos_{0};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace brir

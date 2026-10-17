#include "brir/spsc_ring.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace brir {

SpscRing::SpscRing(int channels, int minCapacityFrames)
    : channels_(channels), capacity_(1)
{
    if (channels <= 0 || minCapacityFrames <= 0) throw std::invalid_argument("Invalid ring size.");
    while (capacity_ < minCapacityFrames) capacity_ *= 2;
    data_.assign(static_cast<std::size_t>(capacity_) * channels_, 0.0f);
}

template <typename CopyFn>
int SpscRing::writeImpl(int frames, CopyFn copy) noexcept
{
    std::uint64_t write = writePos_.load(std::memory_order_relaxed);
    std::uint64_t read = readPos_.load(std::memory_order_acquire);
    int space = capacity_ - static_cast<int>(write - read);
    int n = std::min(frames, space);
    if (n <= 0) return 0;

    int start = static_cast<int>(write & static_cast<std::uint64_t>(capacity_ - 1));
    int first = std::min(n, capacity_ - start);
    copy(0, start, first);
    copy(first, 0, n - first);
    writePos_.store(write + static_cast<std::uint64_t>(n), std::memory_order_release);
    return n;
}

int SpscRing::write(const float* interleaved, int frames) noexcept
{
    return writeImpl(frames, [&](int srcFrame, int dstFrame, int count) {
        std::memcpy(data_.data() + static_cast<std::size_t>(dstFrame) * channels_,
                    interleaved + static_cast<std::size_t>(srcFrame) * channels_,
                    sizeof(float) * static_cast<std::size_t>(count) * channels_);
    });
}

int SpscRing::writeSilence(int frames) noexcept
{
    return writeImpl(frames, [&](int, int dstFrame, int count) {
        std::fill_n(data_.data() + static_cast<std::size_t>(dstFrame) * channels_,
                    static_cast<std::size_t>(count) * channels_, 0.0f);
    });
}

int SpscRing::read(float* interleaved, int frames) noexcept
{
    std::uint64_t read = readPos_.load(std::memory_order_relaxed);
    std::uint64_t write = writePos_.load(std::memory_order_acquire);
    int n = std::min(frames, static_cast<int>(write - read));
    if (n <= 0) return 0;

    int start = static_cast<int>(read & static_cast<std::uint64_t>(capacity_ - 1));
    int first = std::min(n, capacity_ - start);
    std::memcpy(interleaved, data_.data() + static_cast<std::size_t>(start) * channels_,
                sizeof(float) * static_cast<std::size_t>(first) * channels_);
    std::memcpy(interleaved + static_cast<std::size_t>(first) * channels_, data_.data(),
                sizeof(float) * static_cast<std::size_t>(n - first) * channels_);
    readPos_.store(read + static_cast<std::uint64_t>(n), std::memory_order_release);
    return n;
}

int SpscRing::available() const noexcept
{
    std::uint64_t write = writePos_.load(std::memory_order_acquire);
    std::uint64_t read = readPos_.load(std::memory_order_acquire);
    return static_cast<int>(write - read);
}

} // namespace brir

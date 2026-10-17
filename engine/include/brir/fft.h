#pragma once

#include <cstddef>
#include <memory>

struct PFFFT_Setup;

namespace brir {

// Float buffer with the SIMD alignment pffft requires. Zero-initialised.
class AlignedBuffer {
public:
    AlignedBuffer() = default;
    explicit AlignedBuffer(std::size_t size);

    float* data() noexcept { return data_.get(); }
    const float* data() const noexcept { return data_.get(); }
    std::size_t size() const noexcept { return size_; }
    void clear() noexcept;

private:
    struct Deleter {
        void operator()(float* p) const noexcept;
    };

    std::unique_ptr<float[], Deleter> data_;
    std::size_t size_ = 0;
};

// Real-input FFT of a fixed size. Spectra are kept in pffft's internal
// (unordered) layout, which is only meant to be multiplied and transformed back.
// Not thread-safe: each instance owns a scratch buffer.
class RealFft {
public:
    explicit RealFft(int size);
    ~RealFft();

    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    int size() const noexcept { return size_; }

    void forward(const float* input, float* spectrum) noexcept;
    // Unnormalised: forward followed by inverse scales by size().
    void inverse(const float* spectrum, float* output) noexcept;
    // acc += a * b * scale
    void multiplyAccumulate(const float* a, const float* b, float* acc, float scale) const noexcept;

private:
    int size_;
    PFFFT_Setup* setup_;
    AlignedBuffer work_;
};

} // namespace brir

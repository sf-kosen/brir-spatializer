#include "brir/fft.h"

#include <pffft/pffft.h>

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string>

namespace brir {

AlignedBuffer::AlignedBuffer(std::size_t size)
    : data_(static_cast<float*>(pffft_aligned_malloc(std::max<std::size_t>(size, 1) * sizeof(float)))),
      size_(size)
{
    if (!data_) throw std::bad_alloc();
    clear();
}

void AlignedBuffer::clear() noexcept
{
    if (data_) std::fill_n(data_.get(), size_, 0.0f);
}

void AlignedBuffer::Deleter::operator()(float* p) const noexcept
{
    pffft_aligned_free(p);
}

RealFft::RealFft(int size)
    : size_(size), setup_(nullptr), work_(static_cast<std::size_t>(size))
{
    if (!pffft_is_valid_size(size, PFFFT_REAL))
        throw std::invalid_argument("Unsupported real FFT size: " + std::to_string(size));
    setup_ = pffft_new_setup(size, PFFFT_REAL);
    if (!setup_) throw std::bad_alloc();
}

RealFft::~RealFft()
{
    pffft_destroy_setup(setup_);
}

void RealFft::forward(const float* input, float* spectrum) noexcept
{
    pffft_transform(setup_, input, spectrum, work_.data(), PFFFT_FORWARD);
}

void RealFft::inverse(const float* spectrum, float* output) noexcept
{
    pffft_transform(setup_, spectrum, output, work_.data(), PFFFT_BACKWARD);
}

void RealFft::multiplyAccumulate(const float* a, const float* b, float* acc, float scale) const noexcept
{
    pffft_zconvolve_accumulate(setup_, a, b, acc, scale);
}

} // namespace brir

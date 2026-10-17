#pragma once

#include "brir/fft.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace brir {

// Impulse responses indexed as irs[input][output]. An empty vector means no path.
using IrMatrix = std::vector<std::vector<std::vector<float>>>;

struct ConvolverConfig {
    // Block size of the synchronous head stage; this is the convolver latency.
    // Must be a positive multiple of 16.
    int headBlockSize = 128;
    // Block size ratio between successive stages.
    int growthFactor = 8;
    // Stages stop growing once the next block size would exceed this.
    int maxBlockSize = 8192;
    // Run tail stages on worker threads. When false, everything runs in the
    // caller's thread, which is deterministic and meant for offline use and tests.
    bool threaded = true;
    // Called once on each worker thread before it starts (e.g. to join MMCSS).
    std::function<void()> onWorkerStart;
};

// Non-uniformly partitioned overlap-save convolution of an N-input, M-output IR
// matrix. Each input is transformed once per block and the products are summed
// per output in the frequency domain.
//
// The head stage runs inside process(). Every larger stage starts at an IR offset
// of at least twice its own block size, so a worker has at least one of its blocks
// of slack before the result is due.
class Convolver {
public:
    Convolver(int numInputs, int numOutputs, const IrMatrix& irs, ConvolverConfig config = {});
    ~Convolver();

    Convolver(const Convolver&) = delete;
    Convolver& operator=(const Convolver&) = delete;

    int numInputs() const noexcept { return numInputs_; }
    int numOutputs() const noexcept { return numOutputs_; }
    int latencySamples() const noexcept { return headBlockSize_; }
    int numStages() const noexcept;

    // Real-time safe. Planar buffers, any frame count. Output may alias input.
    void process(const float* const* input, float* const* output, int numFrames) noexcept;

    // Blocks until every tail block that has been fed is finished.
    // For offline rendering and tests only.
    void waitForWorkers() const;

    // Tail blocks whose result was not ready in time (output for them was dropped).
    std::uint64_t lateBlockCount() const noexcept;

private:
    class Stage;
    class TailStage;

    int numInputs_;
    int numOutputs_;
    int headBlockSize_;

    std::unique_ptr<Stage> head_;
    std::vector<std::unique_ptr<TailStage>> tails_;

    std::vector<AlignedBuffer> headIn_;
    std::vector<AlignedBuffer> headOut_;
    std::vector<const float*> headInPtrs_;
    std::vector<float*> headOutPtrs_;
    int headPos_ = 0;
    std::int64_t framesProcessed_ = 0;
};

} // namespace brir

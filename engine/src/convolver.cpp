#include "brir/convolver.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <semaphore>
#include <stdexcept>
#include <thread>

namespace brir {

namespace {

struct StagePlan {
    int blockSize;
    int offset; // first IR sample covered by this stage
    int length; // IR samples covered, a multiple of blockSize
};

int roundUp(int value, int multiple)
{
    return (value + multiple - 1) / multiple * multiple;
}

// Head stage covers [0, 2 * L1), stage k covers [2 * Lk, 2 * Lk+1), and the last
// stage covers the rest. Keeping every stage offset at >= 2 * its block size
// guarantees a full block of slack for the worker computing it.
std::vector<StagePlan> planStages(int irLength, const ConvolverConfig& config)
{
    std::vector<StagePlan> plan;
    int blockSize = config.headBlockSize;
    int offset = 0;
    do {
        int next = blockSize * config.growthFactor;
        bool grows = config.growthFactor > 1 && next <= config.maxBlockSize;
        int end = grows ? std::min(irLength, 2 * next) : irLength;
        int length = roundUp(std::max(end - offset, 1), blockSize);
        plan.push_back({blockSize, offset, length});
        offset += length;
        if (grows) blockSize = next;
    } while (offset < irLength);
    return plan;
}

std::size_t maxIrLength(const IrMatrix& irs)
{
    std::size_t length = 0;
    for (const auto& row : irs)
        for (const auto& ir : row) length = std::max(length, ir.size());
    return length;
}

} // namespace

// Uniformly partitioned overlap-save convolution of one IR segment.
class Convolver::Stage {
public:
    Stage(int numInputs, int numOutputs, const IrMatrix& irs, const StagePlan& plan)
        : blockSize_(plan.blockSize),
          fftSize_(2 * plan.blockSize),
          partitions_(plan.length / plan.blockSize),
          numInputs_(numInputs),
          numOutputs_(numOutputs),
          fft_(fftSize_),
          pathPartitions_(static_cast<std::size_t>(numInputs * numOutputs), 0),
          inputUsed_(static_cast<std::size_t>(numInputs), false),
          acc_(static_cast<std::size_t>(fftSize_)),
          timeOut_(static_cast<std::size_t>(fftSize_))
    {
        AlignedBuffer segment(static_cast<std::size_t>(fftSize_));
        for (int in = 0; in < numInputs; ++in) {
            for (int out = 0; out < numOutputs; ++out) {
                const std::vector<float>& ir = irs[static_cast<std::size_t>(in)][static_cast<std::size_t>(out)];
                int available = static_cast<int>(ir.size()) - plan.offset;
                int used = available > 0 ? std::min(partitions_, (available + blockSize_ - 1) / blockSize_) : 0;
                std::size_t path = pathIndex(in, out);
                pathPartitions_[path] = used;
                irSpectra_.emplace_back(static_cast<std::size_t>(used) * fftSize_);
                for (int p = 0; p < used; ++p) {
                    segment.clear();
                    int start = plan.offset + p * blockSize_;
                    int count = std::min(blockSize_, static_cast<int>(ir.size()) - start);
                    std::copy_n(ir.data() + start, count, segment.data());
                    fft_.forward(segment.data(), irSpectra_[path].data() + static_cast<std::size_t>(p) * fftSize_);
                }
                if (used > 0) inputUsed_[static_cast<std::size_t>(in)] = true;
            }
        }

        for (int in = 0; in < numInputs; ++in) {
            history_.emplace_back(static_cast<std::size_t>(fftSize_));
            fdl_.emplace_back(static_cast<std::size_t>(partitions_) * fftSize_);
        }
    }

    int blockSize() const noexcept { return blockSize_; }

    // Consumes blockSize() samples per input and writes blockSize() samples per output.
    void processBlock(const float* const* input, float* const* output) noexcept
    {
        fdlHead_ = (fdlHead_ + 1) % partitions_;
        for (int in = 0; in < numInputs_; ++in) {
            if (!inputUsed_[static_cast<std::size_t>(in)]) continue;
            float* history = history_[static_cast<std::size_t>(in)].data();
            std::memmove(history, history + blockSize_, sizeof(float) * blockSize_);
            std::memcpy(history + blockSize_, input[in], sizeof(float) * blockSize_);
            fft_.forward(history, spectrumAt(in, fdlHead_));
        }

        const float scale = 1.0f / static_cast<float>(fftSize_);
        for (int out = 0; out < numOutputs_; ++out) {
            acc_.clear();
            for (int in = 0; in < numInputs_; ++in) {
                std::size_t path = pathIndex(in, out);
                const float* ir = irSpectra_[path].data();
                for (int p = 0; p < pathPartitions_[path]; ++p) {
                    int slot = (fdlHead_ - p + partitions_) % partitions_;
                    fft_.multiplyAccumulate(spectrumAt(in, slot), ir + static_cast<std::size_t>(p) * fftSize_,
                                            acc_.data(), scale);
                }
            }
            fft_.inverse(acc_.data(), timeOut_.data());
            std::memcpy(output[out], timeOut_.data() + blockSize_, sizeof(float) * blockSize_);
        }
    }

private:
    std::size_t pathIndex(int in, int out) const noexcept
    {
        return static_cast<std::size_t>(in * numOutputs_ + out);
    }

    float* spectrumAt(int in, int slot) noexcept
    {
        return fdl_[static_cast<std::size_t>(in)].data() + static_cast<std::size_t>(slot) * fftSize_;
    }

    int blockSize_;
    int fftSize_;
    int partitions_;
    int numInputs_;
    int numOutputs_;
    RealFft fft_;
    std::vector<int> pathPartitions_;
    std::vector<bool> inputUsed_;
    std::vector<AlignedBuffer> irSpectra_; // [in * numOutputs + out], partitions x fftSize
    std::vector<AlignedBuffer> history_;   // per input: previous block + current block
    std::vector<AlignedBuffer> fdl_;       // per input: frequency-domain delay line
    AlignedBuffer acc_;
    AlignedBuffer timeOut_;
    int fdlHead_ = 0;
};

// A stage fed from and drained into ring buffers indexed by absolute sample time,
// computed either on its own worker thread or inline.
//
// Block m consumes input samples [mL, (m+1)L) and produces the convolution
// output for times [mL + O, mL + O + L), which the head path emits B samples
// later. It becomes computable at time (m+1)L, so the slack is O + B - L >= L + B.
class Convolver::TailStage {
public:
    TailStage(int numInputs, int numOutputs, const IrMatrix& irs, const StagePlan& plan,
              const ConvolverConfig& config)
        : stage_(numInputs, numOutputs, irs, plan),
          blockSize_(plan.blockSize),
          offset_(plan.offset),
          inputCapacity_(4 * plan.blockSize),
          outputCapacity_(plan.offset + 4 * plan.blockSize),
          inputPtrs_(static_cast<std::size_t>(numInputs)),
          outputPtrs_(static_cast<std::size_t>(numOutputs))
    {
        for (int in = 0; in < numInputs; ++in) inputRing_.emplace_back(static_cast<std::size_t>(inputCapacity_));
        for (int out = 0; out < numOutputs; ++out) outputRing_.emplace_back(static_cast<std::size_t>(outputCapacity_));

        if (config.threaded) {
            worker_ = std::thread([this, onStart = config.onWorkerStart] {
                if (onStart) onStart();
                run();
            });
        }
    }

    ~TailStage()
    {
        if (worker_.joinable()) {
            stop_.store(true, std::memory_order_release);
            wake_.release();
            worker_.join();
        }
    }

    // Called by the audio thread. n never crosses a head block boundary, so it
    // never wraps a ring or straddles one of this stage's blocks.
    void pushInput(const float* const* input, int inputOffset, int n) noexcept
    {
        std::size_t position = static_cast<std::size_t>(written_ % inputCapacity_);
        for (std::size_t in = 0; in < inputRing_.size(); ++in)
            std::memcpy(inputRing_[in].data() + position, input[in] + inputOffset, sizeof(float) * n);
        written_ += n;
        if (written_ % blockSize_ != 0) return;

        available_.store(written_ / blockSize_, std::memory_order_release);
        if (worker_.joinable()) wake_.release();
        else processPending();
    }

    // Adds this stage's contribution for convolution-output times [time, time + n).
    void addOutput(float* const* output, int outputOffset, std::int64_t time, int n) noexcept
    {
        if (time < offset_) return;
        std::int64_t block = (time - offset_) / blockSize_;
        if (completed_.load(std::memory_order_acquire) <= block) {
            if (block != lastLateBlock_) {
                lastLateBlock_ = block;
                late_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        std::size_t position = static_cast<std::size_t>(time % outputCapacity_);
        for (std::size_t out = 0; out < outputRing_.size(); ++out) {
            const float* src = outputRing_[out].data() + position;
            float* dst = output[out] + outputOffset;
            for (int i = 0; i < n; ++i) dst[i] += src[i];
        }
    }

    void waitIdle() const
    {
        while (completed_.load(std::memory_order_acquire) < available_.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    std::uint64_t lateBlocks() const noexcept { return late_.load(std::memory_order_relaxed); }

private:
    void run()
    {
        for (;;) {
            wake_.acquire();
            if (stop_.load(std::memory_order_acquire)) return;
            processPending();
        }
    }

    void processPending() noexcept
    {
        std::int64_t available = available_.load(std::memory_order_acquire);
        for (std::int64_t block = completed_.load(std::memory_order_relaxed); block < available; ++block) {
            std::int64_t start = block * blockSize_;
            std::size_t inPos = static_cast<std::size_t>(start % inputCapacity_);
            std::size_t outPos = static_cast<std::size_t>((start + offset_) % outputCapacity_);
            for (std::size_t in = 0; in < inputRing_.size(); ++in) inputPtrs_[in] = inputRing_[in].data() + inPos;
            for (std::size_t out = 0; out < outputRing_.size(); ++out) outputPtrs_[out] = outputRing_[out].data() + outPos;
            stage_.processBlock(inputPtrs_.data(), outputPtrs_.data());
            completed_.store(block + 1, std::memory_order_release);
        }
    }

    Stage stage_;
    int blockSize_;
    int offset_;
    int inputCapacity_;  // multiple of blockSize_
    int outputCapacity_; // multiple of blockSize_, > offset + head block
    std::vector<AlignedBuffer> inputRing_;
    std::vector<AlignedBuffer> outputRing_;
    std::vector<const float*> inputPtrs_;
    std::vector<float*> outputPtrs_;

    std::int64_t written_ = 0;        // audio thread only
    std::int64_t lastLateBlock_ = -1; // audio thread only
    std::atomic<std::int64_t> available_{0};
    std::atomic<std::int64_t> completed_{0};
    std::atomic<std::uint64_t> late_{0};

    std::thread worker_;
    std::counting_semaphore<> wake_{0};
    std::atomic<bool> stop_{false};
};

Convolver::Convolver(int numInputs, int numOutputs, const IrMatrix& irs, ConvolverConfig config)
    : numInputs_(numInputs), numOutputs_(numOutputs), headBlockSize_(config.headBlockSize)
{
    if (numInputs <= 0 || numOutputs <= 0) throw std::invalid_argument("Convolver needs inputs and outputs.");
    if (config.headBlockSize <= 0 || config.headBlockSize % 16 != 0)
        throw std::invalid_argument("headBlockSize must be a positive multiple of 16.");
    if (irs.size() != static_cast<std::size_t>(numInputs))
        throw std::invalid_argument("IR matrix row count must equal numInputs.");
    for (const auto& row : irs)
        if (row.size() != static_cast<std::size_t>(numOutputs))
            throw std::invalid_argument("IR matrix column count must equal numOutputs.");

    std::vector<StagePlan> plan = planStages(static_cast<int>(maxIrLength(irs)), config);
    head_ = std::make_unique<Stage>(numInputs, numOutputs, irs, plan.front());
    for (std::size_t i = 1; i < plan.size(); ++i)
        tails_.push_back(std::make_unique<TailStage>(numInputs, numOutputs, irs, plan[i], config));

    for (int in = 0; in < numInputs; ++in) {
        headIn_.emplace_back(static_cast<std::size_t>(headBlockSize_));
        headInPtrs_.push_back(headIn_.back().data());
    }
    for (int out = 0; out < numOutputs; ++out) {
        headOut_.emplace_back(static_cast<std::size_t>(headBlockSize_));
        headOutPtrs_.push_back(headOut_.back().data());
    }
}

Convolver::~Convolver() = default;

int Convolver::numStages() const noexcept
{
    return 1 + static_cast<int>(tails_.size());
}

void Convolver::process(const float* const* input, float* const* output, int numFrames) noexcept
{
    int index = 0;
    while (index < numFrames) {
        int n = std::min(numFrames - index, headBlockSize_ - headPos_);

        // Consume input before writing output so in-place processing works.
        for (int in = 0; in < numInputs_; ++in)
            std::memcpy(headIn_[static_cast<std::size_t>(in)].data() + headPos_, input[in] + index, sizeof(float) * n);
        for (auto& tail : tails_) tail->pushInput(input, index, n);

        // Emit the convolution output for time (now - latency).
        std::int64_t time = framesProcessed_ - headBlockSize_;
        for (int out = 0; out < numOutputs_; ++out)
            std::memcpy(output[out] + index, headOut_[static_cast<std::size_t>(out)].data() + headPos_, sizeof(float) * n);
        for (auto& tail : tails_) tail->addOutput(output, index, time, n);

        headPos_ += n;
        framesProcessed_ += n;
        index += n;
        if (headPos_ == headBlockSize_) {
            head_->processBlock(headInPtrs_.data(), headOutPtrs_.data());
            headPos_ = 0;
        }
    }
}

void Convolver::waitForWorkers() const
{
    for (const auto& tail : tails_) tail->waitIdle();
}

std::uint64_t Convolver::lateBlockCount() const noexcept
{
    std::uint64_t total = 0;
    for (const auto& tail : tails_) total += tail->lateBlocks();
    return total;
}

} // namespace brir

#pragma once

#include "brir/clock_bridge.h"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brir::io {

struct CaptureDevice {
    std::wstring id;
    std::string name; // UTF-8
    // A render endpoint captured via WASAPI loopback (rather than a capture endpoint).
    bool loopback = false;
};

// Active render endpoints (as loopback sources) followed by active capture endpoints.
std::vector<CaptureDevice> listCaptureDevices();

struct CaptureStats {
    double sampleRate = 0.0;
    int deviceChannels = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t silenceFills = 0; // times the source went idle and was padded with silence
};

// One capture-thread event, for diagnosing delivery timing.
struct PacketRecord {
    enum Kind : int { Data, Silent, SilenceFill, Overlap };
    double wallTime = 0.0;       // when the capture thread handled it
    double firstFrameTime = 0.0; // device timestamp of its first frame
    int frames = 0;
    Kind kind = Data;
};

// Captures a WASAPI endpoint in shared mode on its own MMCSS thread, downmixes
// to stereo and hands the audio to a ClockBridge that the output side reads.
class CaptureStream {
public:
    CaptureStream() = default;
    ~CaptureStream();

    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;

    // Opens the device and starts capturing. Blocks until the stream is running;
    // throws std::runtime_error on failure. bridgeConfig's channels and inputRate
    // are filled in from the device.
    void start(const CaptureDevice& device, ClockBridgeConfig bridgeConfig);
    void stop();
    bool isRunning() const noexcept { return thread_.joinable(); }

    // Output side. Only valid while running.
    ClockBridge& bridge() noexcept { return *bridge_; }

    CaptureStats stats() const noexcept;
    // Records of the first packets after start (up to kPacketLogSize). Read only
    // after stop().
    static constexpr int kPacketLogSize = 8192;
    std::vector<PacketRecord> packetLog() const;
    std::string lastError() const;

private:
    void run(CaptureDevice device, ClockBridgeConfig bridgeConfig, std::promise<void> started);

    std::unique_ptr<ClockBridge> bridge_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<double> sampleRate_{0.0};
    std::atomic<int> deviceChannels_{0};
    std::atomic<std::uint64_t> discontinuities_{0};
    std::atomic<std::uint64_t> silenceFills_{0};
    std::vector<PacketRecord> packetLog_ = std::vector<PacketRecord>(kPacketLogSize);
    std::atomic<int> packetCount_{0};
    void logPacket(double firstFrameTime, int frames, PacketRecord::Kind kind) noexcept;
    mutable std::mutex errorMutex_;
    std::string lastError_;
    void setError(std::string message);
};

} // namespace brir::io

// Captures from a WASAPI endpoint for a while and reports how the clock bridge
// behaves against the system clock (QPC), which stands in for an output device.
//
//   capture_probe            list devices
//   capture_probe <index> [seconds] [trace-prefix]
//
// With a trace prefix, writes <prefix>_reads.csv (every output read) and
// <prefix>_packets.csv (every capture packet) for offline analysis.

#include "brir/io/capture_stream.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<brir::io::CaptureDevice> devices = brir::io::listCaptureDevices();
    if (argc < 2) {
        for (std::size_t i = 0; i < devices.size(); ++i)
            std::printf("%2zu  %s %s\n", i, devices[i].loopback ? "[loopback]" : "[input]   ", devices[i].name.c_str());
        return 0;
    }

    std::size_t index = static_cast<std::size_t>(std::atoi(argv[1]));
    int seconds = argc > 2 ? std::atoi(argv[2]) : 30;
    const char* tracePrefix = argc > 3 ? argv[3] : nullptr;
    struct ReadRecord {
        double time;
        brir::ClockBridgeStats stats;
    };
    std::vector<ReadRecord> reads;
    if (tracePrefix) reads.reserve(static_cast<std::size_t>(seconds) * 400);
    if (index >= devices.size()) {
        std::fprintf(stderr, "No such device.\n");
        return 1;
    }

    constexpr double outputRate = 48000.0;
    constexpr int block = 128;
    brir::ClockBridgeConfig config;
    config.outputRate = outputRate;

    brir::io::CaptureStream stream;
    try {
        stream.start(devices[index], config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to start: %s\n", e.what());
        return 1;
    }

    brir::io::CaptureStats info = stream.stats();
    std::printf("Capturing '%s' at %.0f Hz, %d ch\n", devices[index].name.c_str(), info.sampleRate, info.deviceChannels);

    std::vector<float> left(block), right(block);
    float* out[2] = {left.data(), right.data()};
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(static_cast<long long>(1e9 * block / outputRate));
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    auto start = Clock::now();
    auto next = start;
    auto lastReport = start;
    double peak = 0.0;

    while (Clock::now() - start < std::chrono::seconds(seconds)) {
        // Pace like an output device: a high-resolution timer gets close, then
        // spin. Plain sleeps on Windows can oversleep by ~15 ms and would make
        // the reader, not the capture, the source of underruns.
        next += period;
        auto wait = next - Clock::now() - std::chrono::milliseconds(1);
        if (wait > std::chrono::milliseconds(0)) {
            LARGE_INTEGER due;
            due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        while (Clock::now() < next) {
        }
        double readTime = brir::ClockBridge::now();
        stream.bridge().read(out, block, readTime);
        if (tracePrefix) reads.push_back({readTime, stream.bridge().stats()});
        for (int i = 0; i < block; ++i) peak = std::max(peak, static_cast<double>(std::abs(left[static_cast<std::size_t>(i)])));

        if (Clock::now() - lastReport >= std::chrono::seconds(1)) {
            lastReport = Clock::now();
            brir::ClockBridgeStats s = stream.bridge().stats();
            brir::io::CaptureStats c = stream.stats();
            std::printf("fill %5.1f / %4.1f ms  drift %+7.1f ppm  underruns %llu  resyncs %llu  silence fills %llu  peak %.3f\n",
                        s.fillMs, s.targetMs, s.driftPpm, static_cast<unsigned long long>(s.underruns),
                        static_cast<unsigned long long>(s.resyncs), static_cast<unsigned long long>(c.silenceFills), peak);
            peak = 0.0;
        }
    }
    stream.stop();
    CloseHandle(timer);

    if (tracePrefix) {
        std::string base(tracePrefix);
        if (FILE* f = std::fopen((base + "_reads.csv").c_str(), "w")) {
            std::fprintf(f, "time,buffered_ms,fill_ms,target_ms,gap_ms,margin_ms,drift_ppm,underruns,running\n");
            for (const ReadRecord& r : reads)
                std::fprintf(f, "%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%llu,%d\n", r.time, r.stats.bufferedMs, r.stats.fillMs,
                             r.stats.targetMs, r.stats.gapMs, r.stats.marginMs, r.stats.driftPpm,
                             static_cast<unsigned long long>(r.stats.underruns), r.stats.running ? 1 : 0);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((base + "_packets.csv").c_str(), "w")) {
            std::fprintf(f, "wall,first_frame,frames,kind\n");
            for (const brir::io::PacketRecord& p : stream.packetLog())
                std::fprintf(f, "%.6f,%.6f,%d,%d\n", p.wallTime, p.firstFrameTime, p.frames, static_cast<int>(p.kind));
            std::fclose(f);
        }
    }
    std::string error = stream.lastError();
    if (!error.empty()) std::printf("Error: %s\n", error.c_str());
    return 0;
}

// Runs the app's real-time path (capture -> clock bridge -> renderer) inside a
// real JUCE output device callback, with the output muted, and reports how the
// bridge behaves against that device's clock.
//
//   output_probe                                   list output devices per type
//   output_probe <capture index> <output name part> [type name part] [seconds] [trace.csv] [buffer size] [feed device part]
//
// With a feed device, a quiet tone is played to it (on for 5 s, off for 5 s), so
// a loopback capture of that device sees real packets and idle transitions.
//
// Environment: BRIR_PROBE_CINEMA=<cinema.json> runs the full renderer with that
// cinema set; BRIR_PROBE_MMCSS=1 puts the convolver's worker threads in MMCSS.

#include "../common/cinema_loader.h"
#include "brir/io/capture_stream.h"
#include "brir/renderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CallbackRecord {
    double time;
    int frames;
    brir::ClockBridgeStats stats;
};

class Probe : public juce::AudioIODeviceCallback {
public:
    explicit Probe(brir::ClockBridge& bridge) : bridge_(bridge) { records_.reserve(200000); }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        renderer_ = std::make_unique<brir::Renderer>(device->getCurrentSampleRate(), 4096);
        left_.assign(4096, 0.0f);
        right_.assign(4096, 0.0f);

        juce::String cinema = juce::SystemStats::getEnvironmentVariable("BRIR_PROBE_CINEMA", {});
        if (cinema.isNotEmpty()) {
            brir::IrMatrix irs;
            if (!brir::tools::loadCinema(juce::File(cinema), device->getCurrentSampleRate(), irs)) {
                std::fprintf(stderr, "Cannot load cinema set.\n");
                return;
            }
            brir::ConvolverConfig config;
            if (juce::SystemStats::getEnvironmentVariable("BRIR_PROBE_MMCSS", {}) == "1") {
                config.onWorkerStart = [] {
                    DWORD task = 0;
                    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
                };
            }
            bool mmcss = static_cast<bool>(config.onWorkerStart);
            renderer_->setConvolver(std::make_unique<brir::Convolver>(brir::Renderer::kFeeds, 2, irs, config));
            std::printf("Renderer: cinema set loaded, worker MMCSS %s\n", mmcss ? "on" : "off");
        }
    }

    std::uint64_t lateBlocks() const { return renderer_ ? renderer_->lateBlocks() : 0; }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        int n = std::min(numSamples, 4096);
        float* stereo[2] = {left_.data(), right_.data()};
        bridge_.read(stereo, n);
        renderer_->process(stereo, stereo, n);
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch]) std::fill_n(outputs[ch], numSamples, 0.0f); // muted on purpose
        if (records_.size() < records_.capacity())
            records_.push_back({brir::ClockBridge::now(), numSamples, bridge_.stats()});
    }

    const std::vector<CallbackRecord>& records() const { return records_; }

private:
    brir::ClockBridge& bridge_;
    std::unique_ptr<brir::Renderer> renderer_;
    std::vector<float> left_, right_;
    std::vector<CallbackRecord> records_;
};

// Plays a quiet 440 Hz tone, gated on and off every 5 seconds.
class ToneFeed : public juce::AudioIODeviceCallback {
public:
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override { rate_ = device->getCurrentSampleRate(); }
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int i = 0; i < numSamples; ++i, ++sample_) {
            bool on = static_cast<long long>(sample_ / (5.0 * rate_)) % 2 == 0;
            float v = on ? 0.05f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * sample_ / rate_)) : 0.0f;
            for (int ch = 0; ch < numOutputs; ++ch)
                if (outputs[ch]) outputs[ch][i] = v;
        }
    }

private:
    double rate_ = 48000.0;
    long long sample_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::AudioDeviceManager manager;
    manager.initialise(0, 2, nullptr, false);

    if (argc < 3) {
        for (juce::AudioIODeviceType* type : manager.getAvailableDeviceTypes()) {
            type->scanForDevices();
            std::printf("[%s]\n", type->getTypeName().toRawUTF8());
            for (const juce::String& name : type->getDeviceNames(false)) std::printf("    %s\n", name.toRawUTF8());
        }
        return 0;
    }

    std::size_t captureIndex = static_cast<std::size_t>(std::atoi(argv[1]));
    juce::String outputPart = juce::String::fromUTF8(argv[2]);
    juce::String typePart = argc > 3 ? juce::String::fromUTF8(argv[3]) : juce::String("Windows Audio");
    int seconds = argc > 4 ? std::atoi(argv[4]) : 30;
    const char* tracePath = argc > 5 && std::string(argv[5]) != "-" ? argv[5] : nullptr;
    int bufferSize = argc > 6 ? std::atoi(argv[6]) : 0;
    juce::String feedPart = argc > 7 ? juce::String::fromUTF8(argv[7]) : juce::String();

    juce::AudioDeviceManager feedManager;
    ToneFeed tone;
    if (feedPart.isNotEmpty()) {
        feedManager.initialise(0, 2, nullptr, false);
        juce::AudioDeviceManager::AudioDeviceSetup feedSetup = feedManager.getAudioDeviceSetup();
        for (const juce::String& name : feedManager.getCurrentDeviceTypeObject()->getDeviceNames(false))
            if (name.containsIgnoreCase(feedPart)) feedSetup.outputDeviceName = name;
        juce::String feedError = feedManager.setAudioDeviceSetup(feedSetup, true);
        if (feedError.isNotEmpty()) {
            std::fprintf(stderr, "Failed to open feed device: %s\n", feedError.toRawUTF8());
            return 1;
        }
        feedManager.addAudioCallback(&tone);
        std::printf("Feeding tone to %s\n", feedManager.getCurrentAudioDevice()->getName().toRawUTF8());
    }

    // Pick the device type whose name matches best (exact match first).
    juce::AudioIODeviceType* chosenType = nullptr;
    for (juce::AudioIODeviceType* type : manager.getAvailableDeviceTypes())
        if (type->getTypeName() == typePart) chosenType = type;
    for (juce::AudioIODeviceType* type : manager.getAvailableDeviceTypes())
        if (!chosenType && type->getTypeName().containsIgnoreCase(typePart)) chosenType = type;
    if (!chosenType) {
        std::fprintf(stderr, "No device type matching '%s'\n", typePart.toRawUTF8());
        return 1;
    }
    manager.setCurrentAudioDeviceType(chosenType->getTypeName(), true);
    chosenType->scanForDevices();
    juce::String outputName;
    for (const juce::String& name : chosenType->getDeviceNames(false))
        if (name.containsIgnoreCase(outputPart)) outputName = name;
    if (outputName.isEmpty()) {
        std::fprintf(stderr, "No output device matching '%s'\n", outputPart.toRawUTF8());
        return 1;
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup = manager.getAudioDeviceSetup();
    setup.outputDeviceName = outputName;
    setup.inputDeviceName = {};
    if (bufferSize > 0) setup.bufferSize = bufferSize;
    juce::String error = manager.setAudioDeviceSetup(setup, true);
    juce::AudioIODevice* device = manager.getCurrentAudioDevice();
    if (error.isNotEmpty() || !device) {
        std::fprintf(stderr, "Failed to open output: %s\n", error.toRawUTF8());
        return 1;
    }
    manager.closeAudioDevice(); // reopened once the bridge exists

    std::vector<brir::io::CaptureDevice> inputs = brir::io::listCaptureDevices();
    if (captureIndex >= inputs.size()) {
        std::fprintf(stderr, "No such capture device.\n");
        return 1;
    }

    brir::io::CaptureStream capture;
    brir::ClockBridgeConfig config;
    manager.restartLastAudioDevice();
    device = manager.getCurrentAudioDevice();
    config.outputRate = device->getCurrentSampleRate();
    capture.start(inputs[captureIndex], config);

    Probe probe(capture.bridge());
    manager.addAudioCallback(&probe);
    std::printf("Output: [%s] %s, %.0f Hz, %d samples/block, reported output latency %d samples\n",
                chosenType->getTypeName().toRawUTF8(), outputName.toRawUTF8(), device->getCurrentSampleRate(),
                device->getCurrentBufferSizeSamples(), device->getOutputLatencyInSamples());
    std::printf("Capture: %s (%.0f Hz, %d ch)\n", inputs[captureIndex].name.c_str(), capture.stats().sampleRate,
                capture.stats().deviceChannels);

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    manager.removeAudioCallback(&probe);
    feedManager.removeAudioCallback(&tone);
    manager.closeAudioDevice();
    capture.stop();

    const auto& records = probe.records();
    if (records.size() < 2) {
        std::fprintf(stderr, "No callbacks recorded.\n");
        return 1;
    }
    std::vector<double> intervals;
    for (std::size_t i = 1; i < records.size(); ++i) intervals.push_back((records[i].time - records[i - 1].time) * 1000.0);
    std::vector<double> sorted = intervals;
    std::sort(sorted.begin(), sorted.end());
    const brir::ClockBridgeStats& last = records.back().stats;
    double minBuffered = 1e9;
    for (const auto& r : records)
        if (r.stats.running && r.time - records.front().time > 1.0) minBuffered = std::min(minBuffered, r.stats.bufferedMs);

    std::printf("Callbacks: %zu, interval median %.2f ms, p99.9 %.2f ms, max %.2f ms\n", records.size(),
                sorted[sorted.size() / 2], sorted[sorted.size() * 999 / 1000], sorted.back());
    std::printf("Bridge: target %.2f ms, fill %.2f ms, drift %+.1f ppm, min buffered (after 1 s) %.2f ms\n", last.targetMs,
                last.fillMs, last.driftPpm, minBuffered);
    std::printf("Convolver late blocks %llu\n", static_cast<unsigned long long>(probe.lateBlocks()));
    std::printf("Underruns %llu, resyncs %llu, overflows %llu\n", static_cast<unsigned long long>(last.underruns),
                static_cast<unsigned long long>(last.resyncs), static_cast<unsigned long long>(last.overflows));

    if (tracePath) {
        if (FILE* f = std::fopen(tracePath, "w")) {
            std::fprintf(f, "time,frames,buffered_ms,fill_ms,target_ms,drift_ppm,underruns,running\n");
            for (const auto& r : records)
                std::fprintf(f, "%.6f,%d,%.3f,%.3f,%.3f,%.1f,%llu,%d\n", r.time, r.frames, r.stats.bufferedMs, r.stats.fillMs,
                             r.stats.targetMs, r.stats.driftPpm, static_cast<unsigned long long>(r.stats.underruns),
                             r.stats.running ? 1 : 0);
            std::fclose(f);
        }
    }
    return 0;
}

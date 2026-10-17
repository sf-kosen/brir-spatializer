#include "brir/io/capture_stream.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace brir::io {

namespace {

// PKEY_Device_FriendlyName, defined locally to avoid INITGUID clashes.
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// Shared-mode buffer; generous because we poll rather than rely on events.
constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200 ms in 100 ns units
constexpr LONG kPollPeriodMs = 2;
// While a loopback source is idle WASAPI delivers nothing; after this long we
// feed silence so the output side keeps a steady stream.
// Some virtual devices deliver in irregular bursts with gaps of tens of ms
// while playing; treating those as "idle" inserted silence mid-programme.
constexpr double kSilenceFillAfterSeconds = 0.200;
constexpr double kSilenceFillMarginSeconds = 0.005;

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** operator&() noexcept { return &p; }
    T* operator->() const noexcept { return p; }
};

struct CoTaskMem {
    void* p = nullptr;
    ~CoTaskMem() { CoTaskMemFree(p); }
};

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

void check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buffer);
    }
}

std::string toUtf8(const wchar_t* text)
{
    if (!text) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size - 1, 0)), '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string friendlyName(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return "Unknown device";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name = "Unknown device";
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR) name = toUtf8(value.pwszVal);
    PropVariantClear(&value);
    return name;
}

double secondsFromHns(UINT64 hns)
{
    return static_cast<double>(hns) * 1e-7;
}

enum class SampleType { Float32, Int16, Int24, Int32 };

// Converts device frames to interleaved stereo floats using a per-channel
// left/right gain matrix derived from the channel mask.
class Downmixer {
public:
    explicit Downmixer(const WAVEFORMATEX& format)
        : channels_(format.nChannels), bytesPerSample_(format.wBitsPerSample / 8)
    {
        WORD tag = format.wFormatTag;
        DWORD mask = 0;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
            tag = IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? WAVE_FORMAT_IEEE_FLOAT
                  : IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)      ? WAVE_FORMAT_PCM
                                                                              : 0;
            mask = ext.dwChannelMask; // 24-in-32 PCM is read as 32-bit below
        }

        if (tag == WAVE_FORMAT_IEEE_FLOAT && bytesPerSample_ == 4) type_ = SampleType::Float32;
        else if (tag == WAVE_FORMAT_PCM && bytesPerSample_ == 2) type_ = SampleType::Int16;
        else if (tag == WAVE_FORMAT_PCM && bytesPerSample_ == 3) type_ = SampleType::Int24;
        else if (tag == WAVE_FORMAT_PCM && bytesPerSample_ == 4) type_ = SampleType::Int32;
        else throw std::runtime_error("Unsupported capture sample format.");

        buildMatrix(mask);
    }

    void convert(const BYTE* data, UINT32 frames, float* stereo) const noexcept
    {
        for (UINT32 f = 0; f < frames; ++f) {
            float left = 0.0f, right = 0.0f;
            for (int ch = 0; ch < channels_; ++ch) {
                float v = sample(data, static_cast<std::size_t>(f) * channels_ + ch);
                left += v * gains_[static_cast<std::size_t>(ch)][0];
                right += v * gains_[static_cast<std::size_t>(ch)][1];
            }
            stereo[2 * f] = left;
            stereo[2 * f + 1] = right;
        }
    }

private:
    float sample(const BYTE* data, std::size_t index) const noexcept
    {
        const BYTE* p = data + index * bytesPerSample_;
        switch (type_) {
        case SampleType::Float32: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        case SampleType::Int16: {
            std::int16_t v;
            std::memcpy(&v, p, 2);
            return v / 32768.0f;
        }
        case SampleType::Int24: {
            std::int32_t v = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
            return static_cast<float>(v) / 2147483648.0f;
        }
        case SampleType::Int32: {
            std::int32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<float>(v) / 2147483648.0f;
        }
        }
        return 0.0f;
    }

    void buildMatrix(DWORD mask)
    {
        constexpr float kSide = 0.70710678f;
        gains_.assign(static_cast<std::size_t>(channels_), {0.0f, 0.0f});
        if (channels_ == 1) {
            gains_[0] = {1.0f, 1.0f};
            return;
        }
        if (mask == 0) {
            gains_[0] = {1.0f, 0.0f};
            gains_[1] = {0.0f, 1.0f};
            return;
        }

        // Channels appear in ascending speaker-bit order.
        int ch = 0;
        for (DWORD bit = 1; bit != 0 && ch < channels_; bit <<= 1) {
            if ((mask & bit) == 0) continue;
            std::array<float, 2>& g = gains_[static_cast<std::size_t>(ch++)];
            switch (bit) {
            case SPEAKER_FRONT_LEFT: g = {1.0f, 0.0f}; break;
            case SPEAKER_FRONT_RIGHT: g = {0.0f, 1.0f}; break;
            case SPEAKER_FRONT_CENTER: g = {kSide, kSide}; break;
            case SPEAKER_LOW_FREQUENCY: g = {0.0f, 0.0f}; break;
            case SPEAKER_BACK_LEFT:
            case SPEAKER_SIDE_LEFT:
            case SPEAKER_FRONT_LEFT_OF_CENTER:
            case SPEAKER_TOP_FRONT_LEFT:
            case SPEAKER_TOP_BACK_LEFT: g = {kSide, 0.0f}; break;
            case SPEAKER_BACK_RIGHT:
            case SPEAKER_SIDE_RIGHT:
            case SPEAKER_FRONT_RIGHT_OF_CENTER:
            case SPEAKER_TOP_FRONT_RIGHT:
            case SPEAKER_TOP_BACK_RIGHT: g = {0.0f, kSide}; break;
            default: g = {0.5f, 0.5f}; break;
            }
        }
    }

    int channels_;
    int bytesPerSample_;
    SampleType type_ = SampleType::Float32;
    std::vector<std::array<float, 2>> gains_;
};

class HighResTimer {
public:
    HighResTimer()
    {
        handle_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!handle_) handle_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!handle_) throw std::runtime_error("CreateWaitableTimer failed");
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(kPollPeriodMs) * 10'000;
        SetWaitableTimer(handle_, &due, kPollPeriodMs, nullptr, nullptr, FALSE);
    }
    ~HighResTimer() { CloseHandle(handle_); }
    void wait() const noexcept { WaitForSingleObject(handle_, 50); }

private:
    HANDLE handle_ = nullptr;
};

} // namespace

std::vector<CaptureDevice> listCaptureDevices()
{
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                           reinterpret_cast<void**>(&enumerator)),
          "Creating device enumerator");

    std::vector<CaptureDevice> devices;
    for (EDataFlow flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> collection;
        check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection), "Enumerating endpoints");
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, &device))) continue;
            CoTaskMem id;
            if (FAILED(device->GetId(reinterpret_cast<LPWSTR*>(&id.p)))) continue;
            devices.push_back({static_cast<const wchar_t*>(id.p), friendlyName(device.p), flow == eRender});
        }
    }
    return devices;
}

CaptureStream::~CaptureStream()
{
    stop();
}

void CaptureStream::start(const CaptureDevice& device, ClockBridgeConfig bridgeConfig)
{
    stop();
    stopRequested_ = false;
    discontinuities_ = 0;
    silenceFills_ = 0;
    packetCount_ = 0;
    setError({});

    std::promise<void> started;
    std::future<void> ready = started.get_future();
    thread_ = std::thread(&CaptureStream::run, this, device, bridgeConfig, std::move(started));
    try {
        ready.get();
    } catch (...) {
        thread_.join();
        bridge_.reset();
        throw;
    }
}

void CaptureStream::stop()
{
    if (!thread_.joinable()) return;
    stopRequested_ = true;
    thread_.join();
    bridge_.reset();
}

CaptureStats CaptureStream::stats() const noexcept
{
    return {sampleRate_.load(), deviceChannels_.load(), discontinuities_.load(), silenceFills_.load()};
}

void CaptureStream::logPacket(double firstFrameTime, int frames, PacketRecord::Kind kind) noexcept
{
    int index = packetCount_.load(std::memory_order_relaxed);
    if (index >= kPacketLogSize) return;
    packetLog_[static_cast<std::size_t>(index)] = {ClockBridge::now(), firstFrameTime, frames, kind};
    packetCount_.store(index + 1, std::memory_order_release);
}

std::vector<PacketRecord> CaptureStream::packetLog() const
{
    int count = packetCount_.load(std::memory_order_acquire);
    return {packetLog_.begin(), packetLog_.begin() + count};
}

std::string CaptureStream::lastError() const
{
    std::lock_guard lock(errorMutex_);
    return lastError_;
}

void CaptureStream::setError(std::string message)
{
    std::lock_guard lock(errorMutex_);
    lastError_ = std::move(message);
}

void CaptureStream::run(CaptureDevice device, ClockBridgeConfig bridgeConfig, std::promise<void> started)
{
    ComScope com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    bool running = false;
    try {
        check(com.hr, "CoInitializeEx");
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                               reinterpret_cast<void**>(&enumerator)),
              "Creating device enumerator");
        ComPtr<IMMDevice> endpoint;
        check(enumerator->GetDevice(device.id.c_str(), &endpoint), "Opening device");
        ComPtr<IAudioClient> client;
        check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)),
              "Activating audio client");

        CoTaskMem formatMemory;
        check(client->GetMixFormat(reinterpret_cast<WAVEFORMATEX**>(&formatMemory.p)), "GetMixFormat");
        const WAVEFORMATEX& format = *static_cast<WAVEFORMATEX*>(formatMemory.p);
        Downmixer downmixer(format);

        DWORD flags = device.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &format, nullptr),
              "Initializing capture stream");
        ComPtr<IAudioCaptureClient> capture;
        check(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture)),
              "Getting capture client");

        const double rate = format.nSamplesPerSec;
        sampleRate_ = rate;
        deviceChannels_ = format.nChannels;
        bridgeConfig.channels = 2;
        bridgeConfig.inputRate = rate;
        bridge_ = std::make_unique<ClockBridge>(bridgeConfig);

        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);
        std::vector<float> stereo(static_cast<std::size_t>(bufferFrames) * 2);
        HighResTimer timer;

        check(client->Start(), "Starting capture");
        running = true;
        started.set_value();

        double streamEnd = ClockBridge::now(); // capture time just past the last delivered frame
        bool idle = false;
        double timestampOffset = 0.0;
        bool haveOffset = false;
        double lastDelivery = 0.0;
        while (!stopRequested_.load(std::memory_order_relaxed)) {
            timer.wait();
            UINT32 packet = 0;
            bool delivered = false;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD bufferFlags = 0;
                UINT64 qpcPosition = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &bufferFlags, nullptr, &qpcPosition))) break;

                double arrival = ClockBridge::now();
                double firstFrameTime = (bufferFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 || qpcPosition == 0
                                            ? arrival - frames / rate
                                            : secondsFromHns(qpcPosition);
                if (bufferFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) discontinuities_.fetch_add(1);

                // Device timestamps carry a device-specific constant offset: for
                // loopback they mark when the audio will be *played*, tens of ms
                // after it reaches us. Keep their spacing but shift them so the
                // earliest-arriving packet ever seen arrives exactly at its end.
                // (No slow release: a drifting offset would read as clock drift.)
                double delay = arrival - (firstFrameTime + frames / rate);
                if (!haveOffset || delay < timestampOffset) {
                    timestampOffset = delay;
                    haveOffset = true;
                }
                firstFrameTime += timestampOffset;

                // After idle filling, the first real packet can start before the
                // silence we already wrote ends; drop that overlap.
                UINT32 skip = 0;
                if (idle && firstFrameTime < streamEnd)
                    skip = std::min(frames, static_cast<UINT32>((streamEnd - firstFrameTime) * rate));
                idle = false;
                if (skip > 0) logPacket(firstFrameTime, static_cast<int>(skip), PacketRecord::Overlap);

                if (skip < frames) {
                    int count = static_cast<int>(frames - skip);
                    double start = firstFrameTime + skip / rate;
                    if (frames > bufferFrames) stereo.resize(static_cast<std::size_t>(frames) * 2);
                    logPacket(start, count, (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) ? PacketRecord::Silent : PacketRecord::Data);
                    if (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        bridge_->writeSilence(count, start);
                    } else {
                        downmixer.convert(data + static_cast<std::size_t>(skip) * format.nBlockAlign, frames - skip,
                                          stereo.data());
                        bridge_->write(stereo.data(), count, start);
                    }
                    streamEnd = firstFrameTime + frames / rate;
                }
                capture->ReleaseBuffer(frames);
                delivered = true;
                if (lastDelivery > 0.0) {
                    double gapMs = (arrival - lastDelivery) * 1000.0;
                    double seen = maxDeliveryGapMs_.load(std::memory_order_relaxed);
                    while (gapMs > seen && !maxDeliveryGapMs_.compare_exchange_weak(seen, gapMs)) {
                    }
                }
                lastDelivery = arrival;
            }

            // A loopback source delivers nothing while nothing plays. Once nothing
            // has arrived for far longer than any delivery gap seen during
            // playback, keep the stream going with silence in small steps,
            // lagging real time by a small margin.
            double now = ClockBridge::now();
            if (!delivered && device.loopback && (idle || now - streamEnd > kSilenceFillAfterSeconds)) {
                if (!idle) silenceFills_.fetch_add(1);
                idle = true;
                lastDelivery = 0.0; // the gap into idle isn't a delivery gap
                int frames = static_cast<int>((now - kSilenceFillMarginSeconds - streamEnd) * rate);
                if (frames > 0) {
                    logPacket(streamEnd, frames, PacketRecord::SilenceFill);
                    bridge_->writeSilence(frames, streamEnd);
                    streamEnd += frames / rate;
                }
            }
        }
        client->Stop();
    } catch (const std::exception& e) {
        setError(e.what());
        if (!running) started.set_exception(std::current_exception());
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace brir::io

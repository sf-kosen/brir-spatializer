using System.Buffers;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using BrirSpatializer.Audio.Buffer;
using BrirSpatializer.Audio.Dsp;
using BrirSpatializer.Audio.Ir;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace BrirSpatializer.Audio;

public sealed class AudioEngine : IDisposable
{
    private const int Channels = 2;
    private const int BufferMarginMs = 250;
    private static readonly Guid PcmSubType = new("00000001-0000-0010-8000-00AA00389B71");
    private static readonly Guid FloatSubType = new("00000003-0000-0010-8000-00AA00389B71");

    private readonly DeviceManager _deviceManager = new();
    private readonly AudioSettings _settings;
    private readonly DspGraph _dspGraph = new();

    private WasapiCapture? _capture;
    private WasapiOut? _output;
    private RingBuffer? _ringBuffer;
    private EngineWaveProvider? _provider;
    private InputSampleFormat _inputFormat;
    private int _bytesPerFrame;
    private OutputSampleFormat _outputFormat;
    private int _outputBytesPerFrame;

    private double _integral;
    private float _asrcRatio = 1.0f;
    private long _fillLevelMsBits;
    private int _slipCount;
    private BrirSet? _slotASet;
    private BrirSet? _slotBSet;
    private readonly BrirInterpolator _brirInterpolator = new();

    public AudioEngine(AudioSettings settings)
    {
        _settings = settings;
        _dspGraph.SetBlockSize(_settings.BlockSize);
        _dspGraph.SpectrumEnabled = _settings.EnableSpectrumAnalysis;
        _dspGraph.LimiterEnabled = _settings.EnableLimiter;
    }

    public bool IsRunning { get; private set; }

    public MeterSnapshot MeterSnapshot => _dspGraph.GetMeterSnapshot();

    public SpectrumSnapshot SpectrumSnapshot => _dspGraph.GetSpectrumSnapshot();

    public EngineStatus Status => new(
        BitConverter.Int64BitsToDouble(Interlocked.Read(ref _fillLevelMsBits)),
        _asrcRatio,
        Interlocked.CompareExchange(ref _slipCount, 0, 0),
        IsRunning,
        _dspGraph.HasSpatializerOutput(),
        _dspGraph.GetSpatializerPeaks().InputPeak,
        _dspGraph.GetSpatializerPeaks().OutputPeak);

    public IReadOnlyList<AudioDeviceInfo> GetInputDevices() => _deviceManager.GetInputDevices();

    public string? LastError { get; private set; }

    private void ReportError(Exception ex)
    {
        LastError = ex.GetType().Name + ": " + ex.Message;
    }

    public IReadOnlyList<AudioDeviceInfo> GetOutputDevices() => _deviceManager.GetOutputDevices();

    public void UpdateControls(ControlSettings controls) => _dspGraph.UpdateControls(controls);

    public int? TryGetDeviceSampleRate(string deviceId)
    {
        MMDevice? device = _deviceManager.TryGetDevice(deviceId);
        return device?.AudioClient.MixFormat.SampleRate;
    }

    public void SetSpatializerMode(ActiveSlot slot, Dsp.Processors.SpatializerMode mode)
    {
        _dspGraph.SetSpatializerMode(slot, mode);
    }

    public void SetBrir(ActiveSlot slot, Ir.BrirIr ir, int irSwitchXfadeMs)
    {
        _dspGraph.SetBrir(slot, ir, irSwitchXfadeMs);
    }

    public void SetBrirSet(ActiveSlot slot, BrirSet set)
    {
        if (slot == ActiveSlot.A) _slotASet = set;
        else _slotBSet = set;
    }

    public void SetSeat(ActiveSlot slot, float x, float y, NormalizeMode normalizeMode, float targetPeakDbFs, float targetRmsDbFs, int irSwitchXfadeMs)
    {
        BrirSet? set = slot == ActiveSlot.A ? _slotASet : _slotBSet;
        if (set is null) throw new InvalidOperationException("BRIR set not loaded.");
        BrirIr interpolated = _brirInterpolator.Interpolate(set, x, y);
        IrNormalizeResult norm = IrNormalizer.Normalize(interpolated, normalizeMode, targetPeakDbFs, targetRmsDbFs);
        _dspGraph.SetBrir(slot, norm.Ir, irSwitchXfadeMs);
        _dspGraph.SetSpatializerMode(slot, Dsp.Processors.SpatializerMode.Brir);
    }

    public void Start(string inputDeviceId, string outputDeviceId, bool exclusiveOutput = false)
    {
        if (IsRunning) return;

        MMDevice? inputDevice = _deviceManager.TryGetDevice(inputDeviceId);
        MMDevice? outputDevice = _deviceManager.TryGetDevice(outputDeviceId);
        if (inputDevice is null || outputDevice is null)
        {
            throw new InvalidOperationException("Audio device not found.");
        }

        int inputRate = inputDevice.AudioClient.MixFormat.SampleRate;
        int outputRate = outputDevice.AudioClient.MixFormat.SampleRate;
        if (inputRate != outputRate)
        {
            throw new InvalidOperationException("Input/output sample rate mismatch.");
        }

        if (inputDevice.AudioClient.MixFormat.Channels != Channels ||
            outputDevice.AudioClient.MixFormat.Channels != Channels)
        {
            throw new InvalidOperationException("Only stereo devices are supported.");
        }

        _inputFormat = GetInputSampleFormat(inputDevice.AudioClient.MixFormat);
        _bytesPerFrame = inputDevice.AudioClient.MixFormat.BlockAlign;
        if (_inputFormat == InputSampleFormat.Unsupported)
        {
            throw new InvalidOperationException("Input device format not supported.");
        }

        _outputFormat = GetOutputSampleFormat(outputDevice.AudioClient.MixFormat);
        _outputBytesPerFrame = outputDevice.AudioClient.MixFormat.BlockAlign;
        if (_outputFormat == OutputSampleFormat.Unsupported)
        {
            throw new InvalidOperationException("Output device format not supported.");
        }

        int dryDelaySamples = _dspGraph.GetDryAlignmentLatencySamples();
        _dspGraph.SetBlockSize(_settings.BlockSize);
        _dspGraph.SpectrumEnabled = _settings.EnableSpectrumAnalysis;
        _dspGraph.LimiterEnabled = _settings.EnableLimiter;
        _dspGraph.SetDryDelaySamples(dryDelaySamples);
        _dspGraph.ApplyPendingDryDelaySamples();

        int capacityFrames = (int)Math.Ceiling(((_settings.DriftTargetBufferMs + BufferMarginMs) / 1000.0) * outputRate);
        _ringBuffer = new RingBuffer(capacityFrames, Channels);
        _provider = new EngineWaveProvider(
            _ringBuffer,
            _settings,
            _dspGraph,
            outputRate,
            outputDevice.AudioClient.MixFormat,
            _outputFormat,
            _outputBytesPerFrame,
            UpdateDriftRatio,
            UpdateFillLevel,
            () => _asrcRatio,
            IncrementSlipCount,
            ReportError);

        int latencyMs = _settings.DriftCompensationMode == DriftCompensationMode.Strict
            ? Math.Clamp((int)Math.Round((_settings.BlockSize * 0.35) / outputRate * 1000.0), 12, 20)
            : Math.Max(40, (int)Math.Round((_settings.BlockSize * 2.0) / outputRate * 1000.0));
        _capture = inputDevice.DataFlow == DataFlow.Render
            ? new WasapiLoopbackCapture(inputDevice)
            : new WasapiCapture(inputDevice, true, latencyMs);
        _capture.DataAvailable += OnCaptureDataAvailable;
        _capture.RecordingStopped += OnCaptureStopped;

        AudioClientShareMode shareMode = exclusiveOutput
            ? AudioClientShareMode.Exclusive
            : AudioClientShareMode.Shared;
        _output = new WasapiOut(outputDevice, shareMode, true, latencyMs);
        _output.Init(_provider);

        _capture.StartRecording();
        _output.Play();
        IsRunning = true;
    }

    public void Stop()
    {
        if (!IsRunning) return;

        _capture?.StopRecording();
        _output?.Stop();
        _capture?.Dispose();
        _output?.Dispose();
        _provider?.Dispose();
        _capture = null;
        _output = null;
        _ringBuffer = null;
        _provider = null;
        IsRunning = false;
    }

    private void OnCaptureStopped(object? sender, StoppedEventArgs e)
    {
        _ = e;
    }

    private void OnCaptureDataAvailable(object? sender, WaveInEventArgs e)
    {
        try
        {
            if (_ringBuffer is null) return;
            int frames = e.BytesRecorded / _bytesPerFrame;
            if (frames <= 0) return;

            float[] buffer = ArrayPool<float>.Shared.Rent(frames * Channels);
            try
            {
                ConvertToFloat(e.Buffer, e.BytesRecorded, buffer);
                _ringBuffer.Write(buffer, frames);
                _provider?.NotifyInputAvailable();
            }
            finally
            {
                ArrayPool<float>.Shared.Return(buffer);
            }
        }
        catch (Exception ex)
        {
            ReportError(ex);
        }
    }

    private InputSampleFormat GetInputSampleFormat(WaveFormat format)
    {
        if (format is WaveFormatExtensible extensible)
        {
            return GetInputSampleFormatFromExtensible(extensible);
        }

        if (format.Encoding == WaveFormatEncoding.IeeeFloat && format.BitsPerSample == 32)
        {
            return InputSampleFormat.Float32;
        }

        if (format.Encoding == WaveFormatEncoding.Pcm)
        {
            return format.BitsPerSample switch
            {
                16 => InputSampleFormat.Pcm16,
                24 => InputSampleFormat.Pcm24,
                32 => InputSampleFormat.Pcm32,
                _ => InputSampleFormat.Unsupported
            };
        }

        return InputSampleFormat.Unsupported;
    }

    private InputSampleFormat GetInputSampleFormatFromExtensible(WaveFormatExtensible format)
    {
        if (format.SubFormat == FloatSubType && format.BitsPerSample == 32)
        {
            return InputSampleFormat.Float32;
        }

        if (format.SubFormat == PcmSubType)
        {
            return format.BitsPerSample switch
            {
                16 => InputSampleFormat.Pcm16,
                24 => InputSampleFormat.Pcm24,
                32 => InputSampleFormat.Pcm32,
                _ => InputSampleFormat.Unsupported
            };
        }

        return InputSampleFormat.Unsupported;
    }

    private void ConvertToFloat(byte[] source, int bytesRecorded, float[] destination)
    {
        switch (_inputFormat)
        {
            case InputSampleFormat.Float32:
                System.Buffer.BlockCopy(source, 0, destination, 0, bytesRecorded);
                return;
            case InputSampleFormat.Pcm16:
                ConvertPcm16(source, bytesRecorded, destination);
                return;
            case InputSampleFormat.Pcm24:
                ConvertPcm24(source, bytesRecorded, destination);
                return;
            case InputSampleFormat.Pcm32:
                ConvertPcm32(source, bytesRecorded, destination);
                return;
            default:
                Array.Clear(destination, 0, destination.Length);
                return;
        }
    }

    private static void ConvertPcm16(byte[] source, int bytesRecorded, float[] destination)
    {
        ReadOnlySpan<byte> span = new ReadOnlySpan<byte>(source, 0, bytesRecorded);
        ReadOnlySpan<short> samples = MemoryMarshal.Cast<byte, short>(span);
        int count = Math.Min(samples.Length, destination.Length);
        for (int i = 0; i < count; i++)
        {
            destination[i] = samples[i] / 32768f;
        }
    }

    private static void ConvertPcm24(byte[] source, int bytesRecorded, float[] destination)
    {
        int sampleCount = Math.Min(destination.Length, bytesRecorded / 3);
        int src = 0;
        for (int i = 0; i < sampleCount; i++)
        {
            int sample = source[src] | (source[src + 1] << 8) | (source[src + 2] << 16);
            if ((sample & 0x800000) != 0) sample |= unchecked((int)0xFF000000);
            destination[i] = sample / 8388608f;
            src += 3;
        }
    }

    private static void ConvertPcm32(byte[] source, int bytesRecorded, float[] destination)
    {
        ReadOnlySpan<byte> span = new ReadOnlySpan<byte>(source, 0, bytesRecorded);
        ReadOnlySpan<int> samples = MemoryMarshal.Cast<byte, int>(span);
        int count = Math.Min(samples.Length, destination.Length);
        for (int i = 0; i < count; i++)
        {
            destination[i] = samples[i] / 2147483648f;
        }
    }

    private enum InputSampleFormat
    {
        Unsupported,
        Float32,
        Pcm16,
        Pcm24,
        Pcm32
    }

    private OutputSampleFormat GetOutputSampleFormat(WaveFormat format)
    {
        if (format is WaveFormatExtensible extensible)
        {
            return GetOutputSampleFormatFromExtensible(extensible);
        }

        if (format.Encoding == WaveFormatEncoding.IeeeFloat && format.BitsPerSample == 32)
        {
            return OutputSampleFormat.Float32;
        }

        if (format.Encoding == WaveFormatEncoding.Pcm)
        {
            return format.BitsPerSample switch
            {
                16 => OutputSampleFormat.Pcm16,
                24 => OutputSampleFormat.Pcm24,
                32 => OutputSampleFormat.Pcm32,
                _ => OutputSampleFormat.Unsupported
            };
        }

        return OutputSampleFormat.Unsupported;
    }

    private OutputSampleFormat GetOutputSampleFormatFromExtensible(WaveFormatExtensible format)
    {
        if (format.SubFormat == FloatSubType && format.BitsPerSample == 32)
        {
            return OutputSampleFormat.Float32;
        }

        if (format.SubFormat == PcmSubType)
        {
            return format.BitsPerSample switch
            {
                16 => OutputSampleFormat.Pcm16,
                24 => OutputSampleFormat.Pcm24,
                32 => OutputSampleFormat.Pcm32,
                _ => OutputSampleFormat.Unsupported
            };
        }

        return OutputSampleFormat.Unsupported;
    }

    private void UpdateDriftRatio(double bufferLevelMs, int sampleRate)
    {
        if (_settings.DriftCompensationMode != DriftCompensationMode.Asrc)
        {
            _asrcRatio = 1.0f;
            _integral = 0;
            return;
        }

        double error = bufferLevelMs - _settings.DriftTargetBufferMs;
        double dt = _settings.BlockSize / (double)sampleRate;
        _integral += error * dt;
        double r = 1.0 + _settings.DriftAsrc.Kp * error + _settings.DriftAsrc.Ki * _integral;
        r = Math.Clamp(r, _settings.DriftAsrc.RMin, _settings.DriftAsrc.RMax);
        _asrcRatio = (float)r;
    }

    private void UpdateFillLevel(double bufferLevelMs)
    {
        Interlocked.Exchange(ref _fillLevelMsBits, BitConverter.DoubleToInt64Bits(bufferLevelMs));
    }

    private void IncrementSlipCount()
    {
        Interlocked.Increment(ref _slipCount);
    }

    public void Dispose()
    {
        Stop();
    }

    private sealed class EngineWaveProvider : IWaveProvider, IDisposable
    {
        private readonly RingBuffer _ringBuffer;
        private readonly RingBuffer _processedBuffer;
        private readonly AudioSettings _settings;
        private readonly DspGraph _dspGraph;
        private readonly int _sampleRate;
        private readonly WaveFormat _outputFormat;
        private readonly OutputSampleFormat _outputSampleFormat;
        private readonly int _outputBytesPerFrame;
        private readonly Action<double, int> _driftUpdate;
        private readonly Action<double> _fillUpdate;
        private readonly Func<float> _asrcRatioAccessor;
        private readonly Action _slipCount;
        private readonly Action<Exception> _errorCallback;
        private readonly float _liveTrimLinear;
        private readonly int _dspBlockFrames;
        private readonly int _strictTargetFrames;
        private readonly AutoResetEvent _strictInputSignal = new(false);
        private readonly Thread? _strictWorker;
        private float[] _scratch;
        private float[] _processBuffer = Array.Empty<float>();
        private float[] _slipBuffer = Array.Empty<float>();
        private float[] _asrcBuffer = Array.Empty<float>();
        private float[] _dspBlockBuffer = Array.Empty<float>();
        private int _asrcBufferedFrames;
        private double _asrcPhase;
        private readonly Stopwatch _prerollTimer = Stopwatch.StartNew();
        private double _latestFillMs;
        private volatile bool _strictWorkerStop;

        private bool _prerollActive = true;
        private int _fadeInRemaining;
        private int _fadeInTotal;

        public EngineWaveProvider(
            RingBuffer ringBuffer,
            AudioSettings settings,
            DspGraph dspGraph,
            int sampleRate,
            WaveFormat outputFormat,
            OutputSampleFormat outputSampleFormat,
            int outputBytesPerFrame,
            Action<double, int> driftUpdate,
            Action<double> fillUpdate,
            Func<float> asrcRatioAccessor,
            Action slipCount,
            Action<Exception> errorCallback)
        {
            _ringBuffer = ringBuffer;
            _settings = settings;
            _dspGraph = dspGraph;
            _sampleRate = sampleRate;
            _outputFormat = outputFormat;
            _outputSampleFormat = outputSampleFormat;
            _outputBytesPerFrame = outputBytesPerFrame;
            _driftUpdate = driftUpdate;
            _fillUpdate = fillUpdate;
            _asrcRatioAccessor = asrcRatioAccessor;
            _slipCount = slipCount;
            _errorCallback = errorCallback;
            _liveTrimLinear = (float)Math.Pow(10.0, settings.LiveTrimDb / 20.0);
            _dspBlockFrames = Math.Max(1, settings.BlockSize);
            _strictTargetFrames = Math.Max(
                _dspBlockFrames * 8,
                (int)Math.Ceiling((settings.DriftTargetBufferMs / 1000.0) * sampleRate));
            _scratch = new float[settings.BlockSize * Channels];
            _processBuffer = new float[settings.BlockSize * Channels];
            _dspBlockBuffer = new float[_dspBlockFrames * Channels];
            int processedCapacityFrames = Math.Max(
                _strictTargetFrames + (_dspBlockFrames * 8),
                (int)Math.Ceiling(((settings.DriftTargetBufferMs + BufferMarginMs) / 1000.0) * sampleRate));
            _processedBuffer = new RingBuffer(processedCapacityFrames, Channels);
            WaveFormat = outputFormat;

            if (_settings.DriftCompensationMode == DriftCompensationMode.Strict)
            {
                _strictWorker = new Thread(StrictProcessingLoop)
                {
                    IsBackground = true,
                    Name = "BrirStrictDspWorker"
                };
                _strictWorker.Priority = ThreadPriority.Highest;
                _strictWorker.Start();
            }
        }

        public WaveFormat WaveFormat { get; }

        public void NotifyInputAvailable()
        {
            if (_settings.DriftCompensationMode == DriftCompensationMode.Strict)
            {
                _strictInputSignal.Set();
            }
        }

        public int Read(byte[] buffer, int offset, int count)
        {
            try
            {
                int framesRequested = count / _outputBytesPerFrame;
                if (framesRequested <= 0) return 0;

                EnsureScratch(framesRequested);
                Array.Clear(_scratch, 0, framesRequested * Channels);

                double fillMs = GetPlaybackFillLevelMs();
                _latestFillMs = fillMs;
                _fillUpdate(fillMs);
                _driftUpdate(fillMs, _sampleRate);

                bool shouldRead = true;
                bool forceMute = false;
                if (_prerollActive && _settings.Startup.MuteDuringPreroll)
                {
                    forceMute = true;
                    if (_settings.Startup.HoldReadPointerWhileMuted)
                    {
                        shouldRead = false;
                    }

                    if (ShouldStartPlayback(fillMs))
                    {
                        _prerollActive = false;
                        _fadeInTotal = (int)(_sampleRate * (_settings.Startup.FadeInMs / 1000.0));
                        _fadeInRemaining = _fadeInTotal;
                        shouldRead = true;
                        forceMute = false;
                    }
                }

                if (shouldRead)
                {
                    if (_settings.DriftCompensationMode == DriftCompensationMode.Strict)
                    {
                        ReadStrictOutput(framesRequested);
                    }
                    else
                    {
                        ReadFromBuffer(framesRequested);
                    }
                }

                if (forceMute)
                {
                    Array.Clear(_scratch, 0, framesRequested * Channels);
                }

                if (_fadeInRemaining > 0)
                {
                    ApplyFadeIn(framesRequested);
                }

                if (!forceMute && _settings.DriftCompensationMode != DriftCompensationMode.Strict)
                {
                    ApplyLiveTrim(_scratch, framesRequested);
                    ProcessDsp(framesRequested);
                }

                WriteOutput(buffer, offset, framesRequested);
                return framesRequested * _outputBytesPerFrame;
            }
            catch (Exception ex)
            {
                _errorCallback(ex);
                Array.Clear(buffer, offset, count); // Output silence
                return count;
            }
        }

        private void ReadFromBuffer(int framesRequested)
        {
            if (_settings.DriftCompensationMode == DriftCompensationMode.Slip)
            {
                if (TryApplySlip(framesRequested))
                {
                    return;
                }
            }

            if (_settings.DriftCompensationMode == DriftCompensationMode.Asrc)
            {
                if (TryApplyAsrc(framesRequested))
                {
                    return;
                }
            }

            int readFrames = _ringBuffer.Read(_scratch, framesRequested);
            if (readFrames < framesRequested)
            {
                Array.Clear(_scratch, readFrames * Channels, (framesRequested - readFrames) * Channels);
            }
        }

        public void Dispose()
        {
            _strictWorkerStop = true;
            _strictInputSignal.Set();
            if (_strictWorker is not null && _strictWorker.IsAlive)
            {
                _strictWorker.Join(500);
            }
            _strictInputSignal.Dispose();
        }

        private double GetPlaybackFillLevelMs()
        {
            if (_settings.DriftCompensationMode == DriftCompensationMode.Strict)
            {
                return _processedBuffer.GetFillLevelMs(_sampleRate);
            }

            return _ringBuffer.GetFillLevelMs(_sampleRate);
        }

        private bool TryApplyAsrc(int framesRequested)
        {
            if (framesRequested <= 0)
            {
                return true;
            }

            float ratio = Math.Clamp(_asrcRatioAccessor(), 0.95f, 1.05f);
            int sourceFrames = Math.Max(2, (int)Math.Floor(_asrcPhase + ((framesRequested - 1) * ratio)) + 2);
            EnsureAsrcBuffer(sourceFrames);
            FillAsrcBuffer(sourceFrames);

            if (_asrcBufferedFrames <= 0)
            {
                Array.Clear(_scratch, 0, framesRequested * Channels);
                return true;
            }

            if (_asrcBufferedFrames == 1)
            {
                for (int frame = 0; frame < framesRequested; frame++)
                {
                    int dstBase = frame * Channels;
                    _scratch[dstBase] = _asrcBuffer[0];
                    _scratch[dstBase + 1] = _asrcBuffer[1];
                }
                return true;
            }

            ResampleLinear(_asrcBuffer, _asrcBufferedFrames, _asrcPhase, ratio, _scratch, framesRequested, Channels);
            AdvanceAsrcBuffer(framesRequested, ratio);
            return true;
        }

        private bool TryApplySlip(int framesRequested)
        {
            int available = _ringBuffer.AvailableFrames;
            int hiWaterMs = _settings.DriftSlip.HiWaterMs;
            int loWaterMs = _settings.DriftSlip.LoWaterMs;
            int xfadeFrames = (int)Math.Round(_sampleRate * (_settings.DriftSlip.XfadeMs / 1000.0));

            if (_latestFillMs > hiWaterMs && available >= framesRequested + 1)
            {
                EnsureSlipBuffer(framesRequested + 1);
                int readFrames = _ringBuffer.Read(_slipBuffer, framesRequested + 1);
                if (readFrames < framesRequested + 1)
                {
                    Array.Copy(_slipBuffer, _scratch, readFrames * Channels);
                    Array.Clear(_scratch, readFrames * Channels, (framesRequested - readFrames) * Channels);
                    return true;
                }

                ApplyDrop(_slipBuffer, _scratch, framesRequested, Channels, framesRequested / 2, xfadeFrames);
                _slipCount();
                return true;
            }

            if (_latestFillMs < loWaterMs && available >= framesRequested - 1 && framesRequested > 1)
            {
                EnsureSlipBuffer(framesRequested - 1);
                int readFrames = _ringBuffer.Read(_slipBuffer, framesRequested - 1);
                if (readFrames < framesRequested - 1)
                {
                    Array.Copy(_slipBuffer, _scratch, readFrames * Channels);
                    Array.Clear(_scratch, readFrames * Channels, (framesRequested - readFrames) * Channels);
                    return true;
                }

                ApplyDuplicate(_slipBuffer, _scratch, framesRequested, Channels, framesRequested / 2, xfadeFrames);
                _slipCount();
                return true;
            }

            return false;
        }

        private void EnsureSlipBuffer(int frames)
        {
            int samples = frames * Channels;
            if (_slipBuffer.Length >= samples) return;
            _slipBuffer = new float[samples];
        }

        private void EnsureAsrcBuffer(int frames)
        {
            int samples = frames * Channels;
            if (_asrcBuffer.Length >= samples) return;
            _asrcBuffer = new float[samples];
        }

        private void FillAsrcBuffer(int requiredFrames)
        {
            if (_asrcBufferedFrames >= requiredFrames)
            {
                return;
            }

            int framesNeeded = requiredFrames - _asrcBufferedFrames;
            if (framesNeeded <= 0)
            {
                return;
            }

            float[] temp = ArrayPool<float>.Shared.Rent(framesNeeded * Channels);
            try
            {
                int readFrames = _ringBuffer.Read(temp, framesNeeded);
                if (readFrames <= 0)
                {
                    return;
                }

                Array.Copy(temp, 0, _asrcBuffer, _asrcBufferedFrames * Channels, readFrames * Channels);
                _asrcBufferedFrames += readFrames;
            }
            finally
            {
                ArrayPool<float>.Shared.Return(temp);
            }
        }

        private void AdvanceAsrcBuffer(int outputFrames, float ratio)
        {
            double nextPhase = _asrcPhase + (outputFrames * ratio);
            int consumedFrames = (int)Math.Floor(nextPhase);
            _asrcPhase = nextPhase - consumedFrames;

            if (consumedFrames <= 0)
            {
                return;
            }

            if (consumedFrames >= _asrcBufferedFrames)
            {
                _asrcBufferedFrames = 0;
                _asrcPhase = 0;
                return;
            }

            int remainingFrames = _asrcBufferedFrames - consumedFrames;
            Array.Copy(
                _asrcBuffer,
                consumedFrames * Channels,
                _asrcBuffer,
                0,
                remainingFrames * Channels);
            _asrcBufferedFrames = remainingFrames;
        }

        private void ProcessDsp(int framesRequested)
        {
            int offsetFrames = 0;
            while (offsetFrames < framesRequested)
            {
                int chunkFrames = Math.Min(_settings.BlockSize, framesRequested - offsetFrames);
                EnsureProcessBuffer(chunkFrames);
                int chunkSamples = chunkFrames * Channels;
                Array.Copy(_scratch, offsetFrames * Channels, _processBuffer, 0, chunkSamples);
                _dspGraph.Process(_processBuffer, chunkFrames, _sampleRate);
                Array.Copy(_processBuffer, 0, _scratch, offsetFrames * Channels, chunkSamples);
                offsetFrames += chunkFrames;
            }
        }

        private void ReadStrictOutput(int framesRequested)
        {
            int copiedFrames = ReadProcessedFrames(_scratch, 0, framesRequested);
            if (copiedFrames < framesRequested)
            {
                Stopwatch wait = Stopwatch.StartNew();
                while (copiedFrames < framesRequested && wait.ElapsedMilliseconds < 8)
                {
                    _strictInputSignal.Set();
                    Thread.Sleep(1);
                    copiedFrames += ReadProcessedFrames(_scratch, copiedFrames, framesRequested - copiedFrames);
                }
            }

            if (copiedFrames < framesRequested)
            {
                Array.Clear(_scratch, copiedFrames * Channels, (framesRequested - copiedFrames) * Channels);
            }
            _strictInputSignal.Set();
        }

        private void ApplyLiveTrim(float[] targetBuffer, int framesRequested)
        {
            if (Math.Abs(_liveTrimLinear - 1.0f) < 1e-6f)
            {
                return;
            }

            int samples = framesRequested * Channels;
            for (int i = 0; i < samples; i++)
            {
                targetBuffer[i] *= _liveTrimLinear;
            }
        }

        private void EnsureProcessBuffer(int frames)
        {
            int samples = frames * Channels;
            if (_processBuffer.Length >= samples) return;
            Array.Resize(ref _processBuffer, samples);
        }

        private int ReadProcessedFrames(float[] destination, int destinationFrameOffset, int framesRequested)
        {
            if (framesRequested <= 0)
            {
                return 0;
            }

            EnsureProcessBuffer(framesRequested);
            int readFrames = _processedBuffer.Read(_processBuffer, framesRequested);
            if (readFrames > 0)
            {
                Array.Copy(
                    _processBuffer,
                    0,
                    destination,
                    destinationFrameOffset * Channels,
                    readFrames * Channels);
            }

            return readFrames;
        }

        private void StrictProcessingLoop()
        {
            try
            {
                while (!_strictWorkerStop)
                {
                    if (_processedBuffer.AvailableFrames >= _strictTargetFrames)
                    {
                        _strictInputSignal.WaitOne(4);
                        continue;
                    }

                    int processedFreeFrames = _processedBuffer.CapacityFrames - _processedBuffer.AvailableFrames;
                    if (processedFreeFrames <= 0)
                    {
                        _strictInputSignal.WaitOne(2);
                        continue;
                    }

                    int availableInputFrames = _ringBuffer.AvailableFrames;
                    if (availableInputFrames <= 0)
                    {
                        _strictInputSignal.WaitOne(2);
                        continue;
                    }

                    int chunkFrames = Math.Min(_dspBlockFrames, Math.Min(availableInputFrames, processedFreeFrames));
                    int readFrames = _ringBuffer.Read(_dspBlockBuffer, chunkFrames);
                    if (readFrames <= 0)
                    {
                        _strictInputSignal.WaitOne(2);
                        continue;
                    }

                    ApplyLiveTrim(_dspBlockBuffer, readFrames);
                    _dspGraph.Process(_dspBlockBuffer, readFrames, _sampleRate);
                    _processedBuffer.Write(_dspBlockBuffer, readFrames);
                }
            }
            catch (Exception ex)
            {
                _errorCallback(ex);
            }
        }

        private static void ResampleLinear(float[] input, int inputFrames, double phase, float ratio, float[] output, int outputFrames, int channels)
        {
            if (outputFrames <= 0)
            {
                return;
            }

            if (inputFrames <= 0)
            {
                Array.Clear(output, 0, outputFrames * channels);
                return;
            }

            if (inputFrames == 1)
            {
                for (int frame = 0; frame < outputFrames; frame++)
                {
                    for (int ch = 0; ch < channels; ch++)
                    {
                        output[(frame * channels) + ch] = input[ch];
                    }
                }
                return;
            }

            for (int frame = 0; frame < outputFrames; frame++)
            {
                double srcPos = phase + (frame * ratio);
                int src0 = (int)Math.Floor(srcPos);
                int src1 = Math.Min(src0 + 1, inputFrames - 1);
                float t = (float)(srcPos - src0);
                for (int ch = 0; ch < channels; ch++)
                {
                    float a = input[(src0 * channels) + ch];
                    float b = input[(src1 * channels) + ch];
                    output[(frame * channels) + ch] = a + ((b - a) * t);
                }
            }
        }

        private static void ApplyDrop(float[] input, float[] output, int framesOut, int channels, int dropIndex, int xfadeFrames)
        {
            int inputFrames = framesOut + 1;
            int start = xfadeFrames > 0 ? Math.Max(0, dropIndex - xfadeFrames / 2) : 0;
            int end = xfadeFrames > 0 ? Math.Min(framesOut, start + xfadeFrames) : 0;

            for (int frame = 0; frame < framesOut; frame++)
            {
                int srcA = Math.Min(frame, inputFrames - 1);
                int srcB = Math.Min(frame + 1, inputFrames - 1);
                bool useFade = xfadeFrames > 0 && frame >= start && frame < end;
                float t = 0;
                if (useFade)
                {
                    t = (frame - start) / (float)(end - start);
                }

                for (int ch = 0; ch < channels; ch++)
                {
                    int indexA = (srcA * channels) + ch;
                    int indexB = (srcB * channels) + ch;
                    float a = input[indexA];
                    float b = input[indexB];

                    output[(frame * channels) + ch] = useFade ? (a * (1.0f - t) + b * t) : (frame < dropIndex ? a : b);
                }
            }
        }

        private static void ApplyDuplicate(float[] input, float[] output, int framesOut, int channels, int dupIndex, int xfadeFrames)
        {
            int inFrames = framesOut - 1;
            int start = xfadeFrames > 0 ? Math.Max(0, dupIndex - xfadeFrames / 2) : 0;
            int end = xfadeFrames > 0 ? Math.Min(framesOut, start + xfadeFrames) : 0;

            for (int frame = 0; frame < framesOut; frame++)
            {
                int srcFrame = frame > dupIndex ? frame - 1 : frame;
                if (srcFrame >= inFrames) srcFrame = inFrames - 1;

                float t = 0;
                bool useFade = xfadeFrames > 0 && frame >= start && frame < end && srcFrame + 1 < inFrames;
                if (useFade)
                {
                    t = (frame - start) / (float)(end - start);
                }

                for (int ch = 0; ch < channels; ch++)
                {
                    int srcIndex = (srcFrame * channels) + ch;
                    if (useFade)
                    {
                        int nextIndex = srcIndex + channels;
                        float pre = input[srcIndex];
                        float post = input[nextIndex];
                        output[(frame * channels) + ch] = pre * (1.0f - t) + post * t;
                    }
                    else
                    {
                        output[(frame * channels) + ch] = input[srcIndex];
                    }
                }
            }
        }

        private bool ShouldStartPlayback(double fillMs)
        {
            if (_settings.Startup.StartWhenBufferReachesTarget && fillMs >= _settings.DriftTargetBufferMs)
            {
                return true;
            }

            if (_settings.Startup.MaxPrerollMs > 0 && _prerollTimer.ElapsedMilliseconds > _settings.Startup.MaxPrerollMs)
            {
                return true;
            }

            return false;
        }

        private void ApplyFadeIn(int framesRequested)
        {
            int samples = framesRequested * Channels;
            int fadeSamples = _fadeInRemaining;
            if (fadeSamples <= 0) return;

            for (int i = 0; i < samples; i++)
            {
                float gain = _fadeInTotal <= 0 ? 1.0f : 1.0f - (fadeSamples / (float)_fadeInTotal);
                _scratch[i] *= gain;
                if ((i + 1) % Channels == 0 && _fadeInRemaining > 0)
                {
                    _fadeInRemaining--;
                    fadeSamples = _fadeInRemaining;
                }
            }

            if (_fadeInRemaining <= 0)
            {
                _fadeInRemaining = 0;
            }
        }

        private void EnsureScratch(int framesRequested)
        {
            if (_scratch.Length >= framesRequested * Channels) return;
            Array.Resize(ref _scratch, framesRequested * Channels);
        }

        private void WriteOutput(byte[] buffer, int offset, int framesRequested)
        {
            switch (_outputSampleFormat)
            {
                case OutputSampleFormat.Float32:
                    System.Buffer.BlockCopy(_scratch, 0, buffer, offset, framesRequested * Channels * sizeof(float));
                    break;
                case OutputSampleFormat.Pcm16:
                    WritePcm16(buffer, offset, framesRequested);
                    break;
                case OutputSampleFormat.Pcm24:
                    WritePcm24(buffer, offset, framesRequested);
                    break;
                case OutputSampleFormat.Pcm32:
                    WritePcm32(buffer, offset, framesRequested);
                    break;
                default:
                    Array.Clear(buffer, offset, framesRequested * _outputBytesPerFrame);
                    break;
            }
        }

        private void WritePcm16(byte[] buffer, int offset, int framesRequested)
        {
            int samples = framesRequested * Channels;
            int outIndex = offset;
            for (int i = 0; i < samples; i++)
            {
                int v = (int)MathF.Round(Math.Clamp(_scratch[i], -1.0f, 1.0f) * 32767f);
                buffer[outIndex++] = (byte)(v & 0xFF);
                buffer[outIndex++] = (byte)((v >> 8) & 0xFF);
            }
        }

        private void WritePcm24(byte[] buffer, int offset, int framesRequested)
        {
            int samples = framesRequested * Channels;
            int outIndex = offset;
            for (int i = 0; i < samples; i++)
            {
                int v = (int)MathF.Round(Math.Clamp(_scratch[i], -1.0f, 1.0f) * 8388607f);
                buffer[outIndex++] = (byte)(v & 0xFF);
                buffer[outIndex++] = (byte)((v >> 8) & 0xFF);
                buffer[outIndex++] = (byte)((v >> 16) & 0xFF);
            }
        }

        private void WritePcm32(byte[] buffer, int offset, int framesRequested)
        {
            int samples = framesRequested * Channels;
            int outIndex = offset;
            for (int i = 0; i < samples; i++)
            {
                int v = (int)MathF.Round(Math.Clamp(_scratch[i], -1.0f, 1.0f) * 2147483647f);
                buffer[outIndex++] = (byte)(v & 0xFF);
                buffer[outIndex++] = (byte)((v >> 8) & 0xFF);
                buffer[outIndex++] = (byte)((v >> 16) & 0xFF);
                buffer[outIndex++] = (byte)((v >> 24) & 0xFF);
            }
        }
    }
}

public readonly record struct EngineStatus(
    double BufferLevelMs,
    float AsrcRatio,
    int SlipCount,
    bool IsRunning,
    bool BrirHasOutput,
    float BrirInputPeak,
    float BrirOutputPeak);

public enum OutputSampleFormat
{
    Unsupported,
    Float32,
    Pcm16,
    Pcm24,
    Pcm32
}


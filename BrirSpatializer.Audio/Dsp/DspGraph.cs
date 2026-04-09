using System.Threading;
using BrirSpatializer.Audio.Dsp.Processors;
using BrirSpatializer.Audio.Ir;

namespace BrirSpatializer.Audio.Dsp;

public sealed class DspGraph
{
    private readonly DspChain _slotA = new();
    private readonly DspChain _slotB = new();
    private readonly Metering _metering = new();
    private readonly LimiterProcessor _limiter = new();
    private readonly SpectrumAnalyzer _spectrum = new();
    private readonly SmoothFader _bypassFader = new();
    private readonly SmoothFader _abFader = new();
    private readonly DelayLine _dryDelay = new();

    private float[] _dryBuffer = Array.Empty<float>();
    private float[] _dryAlignedBuffer = Array.Empty<float>();
    private float[] _slotABuffer = Array.Empty<float>();
    private float[] _slotBBuffer = Array.Empty<float>();

    private int _xfadeMs = 50;
    private int _abXfadeMs = 50;
    private int _activeSlot = (int)ActiveSlot.A;
    private int _bypass;
    private int _dryDelaySamples;
    private int _pendingDryDelaySamples;
    private int _blockSize = 512;

    public bool SpectrumEnabled { get; set; } = true;
    public bool LimiterEnabled { get; set; } = true;

    public void UpdateControls(ControlSettings controls)
    {
        Volatile.Write(ref _xfadeMs, Math.Max(0, controls.XfadeMs));
        Volatile.Write(ref _abXfadeMs, Math.Max(0, controls.XfadeMs));
        Volatile.Write(ref _activeSlot, (int)controls.ActiveSlot);
        Volatile.Write(ref _bypass, controls.Bypass ? 1 : 0);
        _slotA.LevelMatch.Enabled = controls.VolumeMatch;
        _slotB.LevelMatch.Enabled = controls.VolumeMatch;
    }

    public void SetBlockSize(int blockSize)
    {
        _blockSize = Math.Max(1, blockSize);
    }

    public void SetSpatializerMode(ActiveSlot slot, SpatializerMode mode)
    {
        GetSlot(slot).Spatializer.Mode = mode;
    }

    public void SetBrir(ActiveSlot slot, BrirIr ir, int irSwitchXfadeMs)
    {
        GetSlot(slot).Spatializer.SetBrir(ir, irSwitchXfadeMs);
    }

    public int GetDryAlignmentLatencySamples()
    {
        int a = _slotA.Spatializer.LatencySamples;
        int b = _slotB.Spatializer.LatencySamples;
        return Math.Max(a, b);
    }

    public void SetDryDelaySamples(int samples)
    {
        Volatile.Write(ref _pendingDryDelaySamples, Math.Max(0, samples));
    }

    public void ApplyPendingDryDelaySamples()
    {
        Volatile.Write(ref _dryDelaySamples, Volatile.Read(ref _pendingDryDelaySamples));
    }

    public MeterSnapshot GetMeterSnapshot() => _metering.Snapshot();

    public SpectrumSnapshot GetSpectrumSnapshot() => _spectrum.Snapshot();

    public bool HasSpatializerOutput()
    {
        return _slotA.Spatializer.HasOutput || _slotB.Spatializer.HasOutput;
    }

    public (float InputPeak, float OutputPeak) GetSpatializerPeaks()
    {
        float inPeak = Math.Max(_slotA.Spatializer.LastInputPeak, _slotB.Spatializer.LastInputPeak);
        float outPeak = Math.Max(_slotA.Spatializer.LastOutputPeak, _slotB.Spatializer.LastOutputPeak);
        return (inPeak, outPeak);
    }

    public void Process(float[] buffer, int frames, int sampleRate)
    {
        EnsureBuffers(frames);
        int samples = frames * 2;

        Array.Copy(buffer, _dryBuffer, samples);
        int blockFrames = Math.Max(frames, _blockSize);
        _dryDelay.SetDelaySamples(Volatile.Read(ref _dryDelaySamples), blockFrames, 2);
        _dryDelay.Process(_dryBuffer, _dryAlignedBuffer, frames);

        bool activeA = Volatile.Read(ref _activeSlot) == (int)ActiveSlot.A;

        Array.Copy(_dryBuffer, _slotABuffer, samples);
        _slotA.Process(_slotABuffer, _dryBuffer, frames, sampleRate);

        Array.Copy(_dryBuffer, _slotBBuffer, samples);
        _slotB.Process(_slotBBuffer, _dryBuffer, frames, sampleRate);

        float abTarget = activeA ? 0.0f : 1.0f;
        _abFader.SetTarget(abTarget, sampleRate, Volatile.Read(ref _abXfadeMs));

        float bypassTarget = Volatile.Read(ref _bypass) == 0 ? 1.0f : 0.0f;
        _bypassFader.SetTarget(bypassTarget, sampleRate, Volatile.Read(ref _xfadeMs));

        for (int frame = 0; frame < frames; frame++)
        {
            float ab = _abFader.Next();
            float mix = _bypassFader.Next();
            int baseIndex = frame * 2;

            for (int ch = 0; ch < 2; ch++)
            {
                int i = baseIndex + ch;
                float processed = _slotABuffer[i] * (1.0f - ab) + _slotBBuffer[i] * ab;
                buffer[i] = _dryAlignedBuffer[i] * (1.0f - mix) + processed * mix;
            }
        }

        _metering.Update(buffer, frames, sampleRate);
        if (LimiterEnabled)
        {
            _limiter.Process(buffer, frames, sampleRate);
        }
        if (SpectrumEnabled)
        {
            _spectrum.Update(buffer, frames, sampleRate);
        }
    }

    private void EnsureBuffers(int frames)
    {
        int samples = frames * 2;
        if (_dryBuffer.Length == samples) return;
        _dryBuffer = new float[samples];
        _dryAlignedBuffer = new float[samples];
        _slotABuffer = new float[samples];
        _slotBBuffer = new float[samples];
    }

    private sealed class DspChain
    {
        public PreampProcessor Preamp { get; } = new();
        public SpatializerProcessor Spatializer { get; } = new();
        public WetDryProcessor WetDry { get; } = new();
        public EqProcessor Eq { get; } = new();
        public LevelMatchProcessor LevelMatch { get; } = new();

        public void Process(float[] buffer, float[] dry, int frames, int sampleRate)
        {
            Preamp.Process(buffer, frames);
            Spatializer.Process(buffer, frames, sampleRate);
            WetDry.Process(buffer, dry, frames);
            Eq.Process(buffer, frames);
            LevelMatch.Process(buffer, frames);
        }
    }

    private DspChain GetSlot(ActiveSlot slot) => slot == ActiveSlot.A ? _slotA : _slotB;

    private sealed class SmoothFader
    {
        private float _value = 1.0f;
        private float _target = 1.0f;
        private float _step;
        private int _remaining;

        public void SetTarget(float target, int sampleRate, int xfadeMs)
        {
            if (Math.Abs(target - _target) < 1e-6f && _remaining == 0) return;
            _target = target;
            int totalSamples = (int)(sampleRate * (xfadeMs / 1000.0));
            if (totalSamples <= 0)
            {
                _value = target;
                _remaining = 0;
                _step = 0;
                return;
            }

            _remaining = totalSamples;
            _step = (_target - _value) / _remaining;
        }

        public float Next()
        {
            if (_remaining <= 0) return _value;
            _value += _step;
            _remaining--;
            if (_remaining == 0) _value = _target;
            return _value;
        }
    }
}


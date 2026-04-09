using BrirSpatializer.Audio.Dsp.Spatial;
using BrirSpatializer.Audio.Ir;
using SpatialBrir = BrirSpatializer.Audio.Dsp.Spatial.BrirSpatializer;

namespace BrirSpatializer.Audio.Dsp.Processors;

public enum SpatializerMode
{
    Off,
    Brir
}

public sealed class SpatializerProcessor
{
    private readonly FrameFader _irFader = new();
    private SpatialBrir? _active;
    private SpatialBrir? _pending;
    private SpatialBrir? _fadeFrom;
    private SpatialBrir? _fadeTo;
    private float[] _fadeOld = Array.Empty<float>();
    private float[] _fadeNew = Array.Empty<float>();
    private int _irSwitchMs = 100;

    public SpatializerMode Mode { get; set; } = SpatializerMode.Off;

    public int LatencySamples => Mode == SpatializerMode.Brir
        ? _active?.LatencySamples ?? _pending?.LatencySamples ?? 0
        : 0;

    public bool HasOutput => _active?.HasOutput ?? false;
    public float LastInputPeak => _active?.LastInputPeak ?? 0f;
    public float LastOutputPeak => _active?.LastOutputPeak ?? 0f;

    public void SetBrir(BrirIr ir, int irSwitchXfadeMs)
    {
        _pending = new SpatialBrir(ir, 4096, 2048);
        _pending.Reset();
        _irSwitchMs = Math.Max(0, irSwitchXfadeMs);
        EnsureFadeBuffers(4096);
    }

    public void Process(float[] buffer, int frames, int sampleRate)
    {
        if (Mode != SpatializerMode.Brir)
        {
            return;
        }

        if (_active is null)
        {
            if (_pending is null) return;
            _active = _pending;
            _pending = null;
        }

        if (_pending is not null && !_irFader.IsActive)
        {
            if (_irSwitchMs <= 0)
            {
                _active = _pending;
                _pending = null;
            }
            else
            {
                _fadeFrom = _active;
                _fadeTo = _pending;
                _pending = null;
                int totalFrames = (int)Math.Max(1, Math.Round(sampleRate * (_irSwitchMs / 1000.0)));
                _irFader.Start(0.0f, 1.0f, totalFrames);
            }
        }

        if (_irFader.IsActive && _fadeFrom is not null && _fadeTo is not null)
        {
            EnsureFadeBuffers(frames);
            Array.Copy(buffer, _fadeOld, frames * 2);
            Array.Copy(buffer, _fadeNew, frames * 2);
            _fadeFrom.Process(_fadeOld, frames);
            _fadeTo.Process(_fadeNew, frames);

            for (int frame = 0; frame < frames; frame++)
            {
                float t = _irFader.Next();
                int baseIndex = frame * 2;
                buffer[baseIndex] = _fadeOld[baseIndex] * (1.0f - t) + _fadeNew[baseIndex] * t;
                buffer[baseIndex + 1] = _fadeOld[baseIndex + 1] * (1.0f - t) + _fadeNew[baseIndex + 1] * t;
            }

            if (!_irFader.IsActive)
            {
                _active = _fadeTo;
                _fadeFrom = null;
                _fadeTo = null;
            }
            return;
        }

        _active.Process(buffer, frames);
    }

    private void EnsureFadeBuffers(int frames)
    {
        int samples = frames * 2;
        if (_fadeOld.Length < samples) _fadeOld = new float[samples];
        if (_fadeNew.Length < samples) _fadeNew = new float[samples];
    }

    private sealed class FrameFader
    {
        private float _value;
        private float _target;
        private float _step;
        private int _remaining;

        public bool IsActive => _remaining > 0;

        public void Start(float value, float target, int totalFrames)
        {
            _value = value;
            _target = target;
            _remaining = Math.Max(0, totalFrames);
            _step = _remaining > 0 ? (_target - _value) / _remaining : 0f;
        }

        public float Next()
        {
            if (_remaining <= 0) return _target;
            _value += _step;
            _remaining--;
            if (_remaining == 0) _value = _target;
            return _value;
        }
    }
}


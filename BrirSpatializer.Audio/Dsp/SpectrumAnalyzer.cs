using System.Threading;
using MathNet.Numerics.IntegralTransforms;
using MathNet.Numerics;

namespace BrirSpatializer.Audio.Dsp;

public sealed class SpectrumAnalyzer
{
    private const int FftSize = 2048;
    private const int HopSize = 512;
    private const float MinDb = -80.0f;
    private static readonly float[] BandCentersHz =
    {
        20f, 25f, 31.5f, 40f, 50f, 63f, 80f, 100f, 125f, 160f, 200f, 250f, 315f,
        400f, 500f, 630f, 800f, 1000f, 1250f, 1600f, 2000f, 2500f, 3150f, 4000f,
        5000f, 6300f, 8000f, 10000f, 12500f, 16000f, 20000f
    };

    private readonly object _lock = new();
    private readonly float[] _ring = new float[FftSize];
    private int _ringIndex;
    private int _samplesSinceLast;
    private int _sampleRate;
    private float[] _window = Array.Empty<float>();
    private Complex32[] _fftBuffer = Array.Empty<Complex32>();
    private int[] _bandStartBins = Array.Empty<int>();
    private int[] _bandEndBins = Array.Empty<int>();
    private float[] _bandsDb = new float[BandCentersHz.Length];
    private float[] _smoothedDb = new float[BandCentersHz.Length];

    private float _attackCoeff;
    private float _releaseCoeff;

    public SpectrumAnalyzer()
    {
        for (int i = 0; i < _bandsDb.Length; i++)
        {
            _bandsDb[i] = MinDb;
            _smoothedDb[i] = MinDb;
        }
    }

    public void Update(float[] buffer, int frames, int sampleRate)
    {
        if (frames <= 0) return;
        EnsureInitialized(sampleRate);

        int samples = frames * 2;
        int idx = 0;
        for (int frame = 0; frame < frames; frame++)
        {
            float mono = 0.5f * (buffer[idx] + buffer[idx + 1]);
            _ring[_ringIndex] = mono;
            _ringIndex = (_ringIndex + 1) % FftSize;
            idx += 2;
        }

        _samplesSinceLast += frames;
        while (_samplesSinceLast >= HopSize)
        {
            _samplesSinceLast -= HopSize;
            ComputeBands();
        }
    }

    public SpectrumSnapshot Snapshot()
    {
        lock (_lock)
        {
            float[] copy = new float[_bandsDb.Length];
            Array.Copy(_bandsDb, copy, _bandsDb.Length);
            return new SpectrumSnapshot(copy);
        }
    }

    private void EnsureInitialized(int sampleRate)
    {
        if (_sampleRate == sampleRate && _fftBuffer.Length == FftSize) return;
        _sampleRate = sampleRate;
        _fftBuffer = new Complex32[FftSize];
        _window = Window.Hann(FftSize).Select(v => (float)v).ToArray();
        BuildBandBins(sampleRate);

        float dt = HopSize / (float)sampleRate;
        _attackCoeff = MathF.Exp(-dt / 0.10f);
        _releaseCoeff = MathF.Exp(-dt / 0.30f);
    }

    private void BuildBandBins(int sampleRate)
    {
        _bandStartBins = new int[BandCentersHz.Length];
        _bandEndBins = new int[BandCentersHz.Length];
        float nyquist = sampleRate * 0.5f;
        float binHz = sampleRate / (float)FftSize;
        float bandFactor = MathF.Pow(2.0f, 1.0f / 6.0f);

        for (int i = 0; i < BandCentersHz.Length; i++)
        {
            float center = BandCentersHz[i];
            float lo = MathF.Max(0.0f, center / bandFactor);
            float hi = MathF.Min(nyquist, center * bandFactor);
            int start = (int)MathF.Floor(lo / binHz);
            int end = (int)MathF.Ceiling(hi / binHz);
            start = Math.Clamp(start, 0, FftSize / 2);
            end = Math.Clamp(end, start + 1, FftSize / 2);
            _bandStartBins[i] = start;
            _bandEndBins[i] = end;
        }
    }

    private void ComputeBands()
    {
        int readIndex = _ringIndex;
        for (int i = 0; i < FftSize; i++)
        {
            float sample = _ring[readIndex];
            float windowed = sample * _window[i];
            _fftBuffer[i] = new Complex32(windowed, 0);
            readIndex++;
            if (readIndex == FftSize) readIndex = 0;
        }

        Fourier.Forward(_fftBuffer, FourierOptions.Matlab);

        float[] newBands = new float[BandCentersHz.Length];
        for (int b = 0; b < BandCentersHz.Length; b++)
        {
            int start = _bandStartBins[b];
            int end = _bandEndBins[b];
            float sum = 0.0f;
            int count = 0;
            for (int bin = start; bin <= end; bin++)
            {
                float re = _fftBuffer[bin].Real;
                float im = _fftBuffer[bin].Imaginary;
                float mag = re * re + im * im;
                sum += mag;
                count++;
            }

            float avg = count > 0 ? sum / count : 0.0f;
            float db = 10.0f * MathF.Log10(avg + 1e-12f);
            if (db < MinDb) db = MinDb;
            newBands[b] = db;
        }

        lock (_lock)
        {
            for (int i = 0; i < newBands.Length; i++)
            {
                float target = newBands[i];
                float current = _smoothedDb[i];
                float coeff = target > current ? _attackCoeff : _releaseCoeff;
                float smoothed = target + coeff * (current - target);
                _smoothedDb[i] = smoothed;
                _bandsDb[i] = smoothed;
            }
        }
    }
}

public readonly record struct SpectrumSnapshot(float[] BandsDb);


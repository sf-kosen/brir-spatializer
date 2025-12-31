using System.Threading;

namespace BrirSpatializer.Audio.Dsp;

public sealed class Metering
{
    private float _rms;
    private float _peak;
    private int _clipCount;
    private float _vu;
    private float _peakHold;
    private float _truePeak;
    private float _truePeakHold;
    private float _prevL;
    private float _prevR;
    private bool _hasPrev;

    public void Update(float[] buffer, int frames, int sampleRate)
    {
        if (frames <= 0) return;
        int samples = frames * 2;
        double sum = 0;
        float peak = 0;
        int clip = 0;

        float truePeak = 0.0f;
        int sampleIndex = 0;
        for (int frame = 0; frame < frames; frame++)
        {
            float left = buffer[sampleIndex];
            float right = buffer[sampleIndex + 1];
            sampleIndex += 2;

            float al = Math.Abs(left);
            float ar = Math.Abs(right);
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
            sum += left * left + right * right;
            if (al >= 1.0f) clip++;
            if (ar >= 1.0f) clip++;

            if (!_hasPrev)
            {
                _prevL = left;
                _prevR = right;
                _hasPrev = true;
            }

            truePeak = Math.Max(truePeak, CalcTruePeakSegment(_prevL, left));
            truePeak = Math.Max(truePeak, CalcTruePeakSegment(_prevR, right));
            _prevL = left;
            _prevR = right;
        }

        float rms = (float)Math.Sqrt(sum / samples);
        Volatile.Write(ref _rms, rms);
        Volatile.Write(ref _peak, peak);
        Interlocked.Exchange(ref _clipCount, clip);
        Volatile.Write(ref _truePeak, truePeak);

        float dt = frames / (float)sampleRate;
        float vuCoeff = MathF.Exp(-dt / 0.30f);
        float currentVu = Volatile.Read(ref _vu);
        float targetVu = rms;
        float vu = targetVu + vuCoeff * (currentVu - targetVu);
        Volatile.Write(ref _vu, vu);

        float peakHold = Volatile.Read(ref _peakHold);
        float decayCoeff = MathF.Exp(-dt / 1.0f);
        peakHold = peak >= peakHold ? peak : MathF.Max(peak, peakHold * decayCoeff);
        Volatile.Write(ref _peakHold, peakHold);

        float truePeakHold = Volatile.Read(ref _truePeakHold);
        truePeakHold = truePeak >= truePeakHold ? truePeak : MathF.Max(truePeak, truePeakHold * decayCoeff);
        Volatile.Write(ref _truePeakHold, truePeakHold);
    }

    public MeterSnapshot Snapshot()
    {
        return new MeterSnapshot(
            Volatile.Read(ref _rms),
            Volatile.Read(ref _peak),
            Volatile.Read(ref _clipCount),
            Volatile.Read(ref _vu),
            Volatile.Read(ref _peakHold),
            Volatile.Read(ref _truePeak),
            Volatile.Read(ref _truePeakHold));
    }

    private static float CalcTruePeakSegment(float a, float b)
    {
        float max = MathF.Max(Math.Abs(a), Math.Abs(b));
        float d = b - a;
        float s1 = a + d * 0.25f;
        float s2 = a + d * 0.5f;
        float s3 = a + d * 0.75f;
        max = MathF.Max(max, Math.Abs(s1));
        max = MathF.Max(max, Math.Abs(s2));
        max = MathF.Max(max, Math.Abs(s3));
        return max;
    }
}

public readonly record struct MeterSnapshot(
    float Rms,
    float Peak,
    int ClipCount,
    float Vu,
    float PeakHold,
    float TruePeak,
    float TruePeakHold);


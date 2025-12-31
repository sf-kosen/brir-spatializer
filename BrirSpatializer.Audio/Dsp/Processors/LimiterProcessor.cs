namespace BrirSpatializer.Audio.Dsp.Processors;

public sealed class LimiterProcessor
{
    private float _thresholdLinear = DbToLinear(-1.0f);
    private float _gain = 1.0f;
    private float _releaseMs = 200.0f;

    public float ThresholdDb
    {
        get => LinearToDb(_thresholdLinear);
        set => _thresholdLinear = DbToLinear(value);
    }

    public bool Enabled { get; set; } = true;

    public void Process(float[] buffer, int frames, int sampleRate)
    {
        if (!Enabled || frames <= 0) return;
        int samples = frames * 2;
        float limit = _thresholdLinear;
        float peak = 0f;
        for (int i = 0; i < samples; i++)
        {
            float v = Math.Abs(buffer[i]);
            if (v > peak) peak = v;
        }

        float desiredGain = peak > limit && peak > 0f ? limit / peak : 1.0f;
        if (desiredGain < _gain)
        {
            _gain = desiredGain;
        }
        else if (_gain < 1.0f)
        {
            float releaseSec = Math.Max(1.0f, _releaseMs) / 1000.0f;
            float releaseCoeff = 1.0f - (float)Math.Exp(-frames / (sampleRate * releaseSec));
            _gain += (1.0f - _gain) * releaseCoeff;
            if (_gain > 1.0f) _gain = 1.0f;
        }

        if (Math.Abs(_gain - 1.0f) < 1e-6f) return;
        for (int i = 0; i < samples; i++)
        {
            buffer[i] *= _gain;
        }
    }

    private static float DbToLinear(float db) => (float)Math.Pow(10.0, db / 20.0);
    private static float LinearToDb(float linear) => 20.0f * (float)Math.Log10(Math.Max(linear, 1e-6f));
}


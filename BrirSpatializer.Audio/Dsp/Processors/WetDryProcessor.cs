namespace BrirSpatializer.Audio.Dsp.Processors;

public sealed class WetDryProcessor
{
    private float _wet = 1.0f;

    public float Wet
    {
        get => _wet;
        set => _wet = Math.Clamp(value, 0.0f, 1.0f);
    }

    public void Process(float[] processed, float[] dry, int frames)
    {
        float wet = _wet;
        if (wet >= 0.999f) return;
        float dryGain = 1.0f - wet;
        int samples = frames * 2;
        for (int i = 0; i < samples; i++)
        {
            processed[i] = processed[i] * wet + dry[i] * dryGain;
        }
    }
}


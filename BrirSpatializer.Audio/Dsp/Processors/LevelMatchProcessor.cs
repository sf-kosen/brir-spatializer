namespace BrirSpatializer.Audio.Dsp.Processors;

public sealed class LevelMatchProcessor
{
    private float _gain = 1.0f;

    public bool Enabled { get; set; } = true;

    public float Gain
    {
        get => _gain;
        set => _gain = value;
    }

    public void Process(float[] buffer, int frames)
    {
        if (!Enabled || _gain == 1.0f) return;
        int samples = frames * 2;
        for (int i = 0; i < samples; i++)
        {
            buffer[i] *= _gain;
        }
    }
}


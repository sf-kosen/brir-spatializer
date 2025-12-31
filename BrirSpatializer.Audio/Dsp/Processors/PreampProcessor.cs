namespace BrirSpatializer.Audio.Dsp.Processors;

public sealed class PreampProcessor
{
    private float _gain = 1.0f;

    public float Gain
    {
        get => _gain;
        set => _gain = value;
    }

    public void Process(float[] buffer, int frames)
    {
        if (_gain == 1.0f) return;
        int samples = frames * 2;
        for (int i = 0; i < samples; i++)
        {
            buffer[i] *= _gain;
        }
    }
}


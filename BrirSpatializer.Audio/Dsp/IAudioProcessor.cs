namespace BrirSpatializer.Audio.Dsp;

public interface IAudioProcessor
{
    void Process(float[] interleavedStereo, int frames, int sampleRate);
}


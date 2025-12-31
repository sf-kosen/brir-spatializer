namespace BrirSpatializer.Audio.Ir;

public sealed class BrirIr
{
    public BrirIr(int sampleRate, float[] ll, float[] lr, float[] rl, float[] rr)
    {
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (ll.Length != lr.Length || ll.Length != rl.Length || ll.Length != rr.Length)
        {
            throw new ArgumentException("All IR channels must have the same length.");
        }

        SampleRate = sampleRate;
        LengthFrames = ll.Length;
        Ll = ll;
        Lr = lr;
        Rl = rl;
        Rr = rr;
    }

    public int SampleRate { get; }
    public int LengthFrames { get; }
    public float[] Ll { get; }
    public float[] Lr { get; }
    public float[] Rl { get; }
    public float[] Rr { get; }
}


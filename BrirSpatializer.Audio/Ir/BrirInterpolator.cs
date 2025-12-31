namespace BrirSpatializer.Audio.Ir;

public sealed class BrirInterpolator
{
    public BrirIr Interpolate(BrirSet set, float x, float y)
    {
        BrirGridIndex index = BrirGridIndexCalculator.FromSeat(x, y, set.Rows, set.Columns);

        BrirIr ir00 = set.GetAt(index.Row0, index.Col0);
        BrirIr ir10 = set.GetAt(index.Row0, index.Col1);
        BrirIr ir01 = set.GetAt(index.Row1, index.Col0);
        BrirIr ir11 = set.GetAt(index.Row1, index.Col1);

        float[] ll = new float[set.LengthFrames];
        float[] lr = new float[set.LengthFrames];
        float[] rl = new float[set.LengthFrames];
        float[] rr = new float[set.LengthFrames];

        Mix(ll, ir00.Ll, ir10.Ll, ir01.Ll, ir11.Ll, index);
        Mix(lr, ir00.Lr, ir10.Lr, ir01.Lr, ir11.Lr, index);
        Mix(rl, ir00.Rl, ir10.Rl, ir01.Rl, ir11.Rl, index);
        Mix(rr, ir00.Rr, ir10.Rr, ir01.Rr, ir11.Rr, index);

        return new BrirIr(set.SampleRate, ll, lr, rl, rr);
    }

    private static void Mix(float[] dst, float[] a, float[] b, float[] c, float[] d, BrirGridIndex index)
    {
        float w00 = index.W00;
        float w10 = index.W10;
        float w01 = index.W01;
        float w11 = index.W11;

        for (int i = 0; i < dst.Length; i++)
        {
            dst[i] = a[i] * w00 + b[i] * w10 + c[i] * w01 + d[i] * w11;
        }
    }
}


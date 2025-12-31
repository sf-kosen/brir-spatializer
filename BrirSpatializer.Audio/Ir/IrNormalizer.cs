namespace BrirSpatializer.Audio.Ir;

public enum NormalizeMode
{
    None,
    Peak,
    Rms
}

public sealed class IrNormalizeResult
{
    public IrNormalizeResult(BrirIr ir, bool skipped, string? warning)
    {
        Ir = ir;
        Skipped = skipped;
        Warning = warning;
    }

    public BrirIr Ir { get; }
    public bool Skipped { get; }
    public string? Warning { get; }
}

public static class IrNormalizer
{
    public static IrNormalizeResult Normalize(BrirIr ir, NormalizeMode mode, float targetPeakDbFs, float targetRmsDbFs)
    {
        if (mode == NormalizeMode.None)
        {
            return new IrNormalizeResult(ir, false, null);
        }

        float peak = FindPeak(ir);
        if (peak < 1e-6f)
        {
            return new IrNormalizeResult(ir, true, "IR peak too small; normalization skipped.");
        }

        float gain = 1.0f;
        if (mode == NormalizeMode.Peak)
        {
            gain = DbToLinear(targetPeakDbFs) / peak;
        }
        else if (mode == NormalizeMode.Rms)
        {
            float rms = FindRms(ir);
            if (rms < 1e-6f)
            {
                return new IrNormalizeResult(ir, true, "IR RMS too small; normalization skipped.");
            }
            gain = DbToLinear(targetRmsDbFs) / rms;
        }

        ApplyGain(ir.Ll, gain);
        ApplyGain(ir.Lr, gain);
        ApplyGain(ir.Rl, gain);
        ApplyGain(ir.Rr, gain);
        return new IrNormalizeResult(ir, false, null);
    }

    private static float FindPeak(BrirIr ir)
    {
        float peak = 0f;
        peak = Math.Max(peak, PeakArray(ir.Ll));
        peak = Math.Max(peak, PeakArray(ir.Lr));
        peak = Math.Max(peak, PeakArray(ir.Rl));
        peak = Math.Max(peak, PeakArray(ir.Rr));
        return peak;
    }

    private static float FindRms(BrirIr ir)
    {
        double sum = 0;
        int total = ir.LengthFrames * 4;
        sum += SumSquares(ir.Ll);
        sum += SumSquares(ir.Lr);
        sum += SumSquares(ir.Rl);
        sum += SumSquares(ir.Rr);
        return (float)Math.Sqrt(sum / Math.Max(1, total));
    }

    private static float PeakArray(float[] data)
    {
        float peak = 0f;
        for (int i = 0; i < data.Length; i++)
        {
            float v = Math.Abs(data[i]);
            if (v > peak) peak = v;
        }
        return peak;
    }

    private static double SumSquares(float[] data)
    {
        double sum = 0;
        for (int i = 0; i < data.Length; i++)
        {
            float v = data[i];
            sum += v * v;
        }
        return sum;
    }

    private static void ApplyGain(float[] data, float gain)
    {
        if (Math.Abs(gain - 1.0f) < 1e-6f) return;
        for (int i = 0; i < data.Length; i++)
        {
            data[i] *= gain;
        }
    }

    private static float DbToLinear(float db) => (float)Math.Pow(10.0, db / 20.0);
}


namespace BrirSpatializer.Audio.Ir;

public sealed class BrirSet
{
    public BrirSet(int rows, int columns, IReadOnlyList<BrirIr> irs)
    {
        if (rows <= 0) throw new ArgumentOutOfRangeException(nameof(rows));
        if (columns <= 0) throw new ArgumentOutOfRangeException(nameof(columns));
        if (irs.Count != rows * columns) throw new ArgumentException("IR count must match rows*columns.");
        SampleRate = irs[0].SampleRate;
        LengthFrames = irs[0].LengthFrames;
        foreach (BrirIr ir in irs)
        {
            if (ir.SampleRate != SampleRate) throw new ArgumentException("IR sample rates must match.");
            if (ir.LengthFrames != LengthFrames) throw new ArgumentException("IR lengths must match.");
        }

        Rows = rows;
        Columns = columns;
        Irs = irs;
    }

    public int Rows { get; }
    public int Columns { get; }
    public int SampleRate { get; }
    public int LengthFrames { get; }
    public IReadOnlyList<BrirIr> Irs { get; }

    public BrirIr GetAt(int row, int column)
    {
        int index = row * Columns + column;
        return Irs[index];
    }
}


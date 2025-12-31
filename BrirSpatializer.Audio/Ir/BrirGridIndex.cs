namespace BrirSpatializer.Audio.Ir;

public readonly record struct BrirGridIndex(
    int Row0,
    int Col0,
    int Row1,
    int Col1,
    float W00,
    float W10,
    float W01,
    float W11);

public static class BrirGridIndexCalculator
{
    public static BrirGridIndex FromSeat(float x, float y, int rows, int cols)
    {
        x = Math.Clamp(x, 0f, 1f);
        y = Math.Clamp(y, 0f, 1f);

        float fx = x * (cols - 1);
        float fy = y * (rows - 1);
        int c0 = (int)MathF.Floor(fx);
        int r0 = (int)MathF.Floor(fy);
        int c1 = Math.Min(c0 + 1, cols - 1);
        int r1 = Math.Min(r0 + 1, rows - 1);
        float tx = fx - c0;
        float ty = fy - r0;

        float w00 = (1 - tx) * (1 - ty);
        float w10 = tx * (1 - ty);
        float w01 = (1 - tx) * ty;
        float w11 = tx * ty;

        return new BrirGridIndex(r0, c0, r1, c1, w00, w10, w01, w11);
    }
}


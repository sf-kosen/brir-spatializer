namespace BrirSpatializer.App;

public sealed record VUMeterTick(
    float AngleDeg,
    float StartY,
    float EndY,
    string? Label,
    double LabelX,
    double LabelY,
    bool IsMajor,
    bool IsZero,
    bool IsRed);


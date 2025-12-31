using BrirSpatializer.Audio.Presets;

namespace BrirSpatializer.App;

public sealed class LastSettings
{
    public string? InputDeviceId { get; set; }
    public string? OutputDeviceId { get; set; }
    public Preset? Preset { get; set; }
}


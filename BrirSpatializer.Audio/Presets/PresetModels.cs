using System.Text.Json.Serialization;
using BrirSpatializer.Audio.Ir;

namespace BrirSpatializer.Audio.Presets;

public sealed class Preset
{
    public string Name { get; set; } = "Default";
    public string Version { get; set; } = "0.1.4";
    public PresetAudio Audio { get; set; } = new();
    public PresetControls Controls { get; set; } = new();
    public PresetSlots Slots { get; set; } = new();
}

public sealed class PresetAudio
{
    public int DriftTargetBufferMs { get; set; } = 60;
    public PresetDriftSlip DriftSlip { get; set; } = new();
}

public sealed class PresetDriftSlip
{
    public int HiWaterMs { get; set; } = 90;
    public int LoWaterMs { get; set; } = 30;
    public int XfadeMs { get; set; } = 10;
}

public sealed class PresetControls
{
    public bool Bypass { get; set; }
    public ActiveSlot ActiveSlot { get; set; } = ActiveSlot.A;
    public bool VolumeMatch { get; set; } = true;
    public int XfadeMs { get; set; } = 50;
    public int IrSwitchXfadeMs { get; set; } = 100;
}

public sealed class PresetSlots
{
    public PresetSlot A { get; set; } = new();
    public PresetSlot B { get; set; } = new();
}

public sealed class PresetSlot
{
    public PresetSpatializer Spatializer { get; set; } = new();
}

public enum PresetSpatializerMode
{
    Off,
    Brir
}

public sealed class PresetSpatializer
{
    public PresetSpatializerMode Mode { get; set; } = PresetSpatializerMode.Off;
    public string? BrirPath { get; set; }
    public PresetBrirSet? BrirSet { get; set; }
    public float SeatX { get; set; } = 0.5f;
    public float SeatY { get; set; } = 0.5f;
    public PresetNormalize Normalize { get; set; } = new();
}

public sealed class PresetNormalize
{
    public NormalizeMode Mode { get; set; } = NormalizeMode.Peak;
    public float TargetPeakDbFS { get; set; } = -6.0f;
    public float TargetRmsDbFS { get; set; } = -18.0f;
}

public sealed class PresetBrirSet
{
    public int Rows { get; set; } = 1;
    public int Columns { get; set; } = 1;
    public List<string> Paths { get; set; } = new();
}


namespace BrirSpatializer.Audio;

public enum DriftCompensationMode
{
    Asrc,
    Slip,
    Strict
}

public sealed class StartupSettings
{
    public int PrerollMs { get; set; } = 800;
    public int MaxPrerollMs { get; set; } = 5000;
    public bool MuteDuringPreroll { get; set; } = true;
    public bool HoldReadPointerWhileMuted { get; set; } = true;
    public bool StartWhenBufferReachesTarget { get; set; } = true;
    public int FadeInMs { get; set; } = 80;
}

public sealed class DriftAsrcSettings
{
    public double Kp { get; set; } = 0.00002;
    public double Ki { get; set; } = 0.0000005;
    public double RMin { get; set; } = 0.995;
    public double RMax { get; set; } = 1.005;
}

public sealed class DriftSlipSettings
{
    public int HiWaterMs { get; set; } = 90;
    public int LoWaterMs { get; set; } = 30;
    public int XfadeMs { get; set; } = 10;
}

public sealed class AudioSettings
{
    public int BlockSize { get; set; } = 512;
    public DriftCompensationMode DriftCompensationMode { get; set; } = DriftCompensationMode.Asrc;
    public int DriftTargetBufferMs { get; set; } = 60;
    public float LiveTrimDb { get; set; } = 0.0f;
    public StartupSettings Startup { get; set; } = new();
    public DriftAsrcSettings DriftAsrc { get; set; } = new();
    public DriftSlipSettings DriftSlip { get; set; } = new();
}

public enum ActiveSlot
{
    A,
    B
}

public sealed class ControlSettings
{
    public bool Bypass { get; set; }
    public ActiveSlot ActiveSlot { get; set; } = ActiveSlot.A;
    public bool VolumeMatch { get; set; } = true;
    public int XfadeMs { get; set; } = 50;
    public int IrSwitchXfadeMs { get; set; } = 100;
}


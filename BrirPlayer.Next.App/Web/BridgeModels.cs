namespace BrirPlayer.Next.App.Web;

internal sealed record BridgeDevice(string Id, string Label, string Detail);

internal sealed record BridgeRouteState(
    string Mode,
    string InputId,
    string OutputId,
    double OutputVolume,
    bool Exclusive,
    bool VolumeMatch);

internal sealed record BridgeStatePayload(
    bool Bridge,
    bool IsRunning,
    string SessionStatus,
    string? LastError,
    IReadOnlyList<BridgeDevice> InputDevices,
    IReadOnlyList<BridgeDevice> OutputDevices,
    BridgeRouteState Route);

internal sealed record PlaybackRequest(
    string Mode,
    string PresetId,
    double SeatX,
    double SeatY,
    bool VolumeMatch);

internal sealed record WebEnvelope(string Type, object Payload);

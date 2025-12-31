using NAudio.CoreAudioApi;

namespace BrirSpatializer.Audio;

public sealed class DeviceManager
{
    private readonly MMDeviceEnumerator _enumerator = new();

    public IReadOnlyList<AudioDeviceInfo> GetInputDevices()
    {
        return _enumerator.EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active)
            .Select(d => new AudioDeviceInfo(d.ID, d.FriendlyName))
            .ToList();
    }

    public IReadOnlyList<AudioDeviceInfo> GetOutputDevices()
    {
        return _enumerator.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active)
            .Select(d => new AudioDeviceInfo(d.ID, d.FriendlyName))
            .ToList();
    }

    public MMDevice? TryGetDevice(string id)
    {
        try
        {
            return _enumerator.GetDevice(id);
        }
        catch
        {
            return null;
        }
    }
}


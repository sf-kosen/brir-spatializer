using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;

namespace BrirPlayer.Next.App.Web;

internal sealed class AudioDeviceMonitor : IMMNotificationClient, IDisposable
{
    private readonly MMDeviceEnumerator _enumerator = new();

    public AudioDeviceMonitor()
    {
        _enumerator.RegisterEndpointNotificationCallback(this);
    }

    public event EventHandler? DevicesChanged;

    public void Dispose()
    {
        _enumerator.UnregisterEndpointNotificationCallback(this);
    }

    public void OnDeviceStateChanged(string deviceId, DeviceState newState)
    {
        DevicesChanged?.Invoke(this, EventArgs.Empty);
    }

    public void OnDeviceAdded(string pwstrDeviceId)
    {
        DevicesChanged?.Invoke(this, EventArgs.Empty);
    }

    public void OnDeviceRemoved(string deviceId)
    {
        DevicesChanged?.Invoke(this, EventArgs.Empty);
    }

    public void OnDefaultDeviceChanged(DataFlow flow, Role role, string defaultDeviceId)
    {
        if (role is Role.Console or Role.Multimedia)
        {
            DevicesChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public void OnPropertyValueChanged(string pwstrDeviceId, PropertyKey key)
    {
        _ = pwstrDeviceId;
        _ = key;
    }
}

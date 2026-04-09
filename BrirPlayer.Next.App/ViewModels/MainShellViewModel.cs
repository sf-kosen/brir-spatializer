using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Windows.Media;
using BrirSpatializer.Audio;
using BrirSpatializer.Audio.Presets;
using BrirPlayer.Next.App.Web;

namespace BrirPlayer.Next.App.ViewModels;

public sealed class MainShellViewModel : INotifyPropertyChanged
{
    private readonly DeviceManager _deviceManager = new();
    private readonly AudioSettings _audioSettings = new();
    private Preset _currentPreset = new();
    private string _selectedInputMode = "Virtual Cable";
    private DeviceOption? _selectedInputDevice;
    private DeviceOption? _selectedOutputDevice;
    private string _selectedPresetId = "second-cinema";
    private string _selectedPresetName = "Second Cinema";
    private string _selectedPresetProfile = "Reference";
    private string _selectedPresetSummary = "Balanced room tuning with a stable center image for long listening sessions.";
    private Brush _selectedPresetBrush = CreatePresetBrush("second-cinema");
    private string _selectedModeDescription = GetModeDescription("Virtual Cable");
    private double _seatX = 0.50;
    private double _seatY = 0.50;
    private string _sessionState = "Initializing";
    private bool _exclusiveOutput;
    private bool _volumeMatch = true;
    private double _outputVolume = 1.0;
    private bool _isLiveRunning;
    private LiveTelemetrySnapshot? _telemetry;

    public MainShellViewModel()
    {
        InputModes = new ObservableCollection<string>
        {
            "Virtual Cable",
            "Loopback",
            "File Play",
            "Convert File"
        };

        InputDevices = new ObservableCollection<DeviceOption>();
        OutputDevices = new ObservableCollection<DeviceOption>();
        InputModeChips = new ObservableCollection<ModeChip>();
        SpacePresets = new ObservableCollection<SpacePresetCard>();
        SeatNodes = new ObservableCollection<SeatNode>();
        MetricTiles = new ObservableCollection<MetricTile>();
        Diagnostics = new ObservableCollection<StatusLine>();

        RebuildModeChips();
        RebuildPresetCards();
        RebuildSeatNodes();
        RefreshDevices();
        LoadLegacyDefaultPreset();
        RefreshDiagnostics();
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public ObservableCollection<string> InputModes { get; }
    public ObservableCollection<DeviceOption> InputDevices { get; }
    public ObservableCollection<DeviceOption> OutputDevices { get; }
    public ObservableCollection<ModeChip> InputModeChips { get; }
    public ObservableCollection<SpacePresetCard> SpacePresets { get; }
    public ObservableCollection<SeatNode> SeatNodes { get; }
    public ObservableCollection<MetricTile> MetricTiles { get; }
    public ObservableCollection<StatusLine> Diagnostics { get; }

    public string SelectedInputMode
    {
        get => _selectedInputMode;
        set
        {
            if (SetField(ref _selectedInputMode, value))
            {
                SelectedModeDescription = GetModeDescription(value);
                RebuildModeChips();
                OnPropertyChanged(nameof(CanStartLive));
                RefreshDiagnostics();
            }
        }
    }

    public DeviceOption? SelectedInputDevice
    {
        get => _selectedInputDevice;
        set
        {
            if (SetField(ref _selectedInputDevice, value))
            {
                OnPropertyChanged(nameof(CanStartLive));
                RefreshDiagnostics();
            }
        }
    }

    public DeviceOption? SelectedOutputDevice
    {
        get => _selectedOutputDevice;
        set
        {
            if (SetField(ref _selectedOutputDevice, value))
            {
                SyncOutputVolumeFromSelection();
                OnPropertyChanged(nameof(CanStartLive));
                RefreshDiagnostics();
            }
        }
    }

    public string SelectedPresetId
    {
        get => _selectedPresetId;
        private set => SetField(ref _selectedPresetId, value);
    }

    public string SelectedPresetName
    {
        get => _selectedPresetName;
        private set
        {
            if (SetField(ref _selectedPresetName, value))
            {
                RefreshDiagnostics();
            }
        }
    }

    public string SelectedPresetProfile
    {
        get => _selectedPresetProfile;
        private set => SetField(ref _selectedPresetProfile, value);
    }

    public string SelectedPresetSummary
    {
        get => _selectedPresetSummary;
        private set => SetField(ref _selectedPresetSummary, value);
    }

    public Brush SelectedPresetBrush
    {
        get => _selectedPresetBrush;
        private set => SetField(ref _selectedPresetBrush, value);
    }

    public string SelectedModeDescription
    {
        get => _selectedModeDescription;
        private set => SetField(ref _selectedModeDescription, value);
    }

    public double SeatX
    {
        get => _seatX;
        set
        {
            if (SetField(ref _seatX, value))
            {
                ApplySeatToPreset();
            }
        }
    }

    public double SeatY
    {
        get => _seatY;
        set
        {
            if (SetField(ref _seatY, value))
            {
                ApplySeatToPreset();
            }
        }
    }

    public string SessionState
    {
        get => _sessionState;
        private set => SetField(ref _sessionState, value);
    }

    public bool ExclusiveOutput
    {
        get => _exclusiveOutput;
        set
        {
            if (SetField(ref _exclusiveOutput, value))
            {
                RefreshDiagnostics();
            }
        }
    }

    public bool VolumeMatch
    {
        get => _volumeMatch;
        set
        {
            if (SetField(ref _volumeMatch, value))
            {
                RefreshDiagnostics();
            }
        }
    }

    public double OutputVolume
    {
        get => _outputVolume;
        set
        {
            double clamped = Math.Clamp(value, 0.0, 1.0);
            if (SetField(ref _outputVolume, clamped))
            {
                ApplyOutputVolume();
                RefreshDiagnostics();
            }
        }
    }

    public bool IsLiveRunning
    {
        get => _isLiveRunning;
        private set
        {
            if (SetField(ref _isLiveRunning, value))
            {
                OnPropertyChanged(nameof(StartButtonText));
                OnPropertyChanged(nameof(CanStartLive));
                OnPropertyChanged(nameof(SessionBadge));
            }
        }
    }

    public string StartButtonText => IsLiveRunning ? "Stop Live" : "Start Live";

    public bool CanStartLive => IsLiveRunning || (
        string.Equals(SelectedInputMode, "Virtual Cable", StringComparison.Ordinal) &&
        SelectedInputDevice is not null &&
        SelectedOutputDevice is not null);

    public string SessionBadge => IsLiveRunning ? "Live On" : "Idle";

    public string SeatLabel => $"Seat {Math.Clamp((int)Math.Round(SeatY * 4.0), 0, 4)}:{Math.Clamp((int)Math.Round(SeatX * 4.0), 0, 4)}";

    public void RefreshDevices()
    {
        string? currentInputId = SelectedInputDevice?.Id;
        string? currentOutputId = SelectedOutputDevice?.Id;

        InputDevices.Clear();
        foreach (AudioDeviceInfo device in _deviceManager.GetInputDevices())
        {
            int sampleRate = _deviceManager.TryGetDevice(device.Id)?.AudioClient.MixFormat.SampleRate ?? 0;
            int channels = _deviceManager.TryGetDevice(device.Id)?.AudioClient.MixFormat.Channels ?? 0;
            InputDevices.Add(new DeviceOption(device.Id, device.Name, $"{sampleRate / 1000.0:F1} kHz / {channels} ch"));
        }

        OutputDevices.Clear();
        foreach (AudioDeviceInfo device in _deviceManager.GetOutputDevices())
        {
            int sampleRate = _deviceManager.TryGetDevice(device.Id)?.AudioClient.MixFormat.SampleRate ?? 0;
            int channels = _deviceManager.TryGetDevice(device.Id)?.AudioClient.MixFormat.Channels ?? 0;
            OutputDevices.Add(new DeviceOption(device.Id, device.Name, $"{sampleRate / 1000.0:F1} kHz / {channels} ch"));
        }

        SelectedInputDevice = InputDevices.FirstOrDefault(device => device.Id == currentInputId) ?? InputDevices.FirstOrDefault();
        SelectedOutputDevice = OutputDevices.FirstOrDefault(device => device.Id == currentOutputId) ?? OutputDevices.FirstOrDefault();
        RefreshDiagnostics();
    }

    public bool LoadLegacyDefaultPreset()
    {
        string path = GetLegacyLastSettingsPath();
        if (!File.Exists(path))
        {
            SessionState = "Legacy preset not found";
            return false;
        }

        LegacySettingsEnvelope? envelope = JsonSerializer.Deserialize<LegacySettingsEnvelope>(File.ReadAllText(path));
        if (envelope?.Preset is null)
        {
            SessionState = "Legacy preset parse failed";
            return false;
        }

        _currentPreset = envelope.Preset;
        VolumeMatch = envelope.Preset.Controls.VolumeMatch;
        ExclusiveOutput = false;
        SelectPreset("second-cinema");
        SeatX = envelope.Preset.Slots.A.Spatializer.SeatX;
        SeatY = envelope.Preset.Slots.A.Spatializer.SeatY;

        if (!string.IsNullOrWhiteSpace(envelope.InputDeviceId))
        {
            SelectedInputDevice = InputDevices.FirstOrDefault(device => device.Id == envelope.InputDeviceId) ?? SelectedInputDevice;
        }

        if (!string.IsNullOrWhiteSpace(envelope.OutputDeviceId))
        {
            SelectedOutputDevice = OutputDevices.FirstOrDefault(device => device.Id == envelope.OutputDeviceId) ?? SelectedOutputDevice;
        }

        SessionState = "Legacy preset loaded";
        RefreshDiagnostics();
        return true;
    }

    public bool TryConvertFile(string inputPath, string outputPath, out string message)
    {
        if (SelectedOutputDevice is null)
        {
            message = "Select an output device first.";
            return false;
        }

        int sampleRate = _deviceManager.TryGetDevice(SelectedOutputDevice.Id)?.AudioClient.MixFormat.SampleRate ?? 0;
        if (sampleRate <= 0)
        {
            message = "Failed to resolve output device sample rate.";
            return false;
        }

        try
        {
            ApplySeatToPreset();
            var renderer = new OfflineRenderer(_audioSettings);
            renderer.Render(inputPath, outputPath, _currentPreset, sampleRate);
            SessionState = "Convert completed";
            RefreshDiagnostics();
            message = $"Converted file written to {outputPath}";
            return true;
        }
        catch (Exception ex)
        {
            SessionState = "Convert failed";
            RefreshDiagnostics(errorOverride: ex.Message);
            message = ex.Message;
            return false;
        }
    }

    public void SelectPreset(string presetId)
    {
        (SelectedPresetId, SelectedPresetName, SelectedPresetProfile, SelectedPresetSummary) = presetId switch
        {
            "live-k-arena" => (
                "live-k-arena",
                "Live K Arena",
                "Wide",
                "Larger venue perspective with more width and bloom for live footage."
            ),
            "studio-near" => (
                "studio-near",
                "Studio Near",
                "Nearfield",
                "Dry, tighter staging for dialogue checks and close monitoring."
            ),
            _ => (
                "second-cinema",
                "Second Cinema",
                "Reference",
                "Balanced room tuning with a stable center image for long listening sessions."
            )
        };

        SelectedPresetBrush = CreatePresetBrush(SelectedPresetId);
        RebuildPresetCards();
        RefreshDiagnostics();
    }

    public void SelectSeat(int row, int column)
    {
        SeatX = column / 4.0;
        SeatY = row / 4.0;
    }

    public void SetLiveState(bool isRunning, string status, string? error = null)
    {
        IsLiveRunning = isRunning;
        SessionState = string.IsNullOrWhiteSpace(error) ? status : $"{status}: {error}";
        RefreshDiagnostics(error);
    }

    public void SetStatusMessage(string status, string? error = null)
    {
        SessionState = string.IsNullOrWhiteSpace(error) ? status : $"{status}: {error}";
        RefreshDiagnostics(error);
    }

    internal void UpdateTelemetry(LiveTelemetrySnapshot telemetry)
    {
        _telemetry = telemetry;
        RefreshDiagnostics(telemetry.SessionError);
    }

    private void ApplySeatToPreset()
    {
        _currentPreset.Slots.A.Spatializer.SeatX = (float)SeatX;
        _currentPreset.Slots.A.Spatializer.SeatY = (float)SeatY;
        RebuildSeatNodes();
        OnPropertyChanged(nameof(SeatLabel));
        RefreshDiagnostics();
    }

    private void ApplyOutputVolume()
    {
        if (SelectedOutputDevice is null)
        {
            return;
        }

        var device = _deviceManager.TryGetDevice(SelectedOutputDevice.Id);
        if (device?.AudioEndpointVolume is null)
        {
            return;
        }

        device.AudioEndpointVolume.MasterVolumeLevelScalar = (float)OutputVolume;
    }

    private void SyncOutputVolumeFromSelection()
    {
        if (SelectedOutputDevice is null)
        {
            return;
        }

        var device = _deviceManager.TryGetDevice(SelectedOutputDevice.Id);
        if (device?.AudioEndpointVolume is null)
        {
            return;
        }

        _outputVolume = Math.Clamp(device.AudioEndpointVolume.MasterVolumeLevelScalar, 0.0f, 1.0f);
        OnPropertyChanged(nameof(OutputVolume));
    }

    private void RebuildPresetCards()
    {
        SpacePresets.Clear();
        SpacePresets.Add(new SpacePresetCard(
            "second-cinema",
            "Second Cinema",
            "Reference",
            "Balanced room tuning with a stable center image and calm low-end shape.",
            SelectedPresetId == "second-cinema"));
        SpacePresets.Add(new SpacePresetCard(
            "live-k-arena",
            "Live K Arena",
            "Wide",
            "Larger venue perspective with more width and audience bloom.",
            SelectedPresetId == "live-k-arena"));
        SpacePresets.Add(new SpacePresetCard(
            "studio-near",
            "Studio Near",
            "Nearfield",
            "Dry, tighter staging for dialogue checks and nearfield judgment.",
            SelectedPresetId == "studio-near"));
    }

    private void RebuildModeChips()
    {
        InputModeChips.Clear();
        foreach (string mode in InputModes)
        {
            InputModeChips.Add(new ModeChip(mode, string.Equals(mode, SelectedInputMode, StringComparison.Ordinal)));
        }
    }

    private void RebuildSeatNodes()
    {
        SeatNodes.Clear();
        int activeRow = Math.Clamp((int)Math.Round(SeatY * 4.0), 0, 4);
        int activeCol = Math.Clamp((int)Math.Round(SeatX * 4.0), 0, 4);
        foreach (int index in Enumerable.Range(0, 25))
        {
            int row = index / 5;
            int col = index % 5;
            SeatNodes.Add(new SeatNode(row, col, $"{row}:{col}", row == activeRow && col == activeCol));
        }
    }

    private void RefreshDiagnostics(string? errorOverride = null)
    {
        Diagnostics.Clear();
        Diagnostics.Add(new StatusLine("Mode", SelectedInputMode));
        Diagnostics.Add(new StatusLine("Input", SelectedInputDevice?.Name ?? "Not selected"));
        Diagnostics.Add(new StatusLine("Output", SelectedOutputDevice?.Name ?? "Not selected"));
        Diagnostics.Add(new StatusLine("Preset", SelectedPresetName));
        Diagnostics.Add(new StatusLine("Seat", $"X={SeatX:F2}, Y={SeatY:F2}"));
        Diagnostics.Add(new StatusLine("Status", errorOverride ?? SessionState));

        LiveTelemetrySnapshot? telemetry = _telemetry;
        if (telemetry is { } live)
        {
            Diagnostics.Add(new StatusLine("Buffer", $"{live.Status.BufferLevelMs:F1} ms"));
            Diagnostics.Add(new StatusLine("Slip", live.Status.SlipCount.ToString()));
            Diagnostics.Add(new StatusLine("In Peak", $"{ToDb(live.Status.BrirInputPeak):F1} dBFS"));
            Diagnostics.Add(new StatusLine("Out Peak", $"{ToDb(live.Status.BrirOutputPeak):F1} dBFS"));
            Diagnostics.Add(new StatusLine("RMS", $"{ToDb(live.Meter.Rms):F1} dBFS"));
            Diagnostics.Add(new StatusLine("Clip", live.Meter.ClipCount.ToString()));
            if (!string.IsNullOrWhiteSpace(live.EngineError))
            {
                Diagnostics.Add(new StatusLine("Engine", live.EngineError));
            }
        }

        MetricTiles.Clear();
        if (telemetry is { } metric)
        {
            MetricTiles.Add(new MetricTile("Buffer", $"{metric.Status.BufferLevelMs:F1} ms", Math.Clamp(metric.Status.BufferLevelMs / 250.0, 0.0, 1.0)));
            MetricTiles.Add(new MetricTile("Slip", metric.Status.SlipCount.ToString(), Math.Clamp(metric.Status.SlipCount / 8.0, 0.0, 1.0)));
            MetricTiles.Add(new MetricTile("Out Peak", $"{ToDb(metric.Status.BrirOutputPeak):F1} dBFS", Math.Clamp(metric.Status.BrirOutputPeak, 0.0, 1.0)));
            MetricTiles.Add(new MetricTile("RMS", $"{ToDb(metric.Meter.Rms):F1} dBFS", Math.Clamp(metric.Meter.Rms * 4.0, 0.0, 1.0)));
            MetricTiles.Add(new MetricTile("Clip", metric.Meter.ClipCount.ToString(), metric.Meter.ClipCount > 0 ? 1.0 : 0.04));
            MetricTiles.Add(new MetricTile("Live", IsLiveRunning ? "running" : "idle", IsLiveRunning ? 0.92 : 0.24));
            return;
        }

        MetricTiles.Add(new MetricTile("Input", SelectedInputDevice?.Details ?? "-", 1.00));
        MetricTiles.Add(new MetricTile("Output", SelectedOutputDevice?.Details ?? "-", 1.00));
        MetricTiles.Add(new MetricTile("Output Vol", $"{OutputVolume * 100:F0}%", OutputVolume));
        MetricTiles.Add(new MetricTile("Seat X", $"{SeatX:F2}", SeatX));
        MetricTiles.Add(new MetricTile("Seat Y", $"{SeatY:F2}", SeatY));
        MetricTiles.Add(new MetricTile("Live", IsLiveRunning ? "running" : "idle", IsLiveRunning ? 0.92 : 0.24));
    }

    private static float ToDb(float linear)
    {
        if (linear <= 1e-6f)
        {
            return -120.0f;
        }

        return 20.0f * (float)Math.Log10(linear);
    }

    private static string GetModeDescription(string mode)
    {
        return mode switch
        {
            "Loopback" => "The app listens to an existing render device and monitors it through a second output.",
            "File Play" => "Local files are decoded and auditioned through the same spatial processing chain.",
            "Convert File" => "Offline export keeps the BRIR path intact while writing a clean wav render.",
            _ => "Browser audio enters the app through the virtual input, then leaves through your listening device."
        };
    }

    private static Brush CreatePresetBrush(string presetId)
    {
        GradientStopCollection stops = presetId switch
        {
            "live-k-arena" => new GradientStopCollection
            {
                new GradientStop(Color.FromRgb(8, 8, 8), 0.0),
                new GradientStop(Color.FromRgb(32, 32, 32), 0.38),
                new GradientStop(Color.FromRgb(116, 116, 116), 1.0)
            },
            "studio-near" => new GradientStopCollection
            {
                new GradientStop(Color.FromRgb(19, 19, 19), 0.0),
                new GradientStop(Color.FromRgb(44, 44, 44), 0.5),
                new GradientStop(Color.FromRgb(98, 98, 98), 1.0)
            },
            _ => new GradientStopCollection
            {
                new GradientStop(Color.FromRgb(16, 16, 16), 0.0),
                new GradientStop(Color.FromRgb(53, 53, 53), 0.45),
                new GradientStop(Color.FromRgb(138, 138, 138), 1.0)
            }
        };

        return new LinearGradientBrush(stops, 135.0);
    }

    private static string GetLegacyLastSettingsPath()
    {
        return Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "..", "..", "..", "..",
            "BrirSpatializer.App",
            "bin",
            "Debug",
            "net8.0-windows",
            "Settings",
            "last_settings.json"));
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}

public sealed record DeviceOption(string Id, string Name, string Details)
{
    public override string ToString() => Name;
}

public sealed record ModeChip(string Name, bool IsActive);

public sealed record SpacePresetCard(string Id, string Name, string Profile, string Summary, bool IsActive);

public sealed record SeatNode(int Row, int Column, string Label, bool IsActive);

public sealed record MetricTile(string Label, string Value, double Fill);

public sealed record StatusLine(string Label, string Value);

file sealed class LegacySettingsEnvelope
{
    public string? InputDeviceId { get; set; }
    public string? OutputDeviceId { get; set; }
    public Preset? Preset { get; set; }
}

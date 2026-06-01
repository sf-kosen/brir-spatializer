using System.IO;
using System.Text.Json;
using BrirSpatializer.Audio;
using BrirSpatializer.Audio.Dsp;
using BrirSpatializer.Audio.Dsp.Processors;
using BrirSpatializer.Audio.Ir;
using BrirSpatializer.Audio.Presets;
using NAudio.CoreAudioApi;

namespace BrirPlayer.Next.App.Web;

internal sealed class LivePlaybackSession : IDisposable
{
    private readonly AudioSettings _audioSettings = new();
    private readonly AudioEngine _engine;
    private readonly DeviceManager _deviceManager = new();
    private readonly Dictionary<string, BrirSet> _brirSetCache = new(StringComparer.OrdinalIgnoreCase);
    private readonly Preset _legacyPreset;
    private PlaybackRequest? _activeRequest;

    public LivePlaybackSession()
    {
        _engine = new AudioEngine(_audioSettings);
        _legacyPreset = LoadLegacyPreset();
        ApplyAudioSettings(_legacyPreset);
        ApplyControlSettings(_legacyPreset.Controls, volumeMatchOverride: _legacyPreset.Controls.VolumeMatch);
        SessionStatus = "Idle";
    }

    public bool IsRunning => _engine.IsRunning;
    public string SessionStatus { get; private set; }
    public string? LastError { get; private set; }

    public void StartOrRestart(BridgeRouteState route, PlaybackRequest request)
    {
        if (!string.Equals(request.Mode, "Virtual Cable", StringComparison.Ordinal))
        {
            throw new InvalidOperationException($"{request.Mode} live mode is not implemented yet.");
        }

        string inputDeviceId = ResolveInputDeviceId(route);

        if (string.IsNullOrWhiteSpace(inputDeviceId) || string.IsNullOrWhiteSpace(route.OutputId))
        {
            throw new InvalidOperationException("Select both input and output devices.");
        }

        int sampleRate = _engine.TryGetDeviceSampleRate(route.OutputId)
            ?? throw new InvalidOperationException("Failed to resolve output device sample rate.");

        if (_engine.IsRunning)
        {
            _engine.Stop();
        }

        ConfigurePreset(route, request, sampleRate);
        try
        {
            _engine.Start(inputDeviceId, route.OutputId, route.Exclusive);
            LastError = null;
            SessionStatus = route.Exclusive ? "Live On (Exclusive)" : "Live On";
        }
        catch (Exception ex) when (route.Exclusive)
        {
            _engine.Stop();
            _engine.Start(inputDeviceId, route.OutputId, false);
            LastError = $"Exclusive output failed, fell back to shared: {ex.Message}";
            SessionStatus = "Live On (Shared Fallback)";
        }

        _activeRequest = request;
    }

    public void Stop()
    {
        if (_engine.IsRunning)
        {
            _engine.Stop();
        }

        _activeRequest = null;
        LastError = null;
        SessionStatus = "Idle";
    }

    public void UpdateSeat(double seatX, double seatY)
    {
        if (_activeRequest is null)
        {
            return;
        }

        _activeRequest = _activeRequest with { SeatX = seatX, SeatY = seatY };
        if (!_engine.IsRunning)
        {
            return;
        }

        PresetNormalize normalize = ResolveNormalize(_activeRequest.PresetId);
        _engine.SetSeat(
            ActiveSlot.A,
            (float)seatX,
            (float)seatY,
            normalize.Mode,
            normalize.TargetPeakDbFS,
            normalize.TargetRmsDbFS,
            _legacyPreset.Controls.IrSwitchXfadeMs);
    }

    public void UpdateVolumeMatch(bool enabled)
    {
        ApplyControlSettings(_legacyPreset.Controls, enabled);
    }

    public LiveTelemetrySnapshot GetTelemetry()
    {
        EngineStatus status = _engine.Status;
        MeterSnapshot meter = _engine.MeterSnapshot;
        string? engineError = _engine.LastError;
        if (!string.IsNullOrWhiteSpace(engineError))
        {
            LastError = engineError;
        }

        return new LiveTelemetrySnapshot(
            status,
            meter,
            LastError,
            engineError);
    }

    public PlaybackRequest? GetActiveRequest() => _activeRequest;

    public void ReportFailure(Exception ex)
    {
        LastError = ex.Message;
        SessionStatus = "Error";
    }

    public void Dispose()
    {
        _engine.Dispose();
    }

    private void ConfigurePreset(BridgeRouteState route, PlaybackRequest request, int sampleRate)
    {
        ApplyAudioSettings(_legacyPreset);
        ApplyControlSettings(_legacyPreset.Controls, route.VolumeMatch);

        PresetNormalize normalize = ResolveNormalize(request.PresetId);
        BrirSet set = ResolveBrirSet(request.PresetId, sampleRate);

        _engine.SetBrirSet(ActiveSlot.A, set);
        _engine.SetSeat(
            ActiveSlot.A,
            (float)request.SeatX,
            (float)request.SeatY,
            normalize.Mode,
            normalize.TargetPeakDbFS,
            normalize.TargetRmsDbFS,
            _legacyPreset.Controls.IrSwitchXfadeMs);
        _engine.SetSpatializerMode(ActiveSlot.A, SpatializerMode.Brir);
        _engine.SetSpatializerMode(ActiveSlot.B, SpatializerMode.Off);
    }

    private void ApplyAudioSettings(Preset preset)
    {
        // Prioritize stability over latency for browser listening.
        // The current live issue is dominated by aggressive slip corrections,
        // so keep a larger steady buffer and avoid time-domain edits.
        _audioSettings.BlockSize = 2048;
        _audioSettings.DriftCompensationMode = DriftCompensationMode.Strict;
        _audioSettings.DriftTargetBufferMs = Math.Max(180, preset.Audio.DriftTargetBufferMs);
        _audioSettings.LiveTrimDb = -4.0f;
        _audioSettings.EnableSpectrumAnalysis = false;
        _audioSettings.EnableLimiter = true;
        _audioSettings.DriftSlip.HiWaterMs = Math.Max(_audioSettings.DriftTargetBufferMs + 70, preset.Audio.DriftSlip.HiWaterMs);
        _audioSettings.DriftSlip.LoWaterMs = Math.Min(_audioSettings.DriftTargetBufferMs - 50, Math.Max(90, preset.Audio.DriftSlip.LoWaterMs));
        _audioSettings.DriftSlip.XfadeMs = Math.Max(24, preset.Audio.DriftSlip.XfadeMs);
        _audioSettings.Startup.MaxPrerollMs = 12000;
        _audioSettings.Startup.FadeInMs = 60;
    }

    private void ApplyControlSettings(PresetControls controls, bool volumeMatchOverride)
    {
        _engine.UpdateControls(new ControlSettings
        {
            Bypass = controls.Bypass,
            ActiveSlot = ActiveSlot.A,
            VolumeMatch = volumeMatchOverride,
            XfadeMs = controls.XfadeMs,
            IrSwitchXfadeMs = controls.IrSwitchXfadeMs
        });
    }

    private PresetNormalize ResolveNormalize(string presetId)
    {
        return presetId switch
        {
            "live-k-arena" => new PresetNormalize
            {
                Mode = NormalizeMode.Peak,
                TargetPeakDbFS = -24.0f,
                TargetRmsDbFS = -26.0f
            },
            "studio-near" => new PresetNormalize
            {
                Mode = NormalizeMode.Peak,
                TargetPeakDbFS = -22.0f,
                TargetRmsDbFS = -24.0f
            },
            _ => new PresetNormalize
            {
                Mode = NormalizeMode.Peak,
                TargetPeakDbFS = -20.0f,
                TargetRmsDbFS = -22.0f
            }
        };
    }

    private BrirSet ResolveBrirSet(string presetId, int sampleRate)
    {
        string cacheKey = $"{presetId}|{sampleRate}";
        if (_brirSetCache.TryGetValue(cacheKey, out BrirSet? cached))
        {
            return cached;
        }

        PresetBrirSet definition = ResolveBrirSetDefinition(presetId);
        BrirSet loaded = LoadBrirSet(definition, sampleRate);
        _brirSetCache[cacheKey] = loaded;
        return loaded;
    }

    private PresetBrirSet ResolveBrirSetDefinition(string presetId)
    {
        if (presetId == "live-k-arena")
        {
            string liveGridDir = Path.Combine(GetAssetsRoot(), "Live_K_Arena_Grid_5x5");
            if (Directory.Exists(liveGridDir))
            {
                return BuildGridDefinition(liveGridDir, 5, 5);
            }
        }

        if (presetId == "studio-near")
        {
            return _legacyPreset.Slots.A.Spatializer.BrirSet
                ?? throw new InvalidOperationException("Legacy BRIR set is missing.");
        }

        return _legacyPreset.Slots.A.Spatializer.BrirSet
            ?? throw new InvalidOperationException("Legacy BRIR set is missing.");
    }

    private static PresetBrirSet BuildGridDefinition(string directory, int rows, int columns)
    {
        var paths = new List<string>(rows * columns);
        for (int row = 0; row < rows; row++)
        {
            for (int col = 0; col < columns; col++)
            {
                string path = Path.Combine(directory, $"Seat_r{row}_c{col}.wav");
                if (!File.Exists(path))
                {
                    throw new FileNotFoundException("BRIR grid file not found.", path);
                }

                paths.Add(path);
            }
        }

        return new PresetBrirSet
        {
            Rows = rows,
            Columns = columns,
            Paths = paths
        };
    }

    private static BrirSet LoadBrirSet(PresetBrirSet preset, int sampleRate)
    {
        if (preset.Paths.Count != preset.Rows * preset.Columns)
        {
            throw new InvalidOperationException("BRIR set paths count mismatch.");
        }

        int maxFrames = (int)Math.Round(sampleRate * 2.0);
        var loader = new IrLoader(maxFrames);
        var list = new List<BrirIr>(preset.Paths.Count);
        foreach (string path in preset.Paths)
        {
            IrLoadResult load = loader.LoadBrir(path, sampleRate);
            if (!load.Success || load.Ir is null)
            {
                throw new InvalidOperationException(load.Message ?? $"Failed to load BRIR: {path}");
            }

            list.Add(load.Ir);
        }

        return new BrirSet(preset.Rows, preset.Columns, list);
    }

    private static Preset LoadLegacyPreset()
    {
        string path = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "..", "..", "..", "..",
            "BrirSpatializer.App",
            "bin",
            "Debug",
            "net8.0-windows",
            "Settings",
            "last_settings.json"));

        if (!File.Exists(path))
        {
            throw new FileNotFoundException("Legacy settings file was not found.", path);
        }

        LegacySettingsEnvelope? envelope = JsonSerializer.Deserialize<LegacySettingsEnvelope>(File.ReadAllText(path));
        return envelope?.Preset ?? throw new InvalidOperationException("Legacy preset was not found.");
    }

    private static string GetAssetsRoot()
    {
        return Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "..", "..", "..", "..",
            "Assets"));
    }

    private string ResolveInputDeviceId(BridgeRouteState route)
    {
        if (!string.Equals(route.Mode, "Virtual Cable", StringComparison.Ordinal))
        {
            return route.InputId;
        }

        var selected = _deviceManager.TryGetDevice(route.InputId);
        if (selected is not null && selected.DataFlow == DataFlow.Render)
        {
            return route.InputId;
        }

        AudioDeviceInfo? virtualRender = _deviceManager.GetOutputDevices()
            .FirstOrDefault(device =>
                device.Name.Contains("CABLE Input", StringComparison.OrdinalIgnoreCase) &&
                device.Name.Contains("VB-Audio Virtual Cable", StringComparison.OrdinalIgnoreCase));

        return virtualRender?.Id ?? route.InputId;
    }

    private sealed class LegacySettingsEnvelope
    {
        public Preset? Preset { get; set; }
    }
}

internal readonly record struct LiveTelemetrySnapshot(
    EngineStatus Status,
    MeterSnapshot Meter,
    string? SessionError,
    string? EngineError);

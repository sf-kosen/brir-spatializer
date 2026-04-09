using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows.Threading;
using BrirSpatializer.Audio;
using BrirSpatializer.Audio.Dsp;
using BrirSpatializer.Audio.Dsp.Processors;
using BrirSpatializer.Audio.Ir;
using BrirSpatializer.Audio.Presets;

namespace BrirSpatializer.App;

public sealed class MainViewModel : INotifyPropertyChanged, IDisposable
{
    private static readonly Regex SeatFileRegex = new(@"Seat_r(?<row>\d+)_c(?<col>\d+)\.wav$", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private readonly AudioEngine _engine;
    private readonly DispatcherTimer _statusTimer;
    private AudioDeviceInfo? _selectedInput;
    private AudioDeviceInfo? _selectedOutput;
    private bool _isRunning;
    private bool _bypass;
    private bool _volumeMatch = true;
    private ActiveSlot _activeSlot = ActiveSlot.A;
    private string _statusText = "Stopped";
    private string _meterText = "RMS: -inf dB, Peak: -inf dB";
    private PresetSlot _slotAState = new();
    private PresetSlot _slotBState = new();
    private readonly PresetStore _presetStore = new();
    private readonly AudioSettings _settings = new();
    private int _xfadeMs = 50;
    private int _irSwitchXfadeMs = 100;
    private string _slotABrirPath = string.Empty;
    private string _slotBBrirPath = string.Empty;
    private NormalizeMode _slotANormalizeMode = NormalizeMode.Peak;
    private NormalizeMode _slotBNormalizeMode = NormalizeMode.Peak;
    private float _slotATargetPeakDbFs = -6.0f;
    private float _slotBTargetPeakDbFs = -6.0f;
    private float _slotATargetRmsDbFs = -18.0f;
    private float _slotBTargetRmsDbFs = -18.0f;
    private bool _slotABrirEnabled;
    private bool _slotBBrirEnabled;
    private float _slotASeatX = 0.5f;
    private float _slotASeatY = 0.5f;
    private float _slotBSeatX = 0.5f;
    private float _slotBSeatY = 0.5f;
    private BrirSet? _slotABrirSet;
    private BrirSet? _slotBBrirSet;
    private int _slotARows = 1;
    private int _slotACols = 1;
    private int _slotBRows = 1;
    private int _slotBCols = 1;
    private List<string> _slotABrirSetPaths = new();
    private List<string> _slotBBrirSetPaths = new();
    private float _vuDb = -120.0f;
    private float _peakDb = -120.0f;
    private float _peakHoldDb = -120.0f;
    private float _truePeakDb = -120.0f;
    private float _truePeakHoldDb = -120.0f;
    private int _clipCount;
    private IReadOnlyList<VUMeterTick> _vuTicks = Array.Empty<VUMeterTick>();
    private IReadOnlyList<float> _spectrumBands = Array.Empty<float>();

    public MainViewModel()
    {
        _engine = new AudioEngine(_settings);
        InputDevices = new ObservableCollection<AudioDeviceInfo>(_engine.GetInputDevices());
        OutputDevices = new ObservableCollection<AudioDeviceInfo>(_engine.GetOutputDevices());
        SelectedInput = InputDevices.FirstOrDefault();
        SelectedOutput = OutputDevices.FirstOrDefault();

        _statusTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(16), DispatcherPriority.Background, (_, _) => UpdateStatus(), Dispatcher.CurrentDispatcher);
        _statusTimer.Start();
        ApplyControls();
        BuildVUTicks();
        LoadLastSettings();
    }

    public ObservableCollection<AudioDeviceInfo> InputDevices { get; }
    public ObservableCollection<AudioDeviceInfo> OutputDevices { get; }
    public IReadOnlyList<ActiveSlot> ActiveSlots { get; } = new[] { ActiveSlot.A, ActiveSlot.B };
    public IReadOnlyList<NormalizeMode> NormalizeModes { get; } = Enum.GetValues<NormalizeMode>();

    public string SlotABrirPath
    {
        get => _slotABrirPath;
        set => SetField(ref _slotABrirPath, value);
    }

    public string SlotBBrirPath
    {
        get => _slotBBrirPath;
        set => SetField(ref _slotBBrirPath, value);
    }

    public NormalizeMode SlotANormalizeMode
    {
        get => _slotANormalizeMode;
        set => SetField(ref _slotANormalizeMode, value);
    }

    public NormalizeMode SlotBNormalizeMode
    {
        get => _slotBNormalizeMode;
        set => SetField(ref _slotBNormalizeMode, value);
    }

    public float SlotATargetPeakDbFs
    {
        get => _slotATargetPeakDbFs;
        set => SetField(ref _slotATargetPeakDbFs, value);
    }

    public float SlotBTargetPeakDbFs
    {
        get => _slotBTargetPeakDbFs;
        set => SetField(ref _slotBTargetPeakDbFs, value);
    }

    public float SlotATargetRmsDbFs
    {
        get => _slotATargetRmsDbFs;
        set => SetField(ref _slotATargetRmsDbFs, value);
    }

    public float SlotBTargetRmsDbFs
    {
        get => _slotBTargetRmsDbFs;
        set => SetField(ref _slotBTargetRmsDbFs, value);
    }

    public bool SlotABrirEnabled
    {
        get => _slotABrirEnabled;
        set
        {
            if (SetField(ref _slotABrirEnabled, value))
            {
                _engine.SetSpatializerMode(ActiveSlot.A, value ? SpatializerMode.Brir : SpatializerMode.Off);
            }
        }
    }

    public bool SlotBBrirEnabled
    {
        get => _slotBBrirEnabled;
        set
        {
            if (SetField(ref _slotBBrirEnabled, value))
            {
                _engine.SetSpatializerMode(ActiveSlot.B, value ? SpatializerMode.Brir : SpatializerMode.Off);
            }
        }
    }

    public float SlotASeatX
    {
        get => _slotASeatX;
        set => SetField(ref _slotASeatX, value);
    }

    public float SlotASeatY
    {
        get => _slotASeatY;
        set => SetField(ref _slotASeatY, value);
    }

    public float SlotBSeatX
    {
        get => _slotBSeatX;
        set => SetField(ref _slotBSeatX, value);
    }

    public float SlotBSeatY
    {
        get => _slotBSeatY;
        set => SetField(ref _slotBSeatY, value);
    }

    public int SlotARows
    {
        get => _slotARows;
        set => SetField(ref _slotARows, Math.Max(1, value));
    }

    public int SlotACols
    {
        get => _slotACols;
        set => SetField(ref _slotACols, Math.Max(1, value));
    }

    public int SlotBRows
    {
        get => _slotBRows;
        set => SetField(ref _slotBRows, Math.Max(1, value));
    }

    public int SlotBCols
    {
        get => _slotBCols;
        set => SetField(ref _slotBCols, Math.Max(1, value));
    }

    public AudioDeviceInfo? SelectedInput
    {
        get => _selectedInput;
        set => SetField(ref _selectedInput, value);
    }

    public AudioDeviceInfo? SelectedOutput
    {
        get => _selectedOutput;
        set => SetField(ref _selectedOutput, value);
    }

    public bool IsRunning
    {
        get => _isRunning;
        private set => SetField(ref _isRunning, value);
    }

    public bool Bypass
    {
        get => _bypass;
        set
        {
            if (SetField(ref _bypass, value)) ApplyControls();
        }
    }

    public bool VolumeMatch
    {
        get => _volumeMatch;
        set
        {
            if (SetField(ref _volumeMatch, value)) ApplyControls();
        }
    }

    public ActiveSlot ActiveSlot
    {
        get => _activeSlot;
        set
        {
            if (SetField(ref _activeSlot, value)) ApplyControls();
        }
    }

    public string StatusText
    {
        get => _statusText;
        private set => SetField(ref _statusText, value);
    }

    public string MeterText
    {
        get => _meterText;
        private set => SetField(ref _meterText, value);
    }

    public float VuDb
    {
        get => _vuDb;
        private set => SetField(ref _vuDb, value);
    }

    public float PeakDb
    {
        get => _peakDb;
        private set => SetField(ref _peakDb, value);
    }

    public float PeakHoldDb
    {
        get => _peakHoldDb;
        private set => SetField(ref _peakHoldDb, value);
    }

    public float TruePeakDb
    {
        get => _truePeakDb;
        private set => SetField(ref _truePeakDb, value);
    }

    public float TruePeakHoldDb
    {
        get => _truePeakHoldDb;
        private set => SetField(ref _truePeakHoldDb, value);
    }

    public int ClipCount
    {
        get => _clipCount;
        private set => SetField(ref _clipCount, value);
    }

    public IReadOnlyList<VUMeterTick> VUTicks
    {
        get => _vuTicks;
        private set => SetField(ref _vuTicks, value);
    }

    public IReadOnlyList<float> SpectrumBands
    {
        get => _spectrumBands;
        private set => SetField(ref _spectrumBands, value);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public void ToggleStartStop()
    {
        if (IsRunning)
        {
            _engine.Stop();
            IsRunning = false;
            StatusText = "Stopped";
            return;
        }

        if (SelectedInput is null || SelectedOutput is null)
        {
            StatusText = "Select devices.";
            return;
        }

        try
        {
            if (!TryPrepareCurrentPresetForPlayback(out string? prepareMessage))
            {
                StatusText = prepareMessage ?? "Failed to prepare BRIR preset.";
                return;
            }

            _engine.Start(SelectedInput.Id, SelectedOutput.Id);
            IsRunning = true;
            StatusText = "Running";
        }
        catch (Exception ex)
        {
            StatusText = ex.Message;
        }
    }

    public bool TryLoadPreset(string path, out string? message)
    {
        message = null;
        if (IsRunning)
        {
            message = "Stop playback before loading presets.";
            return false;
        }

        Preset preset;
        try
        {
            preset = _presetStore.Load(path);
        }
        catch (Exception ex)
        {
            message = ex.Message;
            return false;
        }

        if (SelectedOutput is null)
        {
            message = "Select output device before loading presets.";
            return false;
        }

        int? sampleRate = _engine.TryGetDeviceSampleRate(SelectedOutput.Id);
        if (!sampleRate.HasValue)
        {
            message = "Failed to resolve output sample rate.";
            return false;
        }

        _settings.DriftTargetBufferMs = preset.Audio.DriftTargetBufferMs;
        _settings.DriftSlip.HiWaterMs = preset.Audio.DriftSlip.HiWaterMs;
        _settings.DriftSlip.LoWaterMs = preset.Audio.DriftSlip.LoWaterMs;
        _settings.DriftSlip.XfadeMs = preset.Audio.DriftSlip.XfadeMs;

        Bypass = preset.Controls.Bypass;
        ActiveSlot = preset.Controls.ActiveSlot;
        VolumeMatch = preset.Controls.VolumeMatch;
        ApplyControlsWithXfade(preset.Controls.XfadeMs, preset.Controls.IrSwitchXfadeMs);

        ApplySlotPreset(ActiveSlot.A, preset.Slots.A, sampleRate.Value, preset.Controls.IrSwitchXfadeMs, ref _slotAState, ref message);
        ApplySlotPreset(ActiveSlot.B, preset.Slots.B, sampleRate.Value, preset.Controls.IrSwitchXfadeMs, ref _slotBState, ref message);

        return true;
    }

    public void SavePreset(string path)
    {
        Preset preset = BuildPresetFromUi();

        _presetStore.Save(path, preset);
    }

    public bool TryExportCurrentPreset(string inputPath, string outputPath, out string? message)
    {
        message = null;
        if (IsRunning)
        {
            message = "Stop playback before offline export.";
            return false;
        }

        if (SelectedOutput is null)
        {
            message = "Select output device before offline export.";
            return false;
        }

        int? sampleRate = _engine.TryGetDeviceSampleRate(SelectedOutput.Id);
        if (!sampleRate.HasValue)
        {
            message = "Failed to resolve output sample rate.";
            return false;
        }

        try
        {
            Preset preset = BuildPresetFromUi();
            var renderer = new OfflineRenderer(_settings);
            renderer.Render(inputPath, outputPath, preset, sampleRate.Value);
            message = $"Exported processed WAV to {outputPath}";
            return true;
        }
        catch (Exception ex)
        {
            message = ex.Message;
            return false;
        }
    }

    public void SetSlotPath(ActiveSlot slot, string path)
    {
        if (slot == ActiveSlot.A) SlotABrirPath = path;
        else SlotBBrirPath = path;
    }

    public bool TryApplyBrir(ActiveSlot slot, out string? message)
    {
        message = null;

        if (SelectedOutput is null)
        {
            message = "Select output device before applying BRIR.";
            return false;
        }

        int? sampleRate = _engine.TryGetDeviceSampleRate(SelectedOutput.Id);
        if (!sampleRate.HasValue)
        {
            message = "Failed to resolve output sample rate.";
            return false;
        }

        PresetSlot slotPreset = BuildSlotPresetFromUi(slot);
        if (slotPreset.Spatializer.Mode == PresetSpatializerMode.Off)
        {
            _engine.SetSpatializerMode(slot, SpatializerMode.Off);
            if (slot == ActiveSlot.A) _slotAState = slotPreset;
            else _slotBState = slotPreset;
            return true;
        }

        if (string.IsNullOrWhiteSpace(slotPreset.Spatializer.BrirPath))
        {
            message = "BRIR path missing.";
            return false;
        }

        int maxFrames = (int)Math.Round(sampleRate.Value * 2.0);
        var loader = new IrLoader(maxFrames);
        IrLoadResult load = loader.LoadBrir(slotPreset.Spatializer.BrirPath, sampleRate.Value);
        if (!load.Success || load.Ir is null)
        {
            message = load.Message ?? "Failed to load IR.";
            return false;
        }

        IrNormalizeResult norm = IrNormalizer.Normalize(
            load.Ir,
            slotPreset.Spatializer.Normalize.Mode,
            slotPreset.Spatializer.Normalize.TargetPeakDbFS,
            slotPreset.Spatializer.Normalize.TargetRmsDbFS);

        if (norm.Warning is not null)
        {
            message = norm.Warning;
        }

        _engine.SetBrir(slot, norm.Ir, _irSwitchXfadeMs);
        _engine.SetSpatializerMode(slot, SpatializerMode.Brir);
        if (slot == ActiveSlot.A) SlotABrirEnabled = true;
        else SlotBBrirEnabled = true;
        if (slot == ActiveSlot.A) _slotAState = slotPreset;
        else _slotBState = slotPreset;
        return true;
    }

    public bool TryApplySeat(ActiveSlot slot, out string? message)
    {
        message = null;
        BrirSet? set = slot == ActiveSlot.A ? _slotABrirSet : _slotBBrirSet;
        if (set is null)
        {
            message = "BRIR set not loaded for this slot.";
            return false;
        }


        float x = slot == ActiveSlot.A ? SlotASeatX : SlotBSeatX;
        float y = slot == ActiveSlot.A ? SlotASeatY : SlotBSeatY;
        NormalizeMode mode = slot == ActiveSlot.A ? SlotANormalizeMode : SlotBNormalizeMode;
        float peak = slot == ActiveSlot.A ? SlotATargetPeakDbFs : SlotBTargetPeakDbFs;
        float rms = slot == ActiveSlot.A ? SlotATargetRmsDbFs : SlotBTargetRmsDbFs;

        _engine.SetBrirSet(slot, set);
        _engine.SetSeat(slot, x, y, mode, peak, rms, _irSwitchXfadeMs);
        return true;
    }

    public bool TryLoadBrirSet(ActiveSlot slot, IReadOnlyList<string> paths, out string? message)
    {
        message = null;
        if (SelectedOutput is null)
        {
            message = "Select output device before loading BRIR set.";
            return false;
        }

        int? sampleRate = _engine.TryGetDeviceSampleRate(SelectedOutput.Id);
        if (!sampleRate.HasValue)
        {
            message = "Failed to resolve output sample rate.";
            return false;
        }

        int rows = slot == ActiveSlot.A ? SlotARows : SlotBRows;
        int cols = slot == ActiveSlot.A ? SlotACols : SlotBCols;
        List<string> orderedPaths = paths.ToList();

        if (TryInferGridLayout(paths, out int inferredRows, out int inferredCols, out List<string>? inferredPaths))
        {
            rows = inferredRows;
            cols = inferredCols;
            orderedPaths = inferredPaths;
            if (slot == ActiveSlot.A)
            {
                SlotARows = rows;
                SlotACols = cols;
            }
            else
            {
                SlotBRows = rows;
                SlotBCols = cols;
            }
        }

        if (orderedPaths.Count != rows * cols)
        {
            message = $"Expected {rows * cols} files for {rows}x{cols} grid.";
            return false;
        }

        var preset = new PresetBrirSet
        {
            Rows = rows,
            Columns = cols,
            Paths = orderedPaths
        };

        BrirSet? set = LoadBrirSet(preset, sampleRate.Value, ref message);
        if (set is null) return false;

        if (slot == ActiveSlot.A)
        {
            _slotABrirSet = set;
            _slotABrirSetPaths = preset.Paths;
        }
        else
        {
            _slotBBrirSet = set;
            _slotBBrirSetPaths = preset.Paths;
        }

        UpdateSlotPresetSet(slot, preset);
        
        // Auto-apply seat to initialize the spatializer
        TryApplySeat(slot, out _);
        
        // Auto-enable the module
        if (slot == ActiveSlot.A) SlotABrirEnabled = true;
        else SlotBBrirEnabled = true;

        return true;
    }

    private bool TryPrepareCurrentPresetForPlayback(out string? message)
    {
        message = null;
        if (SelectedOutput is null)
        {
            message = "Select output device before preparing BRIR.";
            return false;
        }

        int? sampleRate = _engine.TryGetDeviceSampleRate(SelectedOutput.Id);
        if (!sampleRate.HasValue)
        {
            message = "Failed to resolve output sample rate.";
            return false;
        }

        var messages = new List<string>();
        PresetSlot slotAPreset = BuildSlotPresetFromUi(ActiveSlot.A);
        PresetSlot slotBPreset = BuildSlotPresetFromUi(ActiveSlot.B);

        string? slotAMessage = null;
        ApplySlotPreset(ActiveSlot.A, slotAPreset, sampleRate.Value, _irSwitchXfadeMs, ref _slotAState, ref slotAMessage);
        if (!string.IsNullOrWhiteSpace(slotAMessage))
        {
            messages.Add($"Slot A: {slotAMessage}");
        }

        string? slotBMessage = null;
        ApplySlotPreset(ActiveSlot.B, slotBPreset, sampleRate.Value, _irSwitchXfadeMs, ref _slotBState, ref slotBMessage);
        if (!string.IsNullOrWhiteSpace(slotBMessage))
        {
            messages.Add($"Slot B: {slotBMessage}");
        }

        if (messages.Count > 0)
        {
            message = string.Join(" | ", messages);
        }

        return messages.Count == 0;
    }

    private static bool TryInferGridLayout(IReadOnlyList<string> paths, out int rows, out int cols, out List<string> orderedPaths)
    {
        rows = 0;
        cols = 0;
        orderedPaths = new List<string>();
        if (paths.Count == 0) return false;

        var indexed = new Dictionary<(int Row, int Col), string>();
        foreach (string path in paths)
        {
            string fileName = Path.GetFileName(path);
            Match match = SeatFileRegex.Match(fileName);
            if (!match.Success) return false;

            int row = int.Parse(match.Groups["row"].Value);
            int col = int.Parse(match.Groups["col"].Value);
            if (!indexed.TryAdd((row, col), path)) return false;

            rows = Math.Max(rows, row + 1);
            cols = Math.Max(cols, col + 1);
        }

        if (rows * cols != paths.Count) return false;

        for (int row = 0; row < rows; row++)
        {
            for (int col = 0; col < cols; col++)
            {
                if (!indexed.TryGetValue((row, col), out string? path)) return false;
                orderedPaths.Add(path);
            }
        }

        return true;
    }

    private void ApplyControls()
    {
        ApplyControlsWithXfade(_xfadeMs, _irSwitchXfadeMs);
    }

    private void ApplyControlsWithXfade(int xfadeMs, int irSwitchXfadeMs)
    {
        _xfadeMs = xfadeMs;
        _irSwitchXfadeMs = irSwitchXfadeMs;
        _engine.UpdateControls(new ControlSettings
        {
            Bypass = Bypass,
            ActiveSlot = ActiveSlot,
            VolumeMatch = VolumeMatch,
            XfadeMs = _xfadeMs,
            IrSwitchXfadeMs = _irSwitchXfadeMs
        });
    }

    private void UpdateStatus()
    {
        if (!IsRunning)
        {
             if (_engine.LastError is not null)
             {
                 StatusText = $"Error: {_engine.LastError}";
             }
             return;
        }
        EngineStatus status = _engine.Status;
        MeterSnapshot meter = _engine.MeterSnapshot;
        SpectrumSnapshot spectrum = _engine.SpectrumSnapshot;
        
        string error = _engine.LastError is null ? "" : $" | Error: {_engine.LastError}";
        StatusText = $"Buffer: {status.BufferLevelMs:F1} ms, ASRC: {status.AsrcRatio:F6}, Slip: {status.SlipCount}, BRIR: {(status.BrirHasOutput ? "On" : "Off")}, InPeak: {ToDb(status.BrirInputPeak):F1} dB, OutPeak: {ToDb(status.BrirOutputPeak):F1} dB{error}";
        MeterText = $"RMS: {ToDb(meter.Rms):F1} dB, Peak: {ToDb(meter.Peak):F1} dB, Clip: {meter.ClipCount}";
        VuDb = ToDb(meter.Vu) + 18.0f;
        PeakDb = ToDb(meter.Peak);
        PeakHoldDb = ToDb(meter.PeakHold);
        TruePeakDb = ToDb(meter.TruePeak);
        TruePeakHoldDb = ToDb(meter.TruePeakHold);
        ClipCount = meter.ClipCount;
        SpectrumBands = spectrum.BandsDb;
    }

    private void BuildVUTicks()
    {
        const float leftDb = -20.0f;
        const float rightDb = 3.0f;
        const float leftAngle = -40.0f;
        const float rightAngle = 20.0f;
        const double cx = 100.0;
        const double cy = 100.0;
        const double labelRadius = 52.0;
        const float tickStartY = 30.0f;

        var labels = new HashSet<int> { -20, -10, -7, -5, -3, 0, 1, 3 };
        var ticks = new List<VUMeterTick>();
        for (int db = -20; db <= 3; db++)
        {
            float t = (db - leftDb) / (rightDb - leftDb);
            float angle = leftAngle + (rightAngle - leftAngle) * t;
            bool isZero = db == 0;
            bool isMajor = db % 5 == 0 || db == 3;
            bool isMid = db % 2 == 0;
            float length = isMajor ? 10.0f : isMid ? 6.0f : 3.0f;
            bool isRed = db >= 1;

            string? label = labels.Contains(db) ? (db > 0 ? $"+{db}" : db.ToString()) : null;
            double rad = angle * Math.PI / 180.0;
            double lx = cx + labelRadius * Math.Sin(rad);
            double ly = cy - labelRadius * Math.Cos(rad);

            float endY = tickStartY - length;
            ticks.Add(new VUMeterTick(angle, tickStartY, endY, label, lx - 6, ly - 6, isMajor, isZero, isRed));
        }

        VUTicks = ticks;
    }


    private static float ToDb(float linear)
    {
        if (linear <= 0) return -120.0f;
        return 20.0f * (float)Math.Log10(linear);
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }

    public void Dispose()
    {
        _statusTimer.Stop();
        _engine.Dispose();
    }

    public void SaveLastSettings()
    {
        try
        {
            string path = GetLastSettingsPath();
            Directory.CreateDirectory(Path.GetDirectoryName(path) ?? ".");
            var settings = new LastSettings
            {
                InputDeviceId = SelectedInput?.Id,
                OutputDeviceId = SelectedOutput?.Id,
                Preset = BuildPresetFromUi()
            };
            string json = JsonSerializer.Serialize(settings, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(path, json);
        }
        catch
        {
        }
    }

    public void LoadLastSettings()
    {
        try
        {
            string path = GetLastSettingsPath();
            if (!File.Exists(path)) return;
            string json = File.ReadAllText(path);
            LastSettings? settings = JsonSerializer.Deserialize<LastSettings>(json);
            if (settings is null) return;

            if (!string.IsNullOrWhiteSpace(settings.InputDeviceId))
            {
                SelectedInput = InputDevices.FirstOrDefault(d => d.Id == settings.InputDeviceId) ?? SelectedInput;
            }

            if (!string.IsNullOrWhiteSpace(settings.OutputDeviceId))
            {
                SelectedOutput = OutputDevices.FirstOrDefault(d => d.Id == settings.OutputDeviceId) ?? SelectedOutput;
            }

            if (settings.Preset is not null)
            {
                ApplyPreset(settings.Preset);
            }
        }
        catch
        {
        }
    }

    private void ApplySlotPreset(ActiveSlot slot, PresetSlot presetSlot, int sampleRate, int irSwitchXfadeMs, ref PresetSlot slotState, ref string? message)
    {
        UpdateSlotUiFromPreset(slot, presetSlot);
        slotState = presetSlot;
        if (presetSlot.Spatializer.BrirSet is not null)
        {
            BrirSet? set = LoadBrirSet(presetSlot.Spatializer.BrirSet, sampleRate, ref message);
            if (slot == ActiveSlot.A) _slotABrirSet = set;
            else _slotBBrirSet = set;

            if (set is not null)
            {
                _engine.SetBrirSet(slot, set);
                _engine.SetSeat(slot, presetSlot.Spatializer.SeatX, presetSlot.Spatializer.SeatY, presetSlot.Spatializer.Normalize.Mode, presetSlot.Spatializer.Normalize.TargetPeakDbFS, presetSlot.Spatializer.Normalize.TargetRmsDbFS, irSwitchXfadeMs);
                _engine.SetSpatializerMode(slot, SpatializerMode.Brir);
                return;
            }
        }

        if (presetSlot.Spatializer.Mode == PresetSpatializerMode.Off)
        {
            _engine.SetSpatializerMode(slot, SpatializerMode.Off);
            return;
        }

        if (string.IsNullOrWhiteSpace(presetSlot.Spatializer.BrirPath))
        {
            message = "BRIR path missing.";
            _engine.SetSpatializerMode(slot, SpatializerMode.Off);
            return;
        }

        int maxFrames = (int)Math.Round(sampleRate * 2.0);
        var loader = new IrLoader(maxFrames);
        IrLoadResult load = loader.LoadBrir(presetSlot.Spatializer.BrirPath, sampleRate);
        if (!load.Success || load.Ir is null)
        {
            message = load.Message ?? "Failed to load IR.";
            _engine.SetSpatializerMode(slot, SpatializerMode.Off);
            return;
        }

        IrNormalizeResult norm = IrNormalizer.Normalize(
            load.Ir,
            presetSlot.Spatializer.Normalize.Mode,
            presetSlot.Spatializer.Normalize.TargetPeakDbFS,
            presetSlot.Spatializer.Normalize.TargetRmsDbFS);

        if (norm.Warning is not null)
        {
            message = norm.Warning;
        }

        _engine.SetBrir(slot, norm.Ir, irSwitchXfadeMs);
        _engine.SetSpatializerMode(slot, SpatializerMode.Brir);
    }

    private void ApplyPreset(Preset preset)
    {
        if (SelectedOutput is null) return;
        int? sampleRate = _engine.TryGetDeviceSampleRate(SelectedOutput.Id);
        if (!sampleRate.HasValue) return;

        _settings.DriftTargetBufferMs = preset.Audio.DriftTargetBufferMs;
        _settings.DriftSlip.HiWaterMs = preset.Audio.DriftSlip.HiWaterMs;
        _settings.DriftSlip.LoWaterMs = preset.Audio.DriftSlip.LoWaterMs;
        _settings.DriftSlip.XfadeMs = preset.Audio.DriftSlip.XfadeMs;

        Bypass = preset.Controls.Bypass;
        ActiveSlot = preset.Controls.ActiveSlot;
        VolumeMatch = preset.Controls.VolumeMatch;
        ApplyControlsWithXfade(preset.Controls.XfadeMs, preset.Controls.IrSwitchXfadeMs);

        string? message = null;
        ApplySlotPreset(ActiveSlot.A, preset.Slots.A, sampleRate.Value, preset.Controls.IrSwitchXfadeMs, ref _slotAState, ref message);
        ApplySlotPreset(ActiveSlot.B, preset.Slots.B, sampleRate.Value, preset.Controls.IrSwitchXfadeMs, ref _slotBState, ref message);
    }

    private Preset BuildPresetFromUi()
    {
        _slotAState = BuildSlotPresetFromUi(ActiveSlot.A);
        _slotBState = BuildSlotPresetFromUi(ActiveSlot.B);

        return new Preset
        {
            Name = "Default",
            Version = "0.1.4",
            Audio = new PresetAudio
            {
                DriftTargetBufferMs = _settings.DriftTargetBufferMs,
                DriftSlip = new PresetDriftSlip
                {
                    HiWaterMs = _settings.DriftSlip.HiWaterMs,
                    LoWaterMs = _settings.DriftSlip.LoWaterMs,
                    XfadeMs = _settings.DriftSlip.XfadeMs
                }
            },
            Controls = new PresetControls
            {
                Bypass = Bypass,
                ActiveSlot = ActiveSlot,
                VolumeMatch = VolumeMatch,
                XfadeMs = _xfadeMs,
                IrSwitchXfadeMs = _irSwitchXfadeMs
            },
            Slots = new PresetSlots
            {
                A = _slotAState,
                B = _slotBState
            }
        };
    }

    private static string GetLastSettingsPath()
    {
        return Path.Combine(AppContext.BaseDirectory, "Settings", "last_settings.json");
    }

    private PresetSlot BuildSlotPresetFromUi(ActiveSlot slot)
    {
        bool enabled = slot == ActiveSlot.A ? SlotABrirEnabled : SlotBBrirEnabled;
        PresetBrirSet? set = null;
        if (slot == ActiveSlot.A && _slotABrirSetPaths.Count > 0)
        {
            set = new PresetBrirSet { Rows = SlotARows, Columns = SlotACols, Paths = new List<string>(_slotABrirSetPaths) };
        }
        if (slot == ActiveSlot.B && _slotBBrirSetPaths.Count > 0)
        {
            set = new PresetBrirSet { Rows = SlotBRows, Columns = SlotBCols, Paths = new List<string>(_slotBBrirSetPaths) };
        }

        return new PresetSlot
        {
            Spatializer = new PresetSpatializer
            {
                Mode = enabled ? PresetSpatializerMode.Brir : PresetSpatializerMode.Off,
                BrirPath = slot == ActiveSlot.A ? SlotABrirPath : SlotBBrirPath,
                BrirSet = set ?? (slot == ActiveSlot.A ? _slotAState.Spatializer.BrirSet : _slotBState.Spatializer.BrirSet),
                SeatX = slot == ActiveSlot.A ? SlotASeatX : SlotBSeatX,
                SeatY = slot == ActiveSlot.A ? SlotASeatY : SlotBSeatY,
                Normalize = new PresetNormalize
                {
                    Mode = slot == ActiveSlot.A ? SlotANormalizeMode : SlotBNormalizeMode,
                    TargetPeakDbFS = slot == ActiveSlot.A ? SlotATargetPeakDbFs : SlotBTargetPeakDbFs,
                    TargetRmsDbFS = slot == ActiveSlot.A ? SlotATargetRmsDbFs : SlotBTargetRmsDbFs
                }
            }
        };
    }

    private void UpdateSlotUiFromPreset(ActiveSlot slot, PresetSlot presetSlot)
    {
        bool enabled = presetSlot.Spatializer.Mode == PresetSpatializerMode.Brir;
        if (slot == ActiveSlot.A)
        {
            SlotABrirEnabled = enabled;
            SlotABrirPath = presetSlot.Spatializer.BrirPath ?? string.Empty;
            SlotANormalizeMode = presetSlot.Spatializer.Normalize.Mode;
            SlotATargetPeakDbFs = presetSlot.Spatializer.Normalize.TargetPeakDbFS;
            SlotATargetRmsDbFs = presetSlot.Spatializer.Normalize.TargetRmsDbFS;
            SlotASeatX = presetSlot.Spatializer.SeatX;
            SlotASeatY = presetSlot.Spatializer.SeatY;
            if (presetSlot.Spatializer.BrirSet is not null)
            {
                SlotARows = presetSlot.Spatializer.BrirSet.Rows;
                SlotACols = presetSlot.Spatializer.BrirSet.Columns;
                _slotABrirSetPaths = presetSlot.Spatializer.BrirSet.Paths.ToList();
            }
        }
        else
        {
            SlotBBrirEnabled = enabled;
            SlotBBrirPath = presetSlot.Spatializer.BrirPath ?? string.Empty;
            SlotBNormalizeMode = presetSlot.Spatializer.Normalize.Mode;
            SlotBTargetPeakDbFs = presetSlot.Spatializer.Normalize.TargetPeakDbFS;
            SlotBTargetRmsDbFs = presetSlot.Spatializer.Normalize.TargetRmsDbFS;
            SlotBSeatX = presetSlot.Spatializer.SeatX;
            SlotBSeatY = presetSlot.Spatializer.SeatY;
            if (presetSlot.Spatializer.BrirSet is not null)
            {
                SlotBRows = presetSlot.Spatializer.BrirSet.Rows;
                SlotBCols = presetSlot.Spatializer.BrirSet.Columns;
                _slotBBrirSetPaths = presetSlot.Spatializer.BrirSet.Paths.ToList();
            }
        }
    }

    private void UpdateSlotPresetSet(ActiveSlot slot, PresetBrirSet set)
    {
        if (slot == ActiveSlot.A) _slotAState.Spatializer.BrirSet = set;
        else _slotBState.Spatializer.BrirSet = set;
    }

    private BrirSet? LoadBrirSet(PresetBrirSet preset, int sampleRate, ref string? message)
    {
        if (preset.Paths.Count != preset.Rows * preset.Columns)
        {
            message = "BRIR set paths count mismatch.";
            return null;
        }

        int maxFrames = (int)Math.Round(sampleRate * 2.0);
        var loader = new IrLoader(maxFrames);
        var list = new List<BrirIr>(preset.Paths.Count);
        foreach (string path in preset.Paths)
        {
            IrLoadResult load = loader.LoadBrir(path, sampleRate);
            if (!load.Success || load.Ir is null)
            {
                message = load.Message ?? "Failed to load BRIR set IR.";
                return null;
            }
            list.Add(load.Ir);
        }

        return new BrirSet(preset.Rows, preset.Columns, list);
    }
}


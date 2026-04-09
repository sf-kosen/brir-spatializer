using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Threading;
using BrirPlayer.Next.App.ViewModels;
using BrirPlayer.Next.App.Web;
using Microsoft.Win32;

namespace BrirPlayer.Next.App;

public partial class MainWindow : Window
{
    private readonly MainShellViewModel _viewModel = new();
    private readonly LivePlaybackSession _livePlayback = new();
    private readonly AudioDeviceMonitor _deviceMonitor = new();
    private readonly DispatcherTimer _telemetryTimer;
    private bool _suppressLiveRestarts;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _viewModel;
        Loaded += MainWindow_Loaded;
        Closed += MainWindow_Closed;
        _viewModel.PropertyChanged += ViewModel_PropertyChanged;
        _deviceMonitor.DevicesChanged += DeviceMonitor_DevicesChanged;
        _telemetryTimer = new DispatcherTimer(
            TimeSpan.FromMilliseconds(180),
            DispatcherPriority.Background,
            TelemetryTimer_Tick,
            Dispatcher);
    }

    private void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        _viewModel.SetStatusMessage("Ready");
        _telemetryTimer.Start();
    }

    private void MainWindow_Closed(object? sender, EventArgs e)
    {
        _telemetryTimer.Stop();
        _deviceMonitor.DevicesChanged -= DeviceMonitor_DevicesChanged;
        _deviceMonitor.Dispose();
        _viewModel.PropertyChanged -= ViewModel_PropertyChanged;
        _livePlayback.Dispose();
    }

    private void DeviceMonitor_DevicesChanged(object? sender, EventArgs e)
    {
        Dispatcher.Invoke(() =>
        {
            RefreshDevicesCore();

            if (_livePlayback.IsRunning &&
                (_viewModel.SelectedInputDevice is null || _viewModel.SelectedOutputDevice is null))
            {
                _livePlayback.Stop();
                _viewModel.SetLiveState(false, "Audio route lost", "Selected device was removed. Re-select the route and start again.");
                return;
            }

            _viewModel.SetStatusMessage("Device list updated");
        });
    }

    private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_suppressLiveRestarts)
        {
            return;
        }

        if (!_livePlayback.IsRunning)
        {
            return;
        }

        switch (e.PropertyName)
        {
            case nameof(MainShellViewModel.SeatX):
            case nameof(MainShellViewModel.SeatY):
                _livePlayback.UpdateSeat(_viewModel.SeatX, _viewModel.SeatY);
                _viewModel.SetLiveState(true, _livePlayback.SessionStatus, _livePlayback.LastError);
                break;

            case nameof(MainShellViewModel.VolumeMatch):
                _livePlayback.UpdateVolumeMatch(_viewModel.VolumeMatch);
                _viewModel.SetLiveState(true, _livePlayback.SessionStatus, _livePlayback.LastError);
                break;

            case nameof(MainShellViewModel.SelectedPresetId):
            case nameof(MainShellViewModel.SelectedInputMode):
            case nameof(MainShellViewModel.SelectedInputDevice):
            case nameof(MainShellViewModel.SelectedOutputDevice):
            case nameof(MainShellViewModel.ExclusiveOutput):
                RestartLiveSession();
                break;
        }
    }

    private void RefreshDevicesButton_Click(object sender, RoutedEventArgs e)
    {
        RefreshDevicesCore();
        _viewModel.SetStatusMessage("Device list refreshed");
    }

    private void ModeButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: ModeChip mode })
        {
            _viewModel.SelectedInputMode = mode.Name;
        }
    }

    private void StartStopButton_Click(object sender, RoutedEventArgs e)
    {
        if (_livePlayback.IsRunning)
        {
            _livePlayback.Stop();
            _viewModel.SetLiveState(false, _livePlayback.SessionStatus, _livePlayback.LastError);
            return;
        }

        StartLiveSession();
    }

    private void ConvertFileButton_Click(object sender, RoutedEventArgs e)
    {
        var openDialog = new OpenFileDialog
        {
            Title = "Select source audio",
            Filter = "Audio Files|*.wav;*.flac;*.mp3;*.m4a|All Files|*.*"
        };

        if (openDialog.ShowDialog(this) != true)
        {
            return;
        }

        var saveDialog = new SaveFileDialog
        {
            Title = "Save spatialized wav",
            Filter = "WAV File|*.wav",
            FileName = Path.GetFileNameWithoutExtension(openDialog.FileName) + ".spatialized.wav"
        };

        if (saveDialog.ShowDialog(this) != true)
        {
            return;
        }

        bool success = _viewModel.TryConvertFile(openDialog.FileName, saveDialog.FileName, out string message);
        MessageBox.Show(this, message, success ? "Convert Complete" : "Convert Failed", MessageBoxButton.OK, success ? MessageBoxImage.Information : MessageBoxImage.Error);
    }

    private void PresetButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: SpacePresetCard preset })
        {
            _viewModel.SelectPreset(preset.Id);
        }
    }

    private void SeatButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: SeatNode seat })
        {
            _viewModel.SelectSeat(seat.Row, seat.Column);
        }
    }

    private void StartLiveSession()
    {
        try
        {
            _livePlayback.StartOrRestart(BuildRouteState(), BuildPlaybackRequest());
            _viewModel.SetLiveState(true, _livePlayback.SessionStatus, _livePlayback.LastError);
        }
        catch (Exception ex)
        {
            _livePlayback.ReportFailure(ex);
            _viewModel.SetLiveState(false, _livePlayback.SessionStatus, _livePlayback.LastError);
        }
    }

    private void RestartLiveSession()
    {
        try
        {
            _livePlayback.StartOrRestart(BuildRouteState(), BuildPlaybackRequest());
            _viewModel.SetLiveState(true, _livePlayback.SessionStatus, _livePlayback.LastError);
        }
        catch (Exception ex)
        {
            _livePlayback.ReportFailure(ex);
            _viewModel.SetLiveState(false, _livePlayback.SessionStatus, _livePlayback.LastError);
        }
    }

    private BridgeRouteState BuildRouteState()
    {
        return new BridgeRouteState(
            _viewModel.SelectedInputMode,
            _viewModel.SelectedInputDevice?.Id ?? string.Empty,
            _viewModel.SelectedOutputDevice?.Id ?? string.Empty,
            _viewModel.OutputVolume,
            _viewModel.ExclusiveOutput,
            _viewModel.VolumeMatch);
    }

    private void RefreshDevicesCore()
    {
        _suppressLiveRestarts = true;
        try
        {
            _viewModel.RefreshDevices();
        }
        finally
        {
            _suppressLiveRestarts = false;
        }
    }

    private PlaybackRequest BuildPlaybackRequest()
    {
        return new PlaybackRequest(
            _viewModel.SelectedInputMode,
            _viewModel.SelectedPresetId,
            _viewModel.SeatX,
            _viewModel.SeatY,
            _viewModel.VolumeMatch);
    }

    private void TelemetryTimer_Tick(object? sender, EventArgs e)
    {
        _viewModel.UpdateTelemetry(_livePlayback.GetTelemetry());
    }
}

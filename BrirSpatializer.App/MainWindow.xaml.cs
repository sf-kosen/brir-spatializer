using System.Windows;
using BrirSpatializer.Audio;
using Microsoft.Win32;
using System.IO;

namespace BrirSpatializer.App;

public partial class MainWindow : Window
{
    private readonly MainViewModel _viewModel = new();

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _viewModel;
    }

    private void StartStop_Click(object sender, RoutedEventArgs e)
    {
        _viewModel.ToggleStartStop();
    }

    private void LoadPreset_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Preset JSON (*.json)|*.json|All files (*.*)|*.*",
            Title = "Load Preset"
        };

        if (dialog.ShowDialog(this) != true) return;
        bool ok = _viewModel.TryLoadPreset(dialog.FileName, out string? message);
        if (!string.IsNullOrWhiteSpace(message))
        {
            MessageBoxImage icon = ok ? MessageBoxImage.Information : MessageBoxImage.Warning;
            MessageBox.Show(this, message, "Preset Load", MessageBoxButton.OK, icon);
        }
    }

    private void SavePreset_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new SaveFileDialog
        {
            Filter = "Preset JSON (*.json)|*.json|All files (*.*)|*.*",
            Title = "Save Preset",
            FileName = "BrirSpatializer_Preset.json"
        };

        if (dialog.ShowDialog(this) != true) return;
        _viewModel.SavePreset(dialog.FileName);
    }

    private void ExportWav_Click(object sender, RoutedEventArgs e)
    {
        var inputDialog = new OpenFileDialog
        {
            Filter = "Audio files (*.wav;*.flac;*.mp3;*.m4a)|*.wav;*.flac;*.mp3;*.m4a|All files (*.*)|*.*",
            Title = "Select Input Audio"
        };

        if (inputDialog.ShowDialog(this) != true) return;

        var outputDialog = new SaveFileDialog
        {
            Filter = "WAV files (*.wav)|*.wav",
            Title = "Save Processed WAV",
            FileName = Path.GetFileNameWithoutExtension(inputDialog.FileName) + "_spatialized.wav"
        };

        if (outputDialog.ShowDialog(this) != true) return;

        bool ok = _viewModel.TryExportCurrentPreset(inputDialog.FileName, outputDialog.FileName, out string? message);
        if (!string.IsNullOrWhiteSpace(message))
        {
            MessageBoxImage icon = ok ? MessageBoxImage.Information : MessageBoxImage.Warning;
            MessageBox.Show(this, message, "Convert File", MessageBoxButton.OK, icon);
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        _viewModel.SaveLastSettings();
        _viewModel.Dispose();
        base.OnClosed(e);
    }

    private void BrowseBrirA_Click(object sender, RoutedEventArgs e)
    {
        string? path = BrowseBrir();
        if (path is null) return;
        _viewModel.SetSlotPath(ActiveSlot.A, path);
    }

    private void BrowseBrirB_Click(object sender, RoutedEventArgs e)
    {
        string? path = BrowseBrir();
        if (path is null) return;
        _viewModel.SetSlotPath(ActiveSlot.B, path);
    }

    private void ApplyBrirA_Click(object sender, RoutedEventArgs e)
    {
        ApplyBrir(ActiveSlot.A);
    }

    private void ApplyBrirB_Click(object sender, RoutedEventArgs e)
    {
        ApplyBrir(ActiveSlot.B);
    }

    private void LoadSetA_Click(object sender, RoutedEventArgs e)
    {
        LoadBrirSet(ActiveSlot.A);
    }

    private void LoadSetB_Click(object sender, RoutedEventArgs e)
    {
        LoadBrirSet(ActiveSlot.B);
    }

    private void ApplySeatA_Click(object sender, RoutedEventArgs e)
    {
        ApplySeat(ActiveSlot.A);
    }

    private void ApplySeatB_Click(object sender, RoutedEventArgs e)
    {
        ApplySeat(ActiveSlot.B);
    }

    private void ApplyBrir(ActiveSlot slot)
    {
        bool ok = _viewModel.TryApplyBrir(slot, out string? message);
        if (!string.IsNullOrWhiteSpace(message))
        {
            MessageBoxImage icon = ok ? MessageBoxImage.Information : MessageBoxImage.Warning;
            MessageBox.Show(this, message, "BRIR", MessageBoxButton.OK, icon);
        }
    }

    private void ApplySeat(ActiveSlot slot)
    {
        bool ok = _viewModel.TryApplySeat(slot, out string? message);
        if (!string.IsNullOrWhiteSpace(message))
        {
            MessageBoxImage icon = ok ? MessageBoxImage.Information : MessageBoxImage.Warning;
            MessageBox.Show(this, message, "Seat", MessageBoxButton.OK, icon);
        }
    }

    private void LoadBrirSet(ActiveSlot slot)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "WAV files (*.wav)|*.wav|All files (*.*)|*.*",
            Title = "Select BRIR Set (multi-select)",
            Multiselect = true
        };

        if (dialog.ShowDialog(this) != true) return;
        bool ok = _viewModel.TryLoadBrirSet(slot, dialog.FileNames, out string? message);
        if (!string.IsNullOrWhiteSpace(message))
        {
            MessageBoxImage icon = ok ? MessageBoxImage.Information : MessageBoxImage.Warning;
            MessageBox.Show(this, message, "BRIR Set", MessageBoxButton.OK, icon);
        }
    }

    private string? BrowseBrir()
    {
        var dialog = new OpenFileDialog
        {
            Filter = "WAV files (*.wav)|*.wav|All files (*.*)|*.*",
            Title = "Select BRIR (4ch WAV)"
        };
        return dialog.ShowDialog(this) == true ? dialog.FileName : null;
    }
}


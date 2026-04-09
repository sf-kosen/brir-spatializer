using BrirSpatializer.Audio.Dsp;
using BrirSpatializer.Audio.Dsp.Processors;
using BrirSpatializer.Audio.Ir;
using BrirSpatializer.Audio.Presets;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace BrirSpatializer.Audio;

public sealed class OfflineRenderer
{
    private readonly AudioSettings _settings;
    private readonly BrirInterpolator _interpolator = new();

    public OfflineRenderer(AudioSettings settings)
    {
        _settings = settings;
    }

    public void Render(string inputPath, string outputPath, Preset preset, int expectedSampleRate)
    {
        var graph = new DspGraph();
        graph.SetBlockSize(_settings.BlockSize);
        graph.UpdateControls(new ControlSettings
        {
            Bypass = preset.Controls.Bypass,
            ActiveSlot = preset.Controls.ActiveSlot,
            VolumeMatch = preset.Controls.VolumeMatch,
            XfadeMs = preset.Controls.XfadeMs,
            IrSwitchXfadeMs = preset.Controls.IrSwitchXfadeMs
        });

        ConfigureSlot(graph, ActiveSlot.A, preset.Slots.A, expectedSampleRate, preset.Controls.IrSwitchXfadeMs);
        ConfigureSlot(graph, ActiveSlot.B, preset.Slots.B, expectedSampleRate, preset.Controls.IrSwitchXfadeMs);

        int dryDelaySamples = graph.GetDryAlignmentLatencySamples();
        graph.SetDryDelaySamples(dryDelaySamples);
        graph.ApplyPendingDryDelaySamples();

        using var reader = new AudioFileReader(inputPath);
        ISampleProvider provider = EnsureStereo(reader);
        if (provider.WaveFormat.SampleRate != expectedSampleRate)
        {
            provider = new WdlResamplingSampleProvider(provider, expectedSampleRate);
        }

        Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
        using var writer = new WaveFileWriter(outputPath, WaveFormat.CreateIeeeFloatWaveFormat(expectedSampleRate, 2));

        int blockFrames = Math.Max(1, _settings.BlockSize);
        float[] buffer = new float[blockFrames * 2];
        while (true)
        {
            int samplesRead = provider.Read(buffer, 0, buffer.Length);
            if (samplesRead <= 0)
            {
                break;
            }

            int framesRead = samplesRead / 2;
            if (framesRead * 2 < buffer.Length)
            {
                Array.Clear(buffer, framesRead * 2, buffer.Length - (framesRead * 2));
            }

            graph.Process(buffer, framesRead, expectedSampleRate);
            writer.WriteSamples(buffer, 0, framesRead * 2);
        }
    }

    private static ISampleProvider EnsureStereo(ISampleProvider provider)
    {
        return provider.WaveFormat.Channels switch
        {
            2 => provider,
            1 => new MonoToStereoSampleProvider(provider),
            _ => throw new InvalidOperationException(
                $"Offline render supports mono or stereo input only. Input channels={provider.WaveFormat.Channels}.")
        };
    }

    private void ConfigureSlot(DspGraph graph, ActiveSlot slot, PresetSlot presetSlot, int sampleRate, int irSwitchXfadeMs)
    {
        if (presetSlot.Spatializer.Mode == PresetSpatializerMode.Off)
        {
            graph.SetSpatializerMode(slot, SpatializerMode.Off);
            return;
        }

        if (presetSlot.Spatializer.BrirSet is not null)
        {
            BrirSet set = LoadBrirSet(presetSlot.Spatializer.BrirSet, sampleRate);
            BrirIr interpolated = _interpolator.Interpolate(set, presetSlot.Spatializer.SeatX, presetSlot.Spatializer.SeatY);
            IrNormalizeResult normalized = IrNormalizer.Normalize(
                interpolated,
                presetSlot.Spatializer.Normalize.Mode,
                presetSlot.Spatializer.Normalize.TargetPeakDbFS,
                presetSlot.Spatializer.Normalize.TargetRmsDbFS);
            graph.SetBrir(slot, normalized.Ir, irSwitchXfadeMs);
            graph.SetSpatializerMode(slot, SpatializerMode.Brir);
            return;
        }

        if (string.IsNullOrWhiteSpace(presetSlot.Spatializer.BrirPath))
        {
            throw new InvalidOperationException($"Slot {slot} BRIR path missing.");
        }

        int maxFrames = (int)Math.Round(sampleRate * 2.0);
        var loader = new IrLoader(maxFrames);
        IrLoadResult load = loader.LoadBrir(presetSlot.Spatializer.BrirPath, sampleRate);
        if (!load.Success || load.Ir is null)
        {
            throw new InvalidOperationException(load.Message ?? $"Failed to load Slot {slot} BRIR.");
        }

        graph.SetBrir(slot, load.Ir, irSwitchXfadeMs);
        graph.SetSpatializerMode(slot, SpatializerMode.Brir);
    }

    private static BrirSet LoadBrirSet(PresetBrirSet preset, int sampleRate)
    {
        if (preset.Paths.Count != preset.Rows * preset.Columns)
        {
            throw new InvalidOperationException("BRIR set paths count mismatch.");
        }

        int maxFrames = (int)Math.Round(sampleRate * 2.0);
        var loader = new IrLoader(maxFrames);
        var irs = new List<BrirIr>(preset.Paths.Count);
        foreach (string path in preset.Paths)
        {
            IrLoadResult load = loader.LoadBrir(path, sampleRate);
            if (!load.Success || load.Ir is null)
            {
                throw new InvalidOperationException(load.Message ?? "Failed to load BRIR set IR.");
            }

            irs.Add(load.Ir);
        }

        return new BrirSet(preset.Rows, preset.Columns, irs);
    }
}

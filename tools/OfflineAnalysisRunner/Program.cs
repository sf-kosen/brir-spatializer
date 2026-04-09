using System.Text.Json;
using BrirSpatializer.Audio;
using BrirSpatializer.Audio.Presets;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;

if (args.Length != 4)
{
    Console.Error.WriteLine("Usage: OfflineAnalysisRunner <input-audio> <last-settings-json> <decoded-wav> <processed-wav>");
    return 1;
}

string inputPath = Path.GetFullPath(args[0]);
string settingsPath = Path.GetFullPath(args[1]);
string decodedWavPath = Path.GetFullPath(args[2]);
string processedWavPath = Path.GetFullPath(args[3]);

if (!File.Exists(inputPath))
{
    Console.Error.WriteLine($"Input audio not found: {inputPath}");
    return 2;
}

if (!File.Exists(settingsPath))
{
    Console.Error.WriteLine($"Settings file not found: {settingsPath}");
    return 3;
}

LastSettingsEnvelope? settings = JsonSerializer.Deserialize<LastSettingsEnvelope>(File.ReadAllText(settingsPath));
if (settings?.Preset is null)
{
    Console.Error.WriteLine("Failed to parse preset from last_settings.json");
    return 4;
}

const int targetSampleRate = 48000;
Directory.CreateDirectory(Path.GetDirectoryName(decodedWavPath)!);
Directory.CreateDirectory(Path.GetDirectoryName(processedWavPath)!);

using (var reader = new AudioFileReader(inputPath))
{
    ISampleProvider provider = EnsureStereo(reader);
    if (provider.WaveFormat.SampleRate != targetSampleRate)
    {
        provider = new WdlResamplingSampleProvider(provider, targetSampleRate);
    }

    WaveFileWriter.CreateWaveFile16(decodedWavPath, provider);
}

var renderer = new OfflineRenderer(new AudioSettings());
renderer.Render(decodedWavPath, processedWavPath, settings.Preset, targetSampleRate);

Console.WriteLine($"Decoded WAV: {decodedWavPath}");
Console.WriteLine($"Processed WAV: {processedWavPath}");
return 0;

static ISampleProvider EnsureStereo(ISampleProvider provider)
{
    if (provider.WaveFormat.Channels == 2)
    {
        return provider;
    }

    if (provider.WaveFormat.Channels == 1)
    {
        return new MonoToStereoSampleProvider(provider);
    }

    throw new InvalidOperationException($"Unsupported input channel count: {provider.WaveFormat.Channels}");
}

file sealed class LastSettingsEnvelope
{
    public Preset? Preset { get; set; }
}

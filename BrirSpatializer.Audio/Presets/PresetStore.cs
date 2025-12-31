using System.Text.Json;
using System.Text.Json.Serialization;

namespace BrirSpatializer.Audio.Presets;

public sealed class PresetStore
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        Converters =
        {
            new JsonStringEnumConverter(JsonNamingPolicy.CamelCase)
        }
    };

    public Preset Load(string path)
    {
        string json = File.ReadAllText(path);
        Preset? preset = JsonSerializer.Deserialize<Preset>(json, Options);
        if (preset is null) throw new InvalidOperationException("Preset parse failed.");
        return preset;
    }

    public void Save(string path, Preset preset)
    {
        string json = JsonSerializer.Serialize(preset, Options);
        File.WriteAllText(path, json);
    }
}


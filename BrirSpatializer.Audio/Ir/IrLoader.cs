using NAudio.Wave;

namespace BrirSpatializer.Audio.Ir;

public sealed class IrLoader
{
    private readonly int _maxLengthFrames;

    public IrLoader(int maxLengthFrames)
    {
        _maxLengthFrames = maxLengthFrames;
    }

    public IrLoadResult LoadBrir(string path, int expectedSampleRate)
    {
        if (!File.Exists(path))
        {
            return new IrLoadResult(null, "IR file not found.");
        }

        using var reader = new WaveFileReader(path);
        if (reader.WaveFormat.Channels != 4)
        {
            return new IrLoadResult(null, "IR must be 4-channel WAV.");
        }

        if (reader.WaveFormat.SampleRate != expectedSampleRate)
        {
            return new IrLoadResult(
                null,
                $"IR sample rate mismatch. IR={reader.WaveFormat.SampleRate} Hz, output device={expectedSampleRate} Hz."
            );
        }

        List<float> samples = reader.WaveFormat.Encoding == WaveFormatEncoding.Pcm && reader.WaveFormat.BitsPerSample == 32
            ? ReadPcm32(reader)
            : ReadViaSampleProvider(reader);

        int totalFrames = samples.Count / 4;
        int frames = Math.Min(totalFrames, _maxLengthFrames);
        if (frames <= 0)
        {
            return new IrLoadResult(null, "IR contains no samples.");
        }

        float[] interleaved = samples.Count > frames * 4
            ? samples.Take(frames * 4).ToArray()
            : samples.ToArray();
        float[] ll = new float[frames];
        float[] lr = new float[frames];
        float[] rl = new float[frames];
        float[] rr = new float[frames];

        int idx = 0;
        for (int i = 0; i < frames; i++)
        {
            ll[i] = interleaved[idx++];
            lr[i] = interleaved[idx++];
            rl[i] = interleaved[idx++];
            rr[i] = interleaved[idx++];
        }

        string? warning = null;
        if (totalFrames > _maxLengthFrames)
        {
            warning = "IR truncated to max length.";
        }

        return new IrLoadResult(new BrirIr(reader.WaveFormat.SampleRate, ll, lr, rl, rr), warning);
    }

    private static List<float> ReadViaSampleProvider(WaveFileReader reader)
    {
        ISampleProvider sampleProvider = reader.ToSampleProvider();
        List<float> samples = new();
        float[] temp = new float[4096 * reader.WaveFormat.Channels];
        while (true)
        {
            int read = sampleProvider.Read(temp, 0, temp.Length);
            if (read <= 0) break;
            for (int i = 0; i < read; i++)
            {
                samples.Add(temp[i]);
            }
        }

        return samples;
    }

    private static List<float> ReadPcm32(WaveFileReader reader)
    {
        List<float> samples = new();
        byte[] buffer = new byte[reader.WaveFormat.BlockAlign * 4096];
        while (true)
        {
            int bytesRead = reader.Read(buffer, 0, buffer.Length);
            if (bytesRead <= 0) break;
            int count = bytesRead / 4;
            int src = 0;
            for (int i = 0; i < count; i++)
            {
                int sample = buffer[src] | (buffer[src + 1] << 8) | (buffer[src + 2] << 16) | (buffer[src + 3] << 24);
                samples.Add(sample / 2147483648f);
                src += 4;
            }
        }

        return samples;
    }
}

public sealed class IrLoadResult
{
    public IrLoadResult(BrirIr? ir, string? message)
    {
        Ir = ir;
        Message = message;
    }

    public BrirIr? Ir { get; }
    public string? Message { get; }
    public bool Success => Ir is not null;
}


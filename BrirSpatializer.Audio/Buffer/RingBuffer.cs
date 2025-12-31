using System.Threading;

namespace BrirSpatializer.Audio.Buffer;

public sealed class RingBuffer
{
    private readonly float[] _buffer;
    private readonly int _capacitySamples;
    private readonly int _channels;
    private int _writeIndex;
    private int _readIndex;
    private int _countSamples;

    public RingBuffer(int capacityFrames, int channels)
    {
        if (capacityFrames <= 0) throw new ArgumentOutOfRangeException(nameof(capacityFrames));
        if (channels <= 0) throw new ArgumentOutOfRangeException(nameof(channels));
        _channels = channels;
        _capacitySamples = capacityFrames * channels;
        _buffer = new float[_capacitySamples];
    }

    public int CapacityFrames => _capacitySamples / _channels;
    public int AvailableFrames => Volatile.Read(ref _countSamples) / _channels;

    public int Write(float[] source, int frames)
    {
        if (frames <= 0) return 0;
        int samplesToWrite = Math.Min(frames * _channels, _capacitySamples - Volatile.Read(ref _countSamples));
        if (samplesToWrite <= 0) return 0;

        int firstCopy = Math.Min(samplesToWrite, _capacitySamples - _writeIndex);
        Array.Copy(source, 0, _buffer, _writeIndex, firstCopy);
        int remaining = samplesToWrite - firstCopy;
        if (remaining > 0)
        {
            Array.Copy(source, firstCopy, _buffer, 0, remaining);
        }

        int newWrite = _writeIndex + samplesToWrite;
        if (newWrite >= _capacitySamples) newWrite -= _capacitySamples;
        Volatile.Write(ref _writeIndex, newWrite);
        Interlocked.Add(ref _countSamples, samplesToWrite);
        return samplesToWrite / _channels;
    }

    public int Read(float[] destination, int frames)
    {
        if (frames <= 0) return 0;
        int availableSamples = Volatile.Read(ref _countSamples);
        int samplesToRead = Math.Min(frames * _channels, availableSamples);
        if (samplesToRead <= 0) return 0;

        int firstCopy = Math.Min(samplesToRead, _capacitySamples - _readIndex);
        Array.Copy(_buffer, _readIndex, destination, 0, firstCopy);
        int remaining = samplesToRead - firstCopy;
        if (remaining > 0)
        {
            Array.Copy(_buffer, 0, destination, firstCopy, remaining);
        }

        int newRead = _readIndex + samplesToRead;
        if (newRead >= _capacitySamples) newRead -= _capacitySamples;
        Volatile.Write(ref _readIndex, newRead);
        Interlocked.Add(ref _countSamples, -samplesToRead);
        return samplesToRead / _channels;
    }

    public double GetFillLevelMs(int sampleRate)
    {
        if (sampleRate <= 0) return 0;
        double frames = Volatile.Read(ref _countSamples) / (double)_channels;
        return frames / sampleRate * 1000.0;
    }
}


namespace BrirSpatializer.Audio.Dsp;

public sealed class DelayLine
{
    private float[] _buffer = Array.Empty<float>();
    private int _writeIndex;
    private int _delaySamples;
    private int _channels;

    public void SetDelaySamples(int delaySamples, int blockFrames, int channels)
    {
        delaySamples = Math.Max(0, delaySamples);
        if (_delaySamples == delaySamples && _channels == channels && _buffer.Length >= (delaySamples + blockFrames) * channels)
        {
            return;
        }

        _delaySamples = delaySamples;
        _channels = channels;
        int capacity = Math.Max(1, (delaySamples + blockFrames) * channels);
        _buffer = new float[capacity];
        _writeIndex = 0;
    }

    public void Process(float[] input, float[] output, int frames)
    {
        int samples = frames * _channels;
        if (_delaySamples <= 0)
        {
            Array.Copy(input, output, samples);
            return;
        }

        int delaySamples = _delaySamples * _channels;
        for (int i = 0; i < samples; i++)
        {
            int readIndex = _writeIndex - delaySamples;
            if (readIndex < 0) readIndex += _buffer.Length;
            output[i] = _buffer[readIndex];
            _buffer[_writeIndex] = input[i];
            _writeIndex++;
            if (_writeIndex >= _buffer.Length) _writeIndex = 0;
        }
    }
}


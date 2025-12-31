using MathNet.Numerics;
using MathNet.Numerics.IntegralTransforms;

namespace BrirSpatializer.Audio.Dsp.Convolution;

public sealed class PartitionedConvolver
{
    private readonly int _fftSize;
    private readonly int _hopSize;
    private readonly int _partitions;
    private readonly Complex32[] _fftBuffer;
    private readonly Complex32[] _accumulate;
    private readonly Complex32[] _freqHistory;
    private readonly Complex32[][] _irFreq;
    private readonly float[] _timeBuffer;
    private readonly float[] _overlapInput;
    private readonly float[] _outputFifo;
    private readonly int _fifoCapacity;
    private int _fifoWrite;
    private int _fifoRead;
    private int _fifoAvailable;
    private int _framesInHop;
    private int _historyIndex;
    private bool _hasProcessed;

    public PartitionedConvolver(float[] ir, int fftSize, int hopSize)
    {
        if (fftSize <= 0 || hopSize <= 0) throw new ArgumentOutOfRangeException();
        if (fftSize != hopSize * 2) throw new ArgumentException("FFT size must be 2x hop size.");
        _fftSize = fftSize;
        _hopSize = hopSize;
        int partitions = (int)Math.Ceiling(ir.Length / (double)hopSize);
        _partitions = Math.Max(1, partitions);

        _fftBuffer = new Complex32[_fftSize];
        _accumulate = new Complex32[_fftSize];
        _freqHistory = new Complex32[_partitions * _fftSize];
        _irFreq = new Complex32[_partitions][];
        _timeBuffer = new float[_hopSize];
        _overlapInput = new float[_hopSize];
        _fifoCapacity = _hopSize * 4;
        _outputFifo = new float[_fifoCapacity];
        PrecomputeIr(ir);
    }

    public int HopSize => _hopSize;
    public int FftSize => _fftSize;
    public int LatencySamples => _hopSize;

    public void Reset()
    {
        Array.Clear(_fftBuffer);
        Array.Clear(_accumulate);
        Array.Clear(_freqHistory);
        Array.Clear(_timeBuffer);
        Array.Clear(_overlapInput);
        Array.Clear(_outputFifo);
        _fifoWrite = 0;
        _fifoRead = 0;
        _fifoAvailable = 0;
        _framesInHop = 0;
        _historyIndex = 0;
        _hasProcessed = false;
    }

    public void Process(float[] input, int inputOffset, float[] output, int outputOffset, int frames)
    {
        int idx = 0;
        while (idx < frames)
        {
            int remaining = frames - idx;
            int need = _hopSize - _framesInHop;
            int copy = Math.Min(need, remaining);

            Array.Copy(input, inputOffset + idx, _timeBuffer, _framesInHop, copy);
            _framesInHop += copy;
            idx += copy;

            if (_framesInHop == _hopSize)
            {
                ProcessHop();
                _framesInHop = 0;
            }
        }

        for (int i = 0; i < frames; i++)
        {
            if (_fifoAvailable > 0)
            {
                output[outputOffset + i] = _outputFifo[_fifoRead];
                _outputFifo[_fifoRead] = 0f;
                _fifoRead++;
                if (_fifoRead >= _fifoCapacity) _fifoRead = 0;
                _fifoAvailable--;
            }
            else
            {
                output[outputOffset + i] = 0f;
            }
        }
    }

    private void ProcessHop()
    {
        Array.Clear(_fftBuffer);
        for (int i = 0; i < _hopSize; i++)
        {
            _fftBuffer[i] = new Complex32(_overlapInput[i], 0f);
            _fftBuffer[i + _hopSize] = new Complex32(_timeBuffer[i], 0f);
        }

        Fourier.Forward(_fftBuffer, FourierOptions.Matlab);

        int historyOffset = _historyIndex * _fftSize;
        Array.Copy(_fftBuffer, 0, _freqHistory, historyOffset, _fftSize);

        Array.Clear(_accumulate);
        for (int p = 0; p < _partitions; p++)
        {
            int idx = _historyIndex - p;
            if (idx < 0) idx += _partitions;
            int srcOffset = idx * _fftSize;
            Complex32[] ir = _irFreq[p];
            for (int k = 0; k < _fftSize; k++)
            {
                _accumulate[k] += _freqHistory[srcOffset + k] * ir[k];
            }
        }

        Fourier.Inverse(_accumulate, FourierOptions.Matlab);

        for (int i = 0; i < _hopSize; i++)
        {
            float v = _accumulate[i + _hopSize].Real;
            _outputFifo[_fifoWrite] = v;
            _fifoWrite++;
            if (_fifoWrite >= _fifoCapacity) _fifoWrite = 0;
        }
        _fifoAvailable = Math.Min(_fifoCapacity, _fifoAvailable + _hopSize);

        for (int i = 0; i < _hopSize; i++)
        {
            _overlapInput[i] = _timeBuffer[i];
        }
        _historyIndex++;
        if (_historyIndex >= _partitions) _historyIndex = 0;
        _hasProcessed = true;
    }

    public bool HasProcessed => _hasProcessed;

    private void PrecomputeIr(float[] ir)
    {
        for (int p = 0; p < _partitions; p++)
        {
            Complex32[] buf = new Complex32[_fftSize];
            int start = p * _hopSize;
            int len = Math.Min(_hopSize, ir.Length - start);
            for (int i = 0; i < len; i++)
            {
                buf[i] = new Complex32(ir[start + i], 0f);
            }
            Fourier.Forward(buf, FourierOptions.Matlab);
            _irFreq[p] = buf;
        }
    }
}


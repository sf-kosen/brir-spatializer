using BrirSpatializer.Audio.Dsp.Convolution;
using BrirSpatializer.Audio.Ir;

namespace BrirSpatializer.Audio.Dsp.Spatial;

public sealed class BrirSpatializer
{
    private readonly PartitionedConvolver _ll;
    private readonly PartitionedConvolver _lr;
    private readonly PartitionedConvolver _rl;
    private readonly PartitionedConvolver _rr;
    private readonly float[] _inputL;
    private readonly float[] _inputR;
    private readonly float[] _outLL;
    private readonly float[] _outLR;
    private readonly float[] _outRL;
    private readonly float[] _outRR;
    private int _framesProcessed;
    private float _lastInputPeak;
    private float _lastOutputPeak;

    public BrirSpatializer(BrirIr ir, int fftSize, int hopSize)
    {
        _ll = new PartitionedConvolver(ir.Ll, fftSize, hopSize);
        _lr = new PartitionedConvolver(ir.Lr, fftSize, hopSize);
        _rl = new PartitionedConvolver(ir.Rl, fftSize, hopSize);
        _rr = new PartitionedConvolver(ir.Rr, fftSize, hopSize);
        _inputL = new float[hopSize];
        _inputR = new float[hopSize];
        _outLL = new float[hopSize];
        _outLR = new float[hopSize];
        _outRL = new float[hopSize];
        _outRR = new float[hopSize];
        LatencySamples = hopSize;
    }

    public int LatencySamples { get; }
    public bool HasOutput => _framesProcessed > 0;
    public float LastInputPeak => _lastInputPeak;
    public float LastOutputPeak => _lastOutputPeak;

    public void Reset()
    {
        _ll.Reset();
        _lr.Reset();
        _rl.Reset();
        _rr.Reset();
        Array.Clear(_inputL);
        Array.Clear(_inputR);
        Array.Clear(_outLL);
        Array.Clear(_outLR);
        Array.Clear(_outRL);
        Array.Clear(_outRR);
        _framesProcessed = 0;
        _lastInputPeak = 0f;
        _lastOutputPeak = 0f;
    }

    public void Process(float[] interleaved, int frames)
    {
        float inPeak = 0f;
        for (int i = 0; i < frames; i++)
        {
            int baseIndex = i * 2;
            _inputL[i] = interleaved[baseIndex];
            _inputR[i] = interleaved[baseIndex + 1];
            float a = Math.Abs(_inputL[i]);
            float b = Math.Abs(_inputR[i]);
            if (a > inPeak) inPeak = a;
            if (b > inPeak) inPeak = b;
        }

        _ll.Process(_inputL, 0, _outLL, 0, frames);
        _lr.Process(_inputL, 0, _outLR, 0, frames);
        _rl.Process(_inputR, 0, _outRL, 0, frames);
        _rr.Process(_inputR, 0, _outRR, 0, frames);

        for (int i = 0; i < frames; i++)
        {
            int baseIndex = i * 2;
            float outL = _outLL[i] + _outRL[i];
            float outR = _outLR[i] + _outRR[i];
            interleaved[baseIndex] = outL;
            interleaved[baseIndex + 1] = outR;
            float a = Math.Abs(outL);
            float b = Math.Abs(outR);
            if (a > _lastOutputPeak) _lastOutputPeak = a;
            if (b > _lastOutputPeak) _lastOutputPeak = b;
        }

        _framesProcessed += frames;
        _lastInputPeak = inPeak;
    }
}


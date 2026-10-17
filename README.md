# BRIR Spatializer

Real-time binaural cinema renderer for headphones (Windows, C++ / JUCE).
Goal: reproduce the sound of the center seat in a real cinema — stereo input is
upmixed to screen (L/C/R) and surround arrays, then convolved with cinema BRIRs
at low latency.

Rewrite in progress. The previous .NET/WPF implementation is kept on the
`legacy` branch.

## Planned v1 scope

- Input: stereo, via WASAPI loopback or a virtual audio cable
- Stereo → L/C/R + surround upmix
- Low-latency non-uniform partitioned convolution with center-seat cinema BRIRs
- Headphone compensation EQ
- BRIR generator: per-reflection HRIR directions + diffuse late reverb

## Requirements

- Windows 10/11
- Visual Studio 2022 Build Tools (MSVC, C++ workload)
- CMake 3.24+

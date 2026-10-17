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

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\tests\Release\brir_tests.exe
```

The app is `build\app\BrirCinema_artefacts\Release\BRIR Cinema.exe`.

## Cinema BRIRs

The app plays a *cinema set*: `cinema.json` plus one 2-channel (left ear, right
ear) WAV per speaker feed (L, C, R, Ls, Rs). Generate the default centre-seat set
(medium cinema, ~3000 m3, RT60 ~0.45 s, small-room X-curve calibration):

```powershell
python -m venv .venv
.venv/Scripts/pip install -r tools/cinema_brir/requirements.txt
.venv/Scripts/python tools/cinema_brir/generate_cinema.py
```

It needs the KU100 HRIR set (`Assets/HRTF/KU100_TH_Koeln/HRIR_FULL2DEG.sofa`) and
writes to `Generated/cinema/center_medium/`, which the app finds on first run.
`tools/render_file` renders any stereo file through the same chain offline.

## Layout

- `engine/` – JUCE-independent DSP: non-uniform partitioned convolver, STFT
  upmixer (stereo -> L C R Ls Rs), clock bridge (drift-compensating resampler),
  limiter, renderer
- `io/` – WASAPI capture (loopback or input endpoint) feeding the clock bridge
- `app/` – JUCE desktop app
- `tests/` – unit tests (doctest)
- `tools/` – `cinema_brir` (BRIR generator), `render_file` (offline render),
  `bench_convolver`, `capture_probe` (capture vs. system clock), `output_probe`
  (full real-time path inside a real output device, muted)


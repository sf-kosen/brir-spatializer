# BRIR Spatializer

Real-time BRIR (Binaural Room Impulse Response) spatializer for Windows (WPF/.NET 8).
This repo focuses on low-latency convolution, AB slot switching, and device drift compensation.
Audio assets are excluded from the repo.

## Features

- Dual-slot BRIR processing with AB crossfade and bypass
- BRIR set interpolation for seat-grid selection
- Metering and spectrum visualization
- WASAPI shared IO with drift compensation (ASRC or slip)
- Preset save/load (JSON)

## Requirements

- Windows 10/11
- .NET 8 SDK (x64)
- Optional: Python 3.10+ for BRIR generation

## Quick start (app)

1) Build the solution:

```powershell
dotnet build .\BrirSpatializer.sln
```

2) Run the app:

```powershell
dotnet run --project .\BrirSpatializer.App\BrirSpatializer.App.csproj
```

3) In the app:

- Select an input device and an output device
- Click Start
- Load a BRIR file (or a BRIR set) for Slot A or Slot B

Tip: If you want to process system audio, route it into a virtual audio cable
and select that cable as the input device.

## BRIR format

Load 4-channel WAV files with channel order:

- ch0: IR_LL
- ch1: IR_LR
- ch2: IR_RL
- ch3: IR_RR

## BRIR generation (optional)

The generator script uses pyroomacoustics and SOFA HRIR data. Provide `--sofa-path`
or set `SOFA_PATH`.

Profiles:

- layout_7_1_4: 7.1.4 layout (L/C/R/SL/SR/BL/BR + 4 heights + LFE)
- layout_lr_sub: L/R/Sub layout
- array_main_delay: main/outfill plus delay arrays

Setup:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r .\requirements.txt
```

Generate:

```powershell
python .\Scripts\generate_brir.py --profile layout_7_1_4 --sofa-path "C:\path\to\D1_48K_24bit_256tap_FIR_SOFA.sofa"
```

Outputs are written under `Generated\BRIR\<profile>\` by default (ignored by git), with:

- `speakers` for per-speaker BRIRs
- `grid_<rows>x<cols>` for seat-grid BRIRs

## Troubleshooting

- No audio output: confirm input/output devices are selected and sample rates match.
- Only stereo devices are supported.
- If BRIR loading fails, confirm the WAV has 4 channels in the required order.


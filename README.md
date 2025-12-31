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

## Build

```powershell
# From the repo root
dotnet build .\BrirSpatializer.sln
```

## Run

```powershell
# From the repo root
dotnet run --project .\BrirSpatializer.App\BrirSpatializer.App.csproj
```

## BRIR format

Place your BRIR WAVs wherever you want and load them from the UI. The app expects 4-channel WAVs with channel order:

- ch0: IR_LL
- ch1: IR_LR
- ch2: IR_RL
- ch3: IR_RR

## BRIR generation (optional)

The generator script uses pyroomacoustics and SOFA HRIR data. Provide `--sofa-path` or set `SOFA_PATH`.

Profiles:

- layout_7_1_4: 7.1.4 layout (L/C/R/SL/SR/BL/BR + 4 heights + LFE)
- layout_lr_sub: L/R/Sub layout
- array_main_delay: main/outfill plus delay arrays

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r .\requirements.txt
python .\Scripts\generate_brir.py --profile layout_7_1_4 --sofa-path "C:\path\to\D1_48K_24bit_256tap_FIR_SOFA.sofa"
```

Outputs are written under `Generated\BRIR\<profile>\` by default (ignored by git), with:

- `speakers` for per-speaker BRIRs
- `grid_<rows>x<cols>` for seat-grid BRIRs


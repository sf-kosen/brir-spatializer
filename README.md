# BRIR Spatializer

Realtime binaural spatializer for Windows (WPF/.NET 8). This repo intentionally excludes BRIR audio assets; generate or supply your own 4ch BRIR WAVs.

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

## BRIR assets

Place your BRIR WAVs wherever you want and load them from the UI. The app expects 4-channel WAVs with channel order:

- ch0: IR_LL
- ch1: IR_LR
- ch2: IR_RL
- ch3: IR_RR

## Generate BRIRs (optional)

The generator script and dependencies are provided. A SOFA file is required; pass `--sofa-path` or set `SOFA_PATH`.

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r .\requirements.txt
python .\Scripts\generate_brir.py --profile cinema --sofa-path "C:\path\to\D1_48K_24bit_256tap_FIR_SOFA.sofa"
```

Outputs are written under `Generated\BRIR\<profile>\` by default (ignored by git).


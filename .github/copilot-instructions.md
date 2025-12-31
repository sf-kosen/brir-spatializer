<!-- Copilot instructions tailored for the BrirSpatializer codebase -->
# How to help in this repo (concise)

- **Big picture:** This repository is a small WPF (.NET 8) audio application and supporting tools.
  - UI app: `BrirSpatializer.App` (WPF, MVVM). Key file: `BrirSpatializer.App/MainViewModel.cs` (startup, presets, UI->engine boundaries).
  - Audio core: `BrirSpatializer.Audio` (real-time audio path, NAudio WASAPI, DSP graph). Key file: `BrirSpatializer.Audio/AudioEngine.cs` (device handling, sample-rate checks, ring-buffer, start/stop semantics).
  - Data and assets: `Assets/` contains HRTF/SOFA and BRIR datasets used by the spatializer.
  - Tools: `Scripts/` contains Python utilities (e.g. `generate_cinema_brir.py`) for generating BRIR/IR assets.

- **Service boundaries & invariants agents must preserve:**
  - `AudioEngine.Start()` enforces stereo and equal input/output sample rate; do not change this contract without updating callers (UI expects exceptions for invalid devices).
  - Spatializer state is managed per-slot (ActiveSlot.A / ActiveSlot.B). `MainViewModel` maps UI toggles to `AudioEngine.SetSpatializerMode(...)`.
  - `BrirSet` loading and interpolation happens before calling `SetSeat(...)`  Eensure BRIR set assignment remains atomic.

- **Build & run (developer workflows):**
  - Build the solution with .NET CLI or open `BrirSpatializer.sln` in Visual Studio 2022+/VS for net8.0.
    ```bash
    dotnet build "BrirSpatializer.sln"
    ```
  - Run the WPF app from Visual Studio or via `dotnet run --project BrirSpatializer.App` for local testing.
  - Python tools: create a venv and install `requirements.txt` before running any `Scripts/*.py` utilities.
    ```bash
    python -m venv .venv
    .venv\Scripts\activate   # Windows
    pip install -r requirements.txt
    python Scripts/generate_cinema_brir.py --help
    ```

- **Project-specific patterns & examples:**
  - MVVM wiring: `MainViewModel` contains state and calls into `AudioEngine`. Example: `ToggleStartStop()` -> `AudioEngine.Start(SelectedInput.Id, SelectedOutput.Id)` and `Stop()`.
  - Audio constraints: `AudioEngine` only supports stereo and performs format checks (see `GetInputSampleFormat`/`GetOutputSampleFormat`). Prefer adding capability flags over implicit changes.
  - Assets loader: `Scripts/generate_cinema_brir.py` expects SOFA HRTF files under `Assets/HRTF/...`. Use `ASSETS_DIR` constant as canonical path.

- **Testing & debugging tips for agents:**
  - Run `dotnet build` to catch compile-time issues after C# edits. Fix compiler diagnostics in the same project before cross-project changes.
  - To validate audio-path logic, prefer non-invasive unit tests or small manual runs (select loopback devices) rather than changing DSP math blindly.
  - When modifying device/format handling, verify behavior with NAudio-supported devices (sample rate mismatch throws in `Start`).

- **What to change vs what to avoid:**
  - Safe: UI text, wiring, presets, small refactors in `MainViewModel`, helper scripts in `Scripts/`.
  - Risky: altering ring-buffer/ASRC logic, DSP pipeline ordering, BRIR interpolation math  Ethese affect runtime audio and should include audio verification steps.

- **Files to inspect for context:**
  - `BrirSpatializer.App/MainViewModel.cs` (UI <-> engine glue)
  - `BrirSpatializer.Audio/AudioEngine.cs` (device lifecycle, format checks)
  - `BrirSpatializer.Audio/Dsp/` (DSP graph implementation)
  - `Scripts/generate_cinema_brir.py` and `requirements.txt` (asset generation pipeline)

If anything here is unclear or you want more examples (unit tests, specific refactor suggestions), tell me which area to expand. 


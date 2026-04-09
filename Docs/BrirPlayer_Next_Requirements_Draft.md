# BrirPlayer Next Requirements Draft

## 1. Overview

- Working name: `BrirPlayer Next`
- Positioning: self-use Windows app for spatial audio experimentation and daily listening
- Goal: replace the current legacy app with a cleaner, more reliable tool for:
  - offline file conversion
  - real-time listening from loopback / virtual cable / file
  - BRIR set auditioning and seat exploration
- Primary success criterion: offline output sounds correct, and live playback behaves predictably enough for daily use

## 2. Target User

- Primary user: repo owner only
- Usage style:
  - listens to YouTube and other stereo content with BRIR
  - compares different BRIR spaces by ear
  - debugs device routing and latency issues directly in the app
- Product implication:
  - advanced controls may be exposed
  - raw device / buffer / sample-rate info may be shown
  - UX should optimize for speed and observability over beginner safety

## 3. Product Principles

- Reliability before feature count
- Offline and live paths must share the same BRIR processing core
- File conversion must always be considered the reference path
- Live routing must be debuggable from the UI without guessing
- UI should feel modern and intentional, but not hide useful low-level controls
- The app may be opinionated toward `48 kHz stereo`

## 4. In Scope

- Windows desktop app
- BRIR-based stereo to binaural processing
- BRIR single file and BRIR grid set loading
- Seat interpolation
- Offline conversion to WAV
- Real-time playback from:
  - audio file
  - WASAPI loopback input
  - optional VB-CABLE style input device
- Device selection
- Preset save/load
- Real-time debug telemetry

## 5. Out of Scope For Initial Version

- public-user onboarding polish
- cross-platform support
- plugin/VST support
- multichannel speaker output rendering
- cloud sync
- library management for large asset collections
- automatic BRIR generation inside the app
- streaming-service-specific integration beyond OS audio routing

## 6. MVP Definition

The first usable version must support all of the following:

1. Load a BRIR grid set and select seat position.
2. Convert a local audio file (`wav`, `flac`, `mp3`, `m4a`) to processed WAV.
3. Play a local audio file through the same BRIR pipeline in real time.
4. Capture a render device through WASAPI loopback and output to another device.
5. Show enough telemetry to diagnose buffer, sample rate, and routing issues.
6. Save and restore the current workspace state.

## 7. Functional Requirements

### 7.1 Source Modes

- The app shall support these input modes:
  - `File Convert`
  - `File Play`
  - `Loopback`
  - `Input Device`
- `Input Device` is primarily for virtual cable workflows and physical capture devices.
- Only one source mode is active at a time.

### 7.2 Output

- The app shall allow selection of a render output device.
- The app shall display the selected output device format.
- The app shall reject unsupported output formats with a clear message.
- Initial target format is `stereo`.

### 7.3 BRIR Loading

- The app shall load:
  - a single 4-channel BRIR WAV
  - a BRIR grid set
- Grid set loading shall support:
  - manual selection
  - automatic inference from `Seat_rX_cY.wav` names
- The app shall validate:
  - sample rate match
  - channel count
  - consistent grid dimensions
  - consistent BRIR lengths inside a set

### 7.4 Seat Control

- The app shall support continuous seat selection using normalized `X/Y`.
- The app shall interpolate within a BRIR grid set.
- The app should expose both:
  - a visual seat map
  - numeric values for exact debugging

### 7.5 Normalize / Gain

- The app shall support:
  - no normalize
  - peak normalize
  - RMS normalize
- The app shall expose target values directly.
- The app shall provide bypass and fast A/B comparison.

### 7.6 Offline Conversion

- The app shall convert local audio files to processed WAV.
- The app shall support mono and stereo input files.
- The app shall resample input to the processing sample rate when needed.
- The app shall finish conversion with a valid, playable WAV file even for long sources.
- The app shall show conversion progress or at minimum a busy state.

### 7.7 Real-Time Playback

- The app shall support real-time playback from:
  - local audio file
  - WASAPI loopback
  - optional capture/input device
- The app shall allow `Start` and `Stop` without requiring app restart.
- The app shall surface runtime errors in a dedicated status area.

### 7.8 Presets / Workspace

- The app shall save and load presets as JSON.
- The app shall restore the previous workspace on next launch.
- Workspace state shall include:
  - source mode
  - selected devices
  - BRIR set/file
  - seat position
  - normalize settings
  - current visual/debug layout if practical

### 7.9 Telemetry / Debug

- The app shall display:
  - current sample rate
  - source mode
  - buffer fill
  - underrun / overrun counts
  - clip count
  - selected device names
  - current BRIR state
- The app should display:
  - peak meters
  - live processing status
  - recent error log

## 8. Non-Functional Requirements

### 8.1 Audio Correctness

- Offline conversion is the ground truth path.
- Live output should match offline output perceptually for the same source and BRIR.
- No corrupted or truncated WAV files may be produced.

### 8.2 Performance

- The app shall handle full-song offline conversion reliably.
- The app should remain responsive during conversion.
- Real-time playback should prefer stable behavior over minimum theoretical latency.

### 8.3 Stability

- Start/stop cycles shall not require full app restart.
- Device changes should fail gracefully.
- File conversion should not leave partially written final files as the default success path.

### 8.4 Maintainability

- UI, audio I/O, and BRIR processing must be separated.
- Live and offline flows must share the same BRIR processor implementation.
- The new app must avoid giant view-model logic blobs.

## 9. UX / Design Requirements

## 9.1 Design Direction

- Visual direction: advanced personal audio lab
- Desired feel:
  - modern
  - bold
  - high-signal
  - slightly futuristic
- Not desired:
  - generic business tool look
  - old instrumentation panel clutter
  - path-string-heavy workflow

### 9.2 Layout

- Preferred structure:
  - left: source / output / transport
  - center: space, seat, BRIR controls
  - right: meters, status, debug
- The main screen should support daily use without modal-heavy workflows.
- Advanced details may live in collapsible panels, but not be hidden in hard-to-find dialogs.

### 9.3 Core Interaction Goals

- Switching spaces should feel fast.
- A/B comparison should be immediate.
- File conversion should be one obvious action.
- Routing state should be visible at a glance.

## 10. Proposed Technical Structure

- `BrirPlayer.Next.App`
  - UI shell
- `BrirPlayer.Next.Core`
  - sessions, presets, workspace, validation
- `BrirPlayer.Next.Audio`
  - sources, sinks, real-time session orchestration
- `BrirPlayer.Next.Brir`
  - BRIR loading, sets, interpolation, normalization, processor

### Shared Core Rule

- The same BRIR processor must be callable from:
  - offline conversion
  - file playback
  - loopback playback

## 11. Initial Technical Decisions

- Platform: Windows desktop
- Initial sample-rate policy: prioritize `48 kHz`
- Initial channel policy: mono/stereo input only
- Initial live policy: stable first, then low-latency optimization
- Keep VB-CABLE support optional, not architecturally central

## 12. Risks

- WASAPI loopback behavior may differ by device and driver
- shared-mode latency may still produce drift issues in long sessions
- if live I/O and processing are over-coupled, the app may regress into the current architecture
- modern UI ambition may slow down shipping unless MVP boundaries stay strict

## 13. Open Questions

- WPF vs WinUI 3
- whether file playback is required in MVP or only offline convert + loopback
- whether the app should allow sample-rate conversion in live mode initially
- how much debug info should remain always visible on the main screen
- whether to support dual-slot A/B in v1 or add it after the core is stable

## 14. Recommended Phase Plan

### Phase 1

- BRIR core extraction
- offline convert
- file playback
- preset/workspace

### Phase 2

- loopback live input
- output routing
- debug telemetry

### Phase 3

- optional VB-CABLE workflow
- richer meters
- A/B workflows
- stronger visual polish

## 15. Definition Of Done For Planning Phase

- Product concept is agreed.
- MVP is agreed.
- Platform and UI stack are chosen.
- Core modules are split before implementation starts.
- At least one reference workflow is written down:
  - `YouTube -> loopback -> BRIR -> headphones`
  - `m4a -> convert -> compare with live`

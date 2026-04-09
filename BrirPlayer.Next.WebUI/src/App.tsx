import {
  startTransition,
  useDeferredValue,
  useEffect,
  useEffectEvent,
  useMemo,
  useRef,
  useState,
} from "react";
import "./App.css";

type Mode = "Virtual Cable" | "Loopback" | "File Play" | "Convert File";

type LogLevel = "info" | "success" | "warn";

type Preset = {
  id: string;
  title: string;
  profile: string;
  summary: string;
  image: string;
};

type DeviceOption = {
  id: string;
  label: string;
  detail: string;
};

type Telemetry = {
  inputSampleRate: string;
  outputSampleRate: string;
  bufferMs: number;
  xruns: number;
  clipCount: number;
  latencyMs: number;
};

type LogEntry = {
  id: number;
  level: LogLevel;
  title: string;
  body: string;
  time: string;
};

type WorkspaceState = {
  mode: Mode;
  inputId: string;
  outputId: string;
  outputVolume: number;
  presetId: string;
  seatX: number;
  seatY: number;
  exclusive: boolean;
  volumeMatch: boolean;
};

type BridgeRoute = Pick<
  WorkspaceState,
  "mode" | "inputId" | "outputId" | "outputVolume" | "exclusive" | "volumeMatch"
>;

type BridgePayload = {
  bridge: boolean;
  isRunning: boolean;
  sessionStatus: string;
  lastError: string | null;
  inputDevices: DeviceOption[];
  outputDevices: DeviceOption[];
  route: BridgeRoute;
};

type BridgeMessage = {
  type: "bridgeState" | "devicesChanged" | "routeUpdated";
  payload: BridgePayload;
};

type WebViewMessageEvent = {
  data: unknown;
};

type WebViewHost = {
  postMessage: (message: unknown) => void;
  addEventListener: (type: "message", listener: (event: WebViewMessageEvent) => void) => void;
  removeEventListener: (type: "message", listener: (event: WebViewMessageEvent) => void) => void;
};

declare global {
  interface Window {
    chrome?: {
      webview?: WebViewHost;
    };
  }
}

const STORAGE_KEY = "brirplayer-next-workspace";
const liveModes: Mode[] = ["Virtual Cable", "Loopback", "File Play"];

const presets: Preset[] = [
  {
    id: "second-cinema",
    title: "Second Cinema",
    profile: "Reference",
    summary:
      "Balanced room tuning with a stable center image and a calm low-end shape for long YouTube listening.",
    image:
      "radial-gradient(circle at 30% 20%, rgba(255,255,255,0.44), transparent 22%), linear-gradient(135deg, #101010 0%, #353535 45%, #8a8a8a 100%)",
  },
  {
    id: "live-k-arena",
    title: "Live K Arena",
    profile: "Wide",
    summary:
      "Larger venue perspective with more width and audience bloom, tuned for live footage and concert captures.",
    image:
      "radial-gradient(circle at 22% 18%, rgba(255,255,255,0.28), transparent 18%), linear-gradient(135deg, #080808 0%, #202020 36%, #747474 100%)",
  },
  {
    id: "studio-near",
    title: "Studio Near",
    profile: "Nearfield",
    summary:
      "Dry, tighter staging for dialogue checks, nearfield judgment, and comparing source balance without excess room bloom.",
    image:
      "radial-gradient(circle at 70% 24%, rgba(255,255,255,0.3), transparent 20%), linear-gradient(135deg, #131313 0%, #2c2c2c 50%, #626262 100%)",
  },
];

const inputOptionsByMode: Record<Mode, DeviceOption[]> = {
  "Virtual Cable": [
    { id: "cable-output", label: "CABLE Output", detail: "48 kHz / stereo capture" },
    { id: "vb-monitor", label: "VB-CABLE Monitor", detail: "Fallback monitor input" },
  ],
  Loopback: [
    { id: "default-render", label: "Default Render", detail: "WASAPI loopback" },
    { id: "chrome-loopback", label: "Chrome Browser", detail: "Dedicated browser loopback" },
  ],
  "File Play": [
    { id: "local-file", label: "Local Audio File", detail: "Decoded inside app" },
    { id: "playlist-queue", label: "Playback Queue", detail: "Gapless file queue" },
  ],
  "Convert File": [
    { id: "file-queue", label: "File Queue", detail: "wav / flac / mp3 / m4a" },
    { id: "watch-folder", label: "Watch Folder", detail: "Batch convert target" },
  ],
};

const outputOptionsByMode: Record<Mode, DeviceOption[]> = {
  "Virtual Cable": [
    { id: "realtek", label: "Realtek Speakers", detail: "48 kHz / exclusive ready" },
    { id: "dac", label: "DAC Headphones", detail: "Reference output" },
  ],
  Loopback: [
    { id: "dac", label: "DAC Headphones", detail: "Monitoring output" },
    { id: "realtek", label: "Realtek Speakers", detail: "Fallback monitor" },
  ],
  "File Play": [
    { id: "realtek", label: "Realtek Speakers", detail: "48 kHz playback" },
    { id: "dac", label: "DAC Headphones", detail: "Critical listening" },
  ],
  "Convert File": [
    { id: "wav-export", label: "WAV Export", detail: "48 kHz stereo file" },
    { id: "archive-folder", label: "Archive Folder", detail: "Rendered output target" },
  ],
};

const defaultWorkspace: WorkspaceState = {
  mode: "Virtual Cable",
  inputId: "cable-output",
  outputId: "realtek",
  outputVolume: 0.8,
  presetId: "second-cinema",
  seatX: 0.5,
  seatY: 0.5,
  exclusive: true,
  volumeMatch: true,
};

const initialLogs: LogEntry[] = [
  {
    id: 1,
    level: "success",
    title: "Offline export verified",
    body: "The current BRIR chain produced a clean file render, so the DSP core looks healthy.",
    time: "16:08",
  },
  {
    id: 2,
    level: "warn",
    title: "Live route still under review",
    body: "Real-time playback quality depends on the capture path, output format, and device behavior.",
    time: "16:10",
  },
  {
    id: 3,
    level: "info",
    title: "Web UI shell started",
    body: "This build focuses on workspace structure, state, presets, and telemetry before native wiring.",
    time: "16:18",
  },
];

const defaultTelemetry: Telemetry = {
  inputSampleRate: "48.0 kHz",
  outputSampleRate: "48.0 kHz",
  bufferMs: 62,
  xruns: 0,
  clipCount: 0,
  latencyMs: 18,
};

function loadWorkspace(): WorkspaceState {
  const raw = window.localStorage.getItem(STORAGE_KEY);

  if (!raw) {
    return defaultWorkspace;
  }

  try {
    return { ...defaultWorkspace, ...(JSON.parse(raw) as Partial<WorkspaceState>) };
  } catch {
    return defaultWorkspace;
  }
}

function formatSeat(workspace: WorkspaceState) {
  const row = Math.round(workspace.seatY * 4);
  const col = Math.round(workspace.seatX * 4);
  return {
    row,
    col,
    label: `Seat ${row}:${col}`,
    coordinates: `X ${workspace.seatX.toFixed(2)} / Y ${workspace.seatY.toFixed(2)}`,
  };
}

function getOption(options: DeviceOption[], id: string) {
  return options.find((option) => option.id === id) ?? options[0];
}

function getModeDescription(mode: Mode) {
  switch (mode) {
    case "Virtual Cable":
      return "Browser audio enters the app through the virtual input, then leaves through your listening device.";
    case "Loopback":
      return "The app listens to an existing render device and monitors it through a second output.";
    case "File Play":
      return "Local files are decoded and auditioned through the same spatial processing chain.";
    case "Convert File":
      return "Offline export keeps the BRIR path intact while writing a clean wav render.";
  }
}

function getNativeHost() {
  return window.chrome?.webview;
}

function App() {
  const [workspace, setWorkspace] = useState<WorkspaceState>(loadWorkspace);
  const [liveRunning, setLiveRunning] = useState(false);
  const [convertState, setConvertState] = useState<"Ready" | "Rendering" | "Complete">("Ready");
  const [telemetry, setTelemetry] = useState<Telemetry>(defaultTelemetry);
  const [logs, setLogs] = useState<LogEntry[]>(initialLogs);
  const [logQuery, setLogQuery] = useState("");
  const [bridgeActive, setBridgeActive] = useState(false);
  const [sessionStatus, setSessionStatus] = useState("Idle");
  const [lastError, setLastError] = useState<string | null>(null);
  const [nativeInputDevices, setNativeInputDevices] = useState<DeviceOption[]>([]);
  const [nativeOutputDevices, setNativeOutputDevices] = useState<DeviceOption[]>([]);
  const deferredLogQuery = useDeferredValue(logQuery);
  const bridgeSeenRef = useRef(false);
  const lastLoggedErrorRef = useRef<string | null>(null);
  const hostTransport = getNativeHost();
  const httpTransport = !hostTransport && window.location.protocol.startsWith("http");
  const liveImplemented = workspace.mode === "Virtual Cable";
  const canStartLive = bridgeActive && liveImplemented;
  const browserPreview = !hostTransport && !httpTransport;

  const currentPreset = useMemo(
    () => presets.find((preset) => preset.id === workspace.presetId) ?? presets[0],
    [workspace.presetId],
  );

  const inputOptions = useMemo(() => {
    if (bridgeActive && liveModes.includes(workspace.mode) && nativeInputDevices.length > 0) {
      return nativeInputDevices;
    }

    return inputOptionsByMode[workspace.mode];
  }, [bridgeActive, nativeInputDevices, workspace.mode]);

  const outputOptions = useMemo(() => {
    if (bridgeActive && workspace.mode !== "Convert File" && nativeOutputDevices.length > 0) {
      return nativeOutputDevices;
    }

    return outputOptionsByMode[workspace.mode];
  }, [bridgeActive, nativeOutputDevices, workspace.mode]);

  const currentInput = useMemo(
    () => getOption(inputOptions, workspace.inputId),
    [inputOptions, workspace.inputId],
  );

  const currentOutput = useMemo(
    () => getOption(outputOptions, workspace.outputId),
    [outputOptions, workspace.outputId],
  );

  const seat = useMemo(() => formatSeat(workspace), [workspace]);

  const filteredLogs = useMemo(() => {
    const query = deferredLogQuery.trim().toLowerCase();
    if (!query) {
      return logs;
    }

    return logs.filter((entry) => `${entry.title} ${entry.body}`.toLowerCase().includes(query));
  }, [deferredLogQuery, logs]);

  function addLog(level: LogLevel, title: string, body: string) {
    setLogs((current) => [
      {
        id: current[0] ? current[0].id + 1 : 1,
        level,
        title,
        body,
        time: new Date().toLocaleTimeString("ja-JP", { hour: "2-digit", minute: "2-digit" }),
      },
      ...current,
    ]);
  }

  function updateWorkspace(patch: Partial<WorkspaceState>) {
    setWorkspace((current) => ({ ...current, ...patch }));
  }

  const applyBridgePayload = useEffectEvent((payload: BridgePayload, messageType?: BridgeMessage["type"]) => {
    setBridgeActive(payload.bridge);
    setNativeInputDevices(payload.inputDevices);
    setNativeOutputDevices(payload.outputDevices);
    setLiveRunning(payload.isRunning);
    setSessionStatus(payload.sessionStatus);
    setLastError(payload.lastError);

    startTransition(() => {
      setWorkspace((current) => ({
        ...current,
        mode: payload.route.mode,
        inputId: payload.route.inputId || current.inputId,
        outputId: payload.route.outputId || current.outputId,
        outputVolume: payload.route.outputVolume,
        exclusive: payload.route.exclusive,
        volumeMatch: payload.route.volumeMatch,
      }));
    });

    if (payload.bridge && !bridgeSeenRef.current) {
      bridgeSeenRef.current = true;
      addLog("success", "Bridge connected", "The Web UI is now talking to the desktop audio engine.");
    }

    if (messageType === "devicesChanged") {
      addLog(
        "info",
        "Audio devices updated",
        "The output list was refreshed from Windows, so newly connected headphones should appear immediately.",
      );
    }

    if (payload.lastError && payload.lastError !== lastLoggedErrorRef.current) {
      lastLoggedErrorRef.current = payload.lastError;
      addLog("warn", "Playback error", payload.lastError);
    }

    if (!payload.lastError) {
      lastLoggedErrorRef.current = null;
    }
  });

  async function postBridgeCommand(type: string, payload?: unknown) {
    if (hostTransport) {
      hostTransport.postMessage(payload === undefined ? { type } : { type, payload });
      return;
    }

    if (!httpTransport) {
      return;
    }

    try {
      const response = await fetch("/api/command", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        body: JSON.stringify(payload === undefined ? { type } : { type, payload }),
      });

      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }

      const nextPayload = (await response.json()) as BridgePayload;
      applyBridgePayload(nextPayload);
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown bridge error";
      setLastError(message);
      addLog("warn", "Bridge request failed", message);
    }
  }

  const handleBridgeMessage = useEffectEvent((message: unknown) => {
    const parsed = typeof message === "string" ? JSON.parse(message) : (message as BridgeMessage | undefined);

    if (!parsed || typeof parsed !== "object" || !("type" in parsed)) {
      return;
    }

    const envelope = parsed as BridgeMessage;
    applyBridgePayload(envelope.payload, envelope.type);
  });

  useEffect(() => {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(workspace));
  }, [workspace]);

  useEffect(() => {
    if (!hostTransport) {
      return;
    }

    const listener = (event: WebViewMessageEvent) => {
      handleBridgeMessage(event.data);
    };

    hostTransport.addEventListener("message", listener);
    hostTransport.postMessage({ type: "bridgeReady" });

    return () => {
      hostTransport.removeEventListener("message", listener);
    };
  }, [handleBridgeMessage, hostTransport]);

  useEffect(() => {
    if (!httpTransport) {
      return;
    }

    let cancelled = false;

    async function refreshState(messageType?: BridgeMessage["type"]) {
      try {
        const response = await fetch("/api/state", { cache: "no-store" });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        const payload = (await response.json()) as BridgePayload;
        if (!cancelled) {
          applyBridgePayload(payload, messageType);
        }
      } catch (error) {
        if (cancelled) {
          return;
        }

        const message = error instanceof Error ? error.message : "Unable to reach local bridge";
        setBridgeActive(false);
        setLastError(message);
      }
    }

    void refreshState("bridgeState");
    const interval = window.setInterval(() => {
      void refreshState();
    }, 1500);

    return () => {
      cancelled = true;
      window.clearInterval(interval);
    };
  }, [applyBridgePayload, httpTransport]);

  useEffect(() => {
    if (!liveRunning) {
      setTelemetry((current) => ({ ...current, bufferMs: 62, latencyMs: 18 }));
      return;
    }

    const interval = window.setInterval(() => {
      setTelemetry((current) => {
        const nextBuffer = 56 + Math.round(Math.random() * 14);
        const nextLatency = 15 + Math.round(Math.random() * 8);
        const xrunHit = Math.random() > 0.93 ? 1 : 0;

        return {
          ...current,
          bufferMs: nextBuffer,
          latencyMs: nextLatency,
          xruns: current.xruns + xrunHit,
        };
      });
    }, 1400);

    return () => {
      window.clearInterval(interval);
    };
  }, [liveRunning]);

  useEffect(() => {
    if (convertState !== "Rendering") {
      return;
    }

    const timer = window.setTimeout(() => {
      setConvertState("Complete");
      addLog("success", "Export complete", "Rendered file written with the current preset, seat, and normalization.");
    }, 1500);

    return () => {
      window.clearTimeout(timer);
    };
  }, [convertState]);

  function handleModeChange(mode: Mode) {
    const nextInput = inputOptionsByMode[mode][0];
    const nextOutput = outputOptionsByMode[mode][0];

    startTransition(() => {
      updateWorkspace({
        mode,
        inputId: bridgeActive && liveModes.includes(mode) ? workspace.inputId : nextInput.id,
        outputId: bridgeActive && mode !== "Convert File" ? workspace.outputId : nextOutput.id,
      });
    });

    void postBridgeCommand("setMode", mode);
    addLog("info", `Route changed to ${mode}`, getModeDescription(mode));
  }

  function handlePresetChange(presetId: string) {
    startTransition(() => {
      updateWorkspace({ presetId });
    });

    if (bridgeActive && liveRunning) {
      void postBridgeCommand("setPreset", {
        mode: workspace.mode,
        presetId,
        seatX: workspace.seatX,
        seatY: workspace.seatY,
        volumeMatch: workspace.volumeMatch,
      });
    }
  }

  function handleSeatChange(seatX: number, seatY: number) {
    startTransition(() => {
      updateWorkspace({ seatX, seatY });
    });

    if (bridgeActive && liveRunning) {
      void postBridgeCommand("setSeat", {
        mode: workspace.mode,
        presetId: workspace.presetId,
        seatX,
        seatY,
        volumeMatch: workspace.volumeMatch,
      });
    }
  }

  function handleInputChange(value: string) {
    updateWorkspace({ inputId: value });
    void postBridgeCommand("setInputDevice", value);
  }

  function handleOutputChange(value: string) {
    updateWorkspace({ outputId: value });
    void postBridgeCommand("setOutputDevice", value);
  }

  function handleOutputVolumeChange(value: number) {
    updateWorkspace({ outputVolume: value });
    void postBridgeCommand("setOutputVolume", value);
  }

  function handleExclusiveChange(checked: boolean) {
    updateWorkspace({ exclusive: checked });
    void postBridgeCommand("setExclusive", checked);
  }

  function handleVolumeMatchChange(checked: boolean) {
    updateWorkspace({ volumeMatch: checked });
    void postBridgeCommand("setVolumeMatch", checked);
  }

  function handleRefreshDevices() {
    void postBridgeCommand("refreshDevices");
  }

  function handleStartStop() {
    if (!canStartLive) {
      addLog(
        "warn",
        browserPreview ? "Preview mode only" : bridgeActive ? "Mode not ready" : "Bridge offline",
        browserPreview
          ? "Use the BrirPlayer Next desktop window. The browser preview cannot control Windows audio devices."
          : bridgeActive
            ? `${workspace.mode} live playback has not been wired yet. Switch back to Virtual Cable for now.`
            : "Start the BrirPlayer Next desktop app first so this page can reach the local audio engine.",
      );
      return;
    }

    const next = !liveRunning;
    if (next) {
      void postBridgeCommand("startLive", {
        mode: workspace.mode,
        presetId: workspace.presetId,
        seatX: workspace.seatX,
        seatY: workspace.seatY,
        volumeMatch: workspace.volumeMatch,
      });
    } else {
      void postBridgeCommand("stopLive");
    }

    addLog(
      next ? "success" : "info",
      next ? "Live session requested" : "Live session stop requested",
      next
        ? `${currentInput.label} will be routed to ${currentOutput.label} with ${currentPreset.title}.`
        : "Stop has been requested for the current live session.",
    );
  }

  function handleConvert() {
    setConvertState("Rendering");
    addLog("info", "Offline export started", `Rendering ${currentPreset.title} from ${seat.label} into a 48 kHz stereo wav.`);
  }

  return (
    <div className={`shell ${liveRunning ? "is-live" : ""}`}>
      {browserPreview ? (
        <div className="bridge-warning">
          <strong>Browser preview only</strong>
          <p>
            This page is just the visual preview. Use the <span>BrirPlayer Next</span> desktop
            window for real output devices and live playback.
          </p>
        </div>
      ) : null}

      <header className="hero">
        <div className="hero__copy">
          <span className="eyebrow">BrirPlayer Next</span>
          <h1>Native bridge, web UI.</h1>
          <p>
            The shell now talks to Windows audio devices through the local desktop bridge, so
            Chrome and the desktop window can control the same live engine.
          </p>
        </div>

        <div className="hero__stats">
          <StatPill label="Session" value={liveRunning ? "Live On" : "Idle"} inverse={false} />
          <StatPill label="Mode" value={workspace.mode} inverse />
          <StatPill
            label="Bridge"
            value={bridgeActive ? "Connected" : browserPreview ? "Preview Only" : "Offline"}
            inverse={false}
          />
        </div>
      </header>

      <main className="layout">
        <section className="rail rail--dark">
          <div className="section-heading">
            <span className="eyebrow">Routing</span>
            <h2>Build the listening path first.</h2>
          </div>

          <div className="segmented">
            {(["Virtual Cable", "Loopback", "File Play", "Convert File"] as const).map((mode) => (
              <button
                key={mode}
                type="button"
                className={workspace.mode === mode ? "is-active" : ""}
                onClick={() => handleModeChange(mode)}
              >
                {mode}
              </button>
            ))}
          </div>

          <ControlField
            label="Input"
            value={workspace.inputId}
            detail={currentInput.detail}
            options={inputOptions}
            onChange={handleInputChange}
          />

          <ControlField
            label="Output"
            value={workspace.outputId}
            detail={currentOutput.detail}
            options={outputOptions}
            onChange={handleOutputChange}
          />

          <div className="switch-grid">
            <Toggle
              label="Exclusive Output"
              checked={workspace.exclusive}
              onChange={handleExclusiveChange}
            />
            <Toggle
              label="Volume Match"
              checked={workspace.volumeMatch}
              onChange={handleVolumeMatchChange}
            />
          </div>

          <div className="volume-block">
            <div className="volume-block__head">
              <span>Output Volume</span>
              <strong>{Math.round(workspace.outputVolume * 100)}%</strong>
            </div>
            <input
              type="range"
              min="0"
              max="1"
              step="0.01"
              value={workspace.outputVolume}
              onChange={(event) => handleOutputVolumeChange(Number(event.target.value))}
            />
          </div>

          <div className="action-row">
            <button
              type="button"
              className="button button--primary"
              onClick={handleStartStop}
              disabled={!canStartLive}
              title={
                browserPreview
                  ? "Desktop app window only"
                  : !bridgeActive
                    ? "Start the desktop app first"
                    : liveImplemented
                    ? undefined
                    : "Only Virtual Cable live is implemented right now"
              }
            >
              {liveRunning ? "Stop Live" : "Start Live"}
            </button>
            <button type="button" className="button button--secondary" onClick={handleConvert}>
              Convert File
            </button>
          </div>

          <div className="action-row action-row--compact">
            <button type="button" className="button button--ghost" onClick={handleRefreshDevices}>
              Refresh Devices
            </button>
          </div>

          <div className="route-card">
            <div className="route-card__head">
              <span className="tag">{workspace.mode}</span>
              <span className="eyebrow">Current path</span>
            </div>

            <RouteStep index="01" title="Source" body={currentInput.label} />
            <RouteStep index="02" title="Processing" body="Resample, BRIR, limiter, telemetry" />
            <RouteStep index="03" title="Destination" body={currentOutput.label} />
          </div>

          <div className="session-note">
            <strong>{sessionStatus}</strong>
            <p>
              {lastError ??
                (browserPreview
                  ? "Desktop app window only. Browser preview does not enumerate Windows audio devices."
                  : !bridgeActive
                    ? "The local desktop bridge is not connected yet. Start BrirPlayer Next to control real devices from Chrome."
                    : liveImplemented
                    ? "Native bridge connected. Virtual Cable live should use the real Windows device list."
                    : `${workspace.mode} live playback is not wired yet. Switch to Virtual Cable for now.`)}
            </p>
          </div>
        </section>

        <section className="stage">
          <section className="surface spotlight">
            <div className="spotlight__visual" style={{ background: currentPreset.image }} />

            <div className="spotlight__copy">
              <span className="eyebrow">Selected space</span>
              <h2>{currentPreset.title}</h2>
              <p>{currentPreset.summary}</p>
            </div>

            <div className="spotlight__facts">
              <FactCard label="Seat" value={seat.coordinates} />
              <FactCard label="Profile" value={currentPreset.profile} />
              <FactCard label="Latency" value={`${telemetry.latencyMs} ms`} />
            </div>
          </section>

          <section className="surface">
            <div className="surface__head">
              <div>
                <span className="eyebrow">Seat field</span>
                <h3>Move the listening point like it belongs to the room.</h3>
              </div>
              <div className="meta-pills">
                <span>5 x 5</span>
                <span>Interpolated</span>
              </div>
            </div>

            <div className="seat-surface">
              <div className="seat-grid">
                {Array.from({ length: 25 }, (_, index) => {
                  const row = Math.floor(index / 5);
                  const col = index % 5;
                  const active = row === seat.row && col === seat.col;

                  return (
                    <button
                      key={`${row}-${col}`}
                      type="button"
                      className={`seat-node ${active ? "is-active" : ""}`}
                      onClick={() => handleSeatChange(col / 4, row / 4)}
                    >
                      {row}:{col}
                    </button>
                  );
                })}
              </div>

              <div className="slider-grid">
                <Slider
                  label="Seat X"
                  value={workspace.seatX}
                  onChange={(value) => handleSeatChange(value, workspace.seatY)}
                />
                <Slider
                  label="Seat Y"
                  value={workspace.seatY}
                  onChange={(value) => handleSeatChange(workspace.seatX, value)}
                />
              </div>
            </div>
          </section>

          <section className="surface">
            <div className="surface__head">
              <div>
                <span className="eyebrow">Presets</span>
                <h3>Keep rooms scannable, not buried in file paths.</h3>
              </div>
              <span className="tag">A/B ready later</span>
            </div>

            <div className="preset-list">
              {presets.map((preset) => (
                <button
                  key={preset.id}
                  type="button"
                  className={`preset-card ${preset.id === workspace.presetId ? "is-active" : ""}`}
                  onClick={() => handlePresetChange(preset.id)}
                >
                  <span>{preset.title}</span>
                  <strong>{preset.profile}</strong>
                  <small>{preset.summary}</small>
                </button>
              ))}
            </div>
          </section>

          <section className="surface convert-panel">
            <div className="surface__head">
              <div>
                <span className="eyebrow">Export</span>
                <h3>Offline conversion uses the same BRIR core as live playback.</h3>
              </div>
              <span className="tag">{convertState}</span>
            </div>

            <div className="convert-grid">
              <div className="bars" aria-hidden="true">
                {[24, 32, 46, 54, 66, 72, 68, 58, 50, 38, 28, 22].map((height, index) => (
                  <span key={height + index} style={{ height: `${height}%` }} />
                ))}
              </div>

              <div className="queue-card">
                <h4>Current export target</h4>
                <p>maou_14_shining_star.spatialized.wav</p>
                <ul>
                  <li>Preset: {currentPreset.title}</li>
                  <li>{seat.label}</li>
                  <li>Normalization: Peak / RMS matched</li>
                </ul>
              </div>
            </div>
          </section>
        </section>

        <aside className="rail">
          <section className="surface inspector-block">
            <div className="surface__head">
              <div>
                <span className="eyebrow">Telemetry</span>
                <h3>Always visible health.</h3>
              </div>
              <span className="tag tag--solid">Live</span>
            </div>

            <Metric label="Input SR" value={telemetry.inputSampleRate} fill={100} />
            <Metric label="Output SR" value={telemetry.outputSampleRate} fill={100} />
            <Metric label="Buffer" value={`${telemetry.bufferMs} ms`} fill={Math.min(100, telemetry.bufferMs)} />
            <Metric label="XRuns" value={String(telemetry.xruns)} fill={Math.min(90, 8 + telemetry.xruns * 4)} />
            <Metric label="Clip" value={String(telemetry.clipCount)} fill={8} />
          </section>

          <section className="surface inspector-block">
            <div className="surface__head">
              <div>
                <span className="eyebrow">Session</span>
                <h3>Working state</h3>
              </div>
              <span className="tag">{liveRunning ? "Monitor" : "Clean"}</span>
            </div>

            <dl className="session-list">
              <div>
                <dt>Preset</dt>
                <dd>{currentPreset.title}</dd>
              </div>
              <div>
                <dt>Seat</dt>
                <dd>{seat.label}</dd>
              </div>
              <div>
                <dt>Output</dt>
                <dd>{currentOutput.label}</dd>
              </div>
            </dl>
          </section>

          <section className="surface inspector-block">
            <div className="surface__head">
              <div>
                <span className="eyebrow">Activity</span>
                <h3>Recent events</h3>
              </div>
              <input
                className="log-filter"
                value={logQuery}
                onChange={(event) => setLogQuery(event.target.value)}
                placeholder="Filter log"
              />
            </div>

            <div className="log-list">
              {filteredLogs.map((entry) => (
                <article key={entry.id} className={`log-item log-item--${entry.level}`}>
                  <div className="log-item__head">
                    <strong>{entry.title}</strong>
                    <span>{entry.time}</span>
                  </div>
                  <p>{entry.body}</p>
                </article>
              ))}
            </div>
          </section>
        </aside>
      </main>
    </div>
  );
}

type StatPillProps = {
  label: string;
  value: string;
  inverse: boolean;
};

function StatPill({ label, value, inverse }: StatPillProps) {
  return (
    <div className={`stat-pill ${inverse ? "stat-pill--inverse" : ""}`}>
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

type ControlFieldProps = {
  label: string;
  value: string;
  detail: string;
  options: DeviceOption[];
  onChange: (value: string) => void;
};

function ControlField({ label, value, detail, options, onChange }: ControlFieldProps) {
  return (
    <label className="control-field">
      <span>{label}</span>
      <div className="control-field__box">
        <select value={value} onChange={(event) => onChange(event.target.value)}>
          {options.map((option) => (
            <option key={option.id} value={option.id}>
              {option.label}
            </option>
          ))}
        </select>
        <small>{detail}</small>
      </div>
    </label>
  );
}

type ToggleProps = {
  label: string;
  checked: boolean;
  onChange: (checked: boolean) => void;
};

function Toggle({ label, checked, onChange }: ToggleProps) {
  return (
    <button
      type="button"
      className={`toggle ${checked ? "is-on" : ""}`}
      onClick={() => onChange(!checked)}
    >
      <span>{label}</span>
      <span className="toggle__knob" />
    </button>
  );
}

type RouteStepProps = {
  index: string;
  title: string;
  body: string;
};

function RouteStep({ index, title, body }: RouteStepProps) {
  return (
    <div className="route-step">
      <span className="route-step__index">{index}</span>
      <div>
        <strong>{title}</strong>
        <p>{body}</p>
      </div>
    </div>
  );
}

type FactCardProps = {
  label: string;
  value: string;
};

function FactCard({ label, value }: FactCardProps) {
  return (
    <div className="fact-card">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

type SliderProps = {
  label: string;
  value: number;
  onChange: (value: number) => void;
};

function Slider({ label, value, onChange }: SliderProps) {
  return (
    <label className="slider">
      <span>{label}</span>
      <input
        type="range"
        min="0"
        max="1"
        step="0.01"
        value={value}
        onChange={(event) => onChange(Number(event.target.value))}
      />
    </label>
  );
}

type MetricProps = {
  label: string;
  value: string;
  fill: number;
};

function Metric({ label, value, fill }: MetricProps) {
  return (
    <div className="metric">
      <div className="metric__copy">
        <span>{label}</span>
        <strong>{value}</strong>
      </div>
      <div className="metric__bar">
        <span style={{ width: `${fill}%` }} />
      </div>
    </div>
  );
}

export default App;

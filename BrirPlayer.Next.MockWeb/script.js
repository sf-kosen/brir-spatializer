const seatGrid = document.getElementById("seat-grid");
const seatValue = document.getElementById("seat-value");
const journalSeat = document.getElementById("journal-seat");
const seatXSlider = document.getElementById("seat-x");
const seatYSlider = document.getElementById("seat-y");
const presetList = document.getElementById("preset-list");
const presetTitle = document.getElementById("preset-title");
const presetSummary = document.getElementById("preset-summary");
const profileValue = document.getElementById("profile-value");
const journalPreset = document.getElementById("journal-preset");
const modeSwitch = document.getElementById("mode-switch");
const activeModeText = document.getElementById("active-mode-text");
const pathBadge = document.getElementById("path-badge");
const pathSource = document.getElementById("path-source");
const pathProcessing = document.getElementById("path-processing");
const pathDestination = document.getElementById("path-destination");
const inputSelect = document.getElementById("input-select");
const inputDetail = document.getElementById("input-detail");
const outputSelect = document.getElementById("output-select");
const outputDetail = document.getElementById("output-detail");
const convertButton = document.getElementById("convert-button");
const convertStatus = document.getElementById("convert-status");
const journalChip = document.getElementById("journal-chip");
const startButton = document.getElementById("start-button");
const sessionState = document.getElementById("session-state");
const journalOutput = document.getElementById("journal-output");
const bufferFill = document.getElementById("buffer-fill");
const xrunCount = document.getElementById("xrun-count");
const xrunFill = document.getElementById("xrun-fill");

const state = {
  seatX: 0.5,
  seatY: 0.5,
  liveRunning: false,
  xruns: 0,
};

const modeConfig = {
  "Virtual Cable": {
    badge: "VB-CABLE",
    source: "Browser audio from YouTube",
    processing: "Shared BRIR live engine",
    destination: "Headphones on Realtek output",
    inputName: "CABLE Output",
    inputDetail: "48 kHz / stereo capture",
    outputName: "Realtek Speakers",
    outputDetail: "48 kHz / exclusive ready",
  },
  Loopback: {
    badge: "Loopback",
    source: "Default render device",
    processing: "Loopback capture and BRIR monitor",
    destination: "Dedicated monitoring output",
    inputName: "Default Render",
    inputDetail: "WASAPI loopback",
    outputName: "DAC Headphones",
    outputDetail: "Shared or exclusive",
  },
  "File Play": {
    badge: "File",
    source: "Decoded local source",
    processing: "Realtime audition with the same BRIR core",
    destination: "Reference listening output",
    inputName: "Local Audio File",
    inputDetail: "wav / flac / mp3 / m4a",
    outputName: "Realtek Speakers",
    outputDetail: "48 kHz playback",
  },
  "Convert File": {
    badge: "Offline",
    source: "Local file batch",
    processing: "Offline render with exact export chain",
    destination: "Processed wav export",
    inputName: "File Queue",
    inputDetail: "wav / flac / mp3 / m4a",
    outputName: "WAV Export",
    outputDetail: "48 kHz stereo file",
  },
};

function renderSeatGrid() {
  seatGrid.innerHTML = "";
  const activeRow = Math.round(state.seatY * 4);
  const activeCol = Math.round(state.seatX * 4);

  for (let row = 0; row < 5; row += 1) {
    for (let col = 0; col < 5; col += 1) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "seat-node";

      if (row === activeRow && col === activeCol) {
        button.classList.add("is-active");
      }

      button.innerHTML = `<span class="seat-label">${row}:${col}</span>`;
      button.addEventListener("click", () => {
        state.seatX = col / 4;
        state.seatY = row / 4;
        seatXSlider.value = String(state.seatX);
        seatYSlider.value = String(state.seatY);
        syncSeatLabels();
        renderSeatGrid();
      });

      seatGrid.appendChild(button);
    }
  }
}

function syncSeatLabels() {
  const row = Math.round(state.seatY * 4);
  const col = Math.round(state.seatX * 4);
  seatValue.textContent = `X ${state.seatX.toFixed(2)} / Y ${state.seatY.toFixed(2)}`;
  journalSeat.textContent = `seat ${row}:${col}`;
}

function applyMode(mode) {
  const config = modeConfig[mode];
  if (!config) {
    return;
  }

  activeModeText.textContent = mode;
  pathBadge.textContent = config.badge;
  pathSource.textContent = config.source;
  pathProcessing.textContent = config.processing;
  pathDestination.textContent = config.destination;
  inputSelect.value = config.inputName;
  inputDetail.textContent = config.inputDetail;
  outputSelect.value = config.outputName;
  outputDetail.textContent = config.outputDetail;
  journalOutput.textContent = config.outputName;
}

inputSelect.addEventListener("change", () => {
  const selected = inputSelect.value;
  pathSource.textContent =
    selected === "Local Audio File" || selected === "File Queue"
      ? "Local source selected in app"
      : selected;
});

outputSelect.addEventListener("change", () => {
  const selected = outputSelect.value;
  journalOutput.textContent = selected;
  pathDestination.textContent =
    selected === "WAV Export" ? "Processed wav export" : `${selected} destination`;
});

function setPreset(button) {
  presetList.querySelectorAll(".preset-card").forEach((card) => {
    card.classList.remove("is-active");
  });

  button.classList.add("is-active");
  presetTitle.textContent = button.dataset.title ?? "";
  presetSummary.textContent = button.dataset.summary ?? "";
  profileValue.textContent = button.dataset.profile ?? "";
  journalPreset.textContent = button.dataset.title ?? "";
}

seatXSlider.addEventListener("input", (event) => {
  state.seatX = Number(event.target.value);
  syncSeatLabels();
  renderSeatGrid();
});

seatYSlider.addEventListener("input", (event) => {
  state.seatY = Number(event.target.value);
  syncSeatLabels();
  renderSeatGrid();
});

presetList.addEventListener("click", (event) => {
  const button = event.target.closest(".preset-card");
  if (!button) {
    return;
  }

  setPreset(button);
});

modeSwitch.addEventListener("click", (event) => {
  const button = event.target.closest(".mode-chip");
  if (!button) {
    return;
  }

  modeSwitch.querySelectorAll(".mode-chip").forEach((chip) => {
    chip.classList.remove("is-active");
  });

  button.classList.add("is-active");
  applyMode(button.dataset.mode ?? "Virtual Cable");
});

convertButton.addEventListener("click", () => {
  convertStatus.textContent = "Rendering";
  journalChip.textContent = "Busy";
  sessionState.textContent = "Converting";

  window.setTimeout(() => {
    convertStatus.textContent = "Complete";
    journalChip.textContent = state.liveRunning ? "Monitor" : "Clean";
    sessionState.textContent = state.liveRunning ? "Live On" : "Idle";
  }, 1200);
});

startButton.addEventListener("click", () => {
  state.liveRunning = !state.liveRunning;
  startButton.textContent = state.liveRunning ? "Stop Live" : "Start Live";
  sessionState.textContent = state.liveRunning ? "Live On" : "Idle";
  journalChip.textContent = state.liveRunning ? "Monitor" : "Clean";
  document.body.classList.toggle("is-live", state.liveRunning);
  bufferFill.style.setProperty("--fill", state.liveRunning ? "74%" : "68%");
});

window.setInterval(() => {
  if (!state.liveRunning) {
    return;
  }

  const jitter = 66 + Math.round(Math.random() * 14);
  const xrun = Math.random() > 0.92 ? 1 : 0;
  state.xruns += xrun;
  bufferFill.style.setProperty("--fill", `${jitter}%`);
  xrunCount.textContent = String(state.xruns);
  xrunFill.style.setProperty("--fill", `${Math.min(8 + state.xruns * 3, 44)}%`);
}, 1400);

syncSeatLabels();
renderSeatGrid();
applyMode("Virtual Cable");

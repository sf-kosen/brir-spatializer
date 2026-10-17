"""Generate binaural room impulse responses for the centre seat of a cinema.

The output is a "cinema set": one 2-channel WAV (left ear, right ear) per
speaker feed (L, C, R, Ls, Rs) plus cinema.json, ready for the BRIR Cinema app.

Model
-----
Early part (direct sound up to ~80 ms after it): image-source method for a
shoebox room. Every image is rendered in the frequency domain with
  - the HRIR for its arrival direction (nearest measured direction),
  - wall absorption per octave band for each reflection,
  - air absorption and 1/r spreading,
  - the speaker's frequency-dependent directivity towards that image,
  - an exact fractional delay.

Late part: a diffuse field. Independent noise from many directions is passed
through the corresponding HRIRs (so interaural coherence matches a diffuse field
for this head), split into octave bands and shaped with each band's
exponential decay (Eyring reverberation time, incl. air absorption). Its level
follows statistical room acoustics: squared impulse response envelope
4*pi*c / (V * Q) per second, where Q is the speaker's directivity factor.

Calibration mimics a real cinema alignment, using an omnidirectional "measurement
microphone" render of the same scene:
  - each feed's steady-state response is equalised to the target curve
    (SMPTE X-curve by default),
  - screen channels are level-matched, surround arrays sit 3 dB below,
  - surround arrays are delayed so their sound arrives with the screen sound,
  - the common propagation delay is removed (no added latency),
  - overall gain: the C feed carries 0.5 energy per ear, so a centred source
    plays back at about its input level.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import soundfile as sf
import sofar
from scipy.spatial import cKDTree

C_SOUND = 343.0
BANDS = np.array([63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000], dtype=float)
# Air absorption, energy attenuation per metre (20 C, ~50% RH).
AIR_M = np.array([0.00002, 0.00008, 0.0003, 0.0006, 0.0010, 0.0024, 0.0076, 0.026, 0.09])


# --------------------------------------------------------------------------- scene


@dataclass
class Speaker:
    name: str
    position: np.ndarray
    aim: np.ndarray  # point the speaker is aimed at
    directivity: np.ndarray  # per-band exponent p of ((1 + cos)/2)^p
    highpass_hz: float


@dataclass
class Room:
    size: np.ndarray  # x (width), y (depth from screen), z (height)
    # Absorption per band for walls: x=0, x=W, y=0 (screen wall), y=L, floor, ceiling.
    absorption: dict[str, np.ndarray]

    @property
    def volume(self) -> float:
        return float(np.prod(self.size))

    def surfaces(self) -> dict[str, float]:
        w, l, h = self.size
        return {"x0": l * h, "x1": l * h, "y0": w * h, "y1": w * h, "z0": w * l, "z1": w * l}


@dataclass
class Cinema:
    name: str
    room: Room
    listener: np.ndarray
    feeds: dict[str, list[Speaker]]
    surround_feeds: tuple[str, ...] = ("Ls", "Rs")
    surround_level_db: float = -3.0
    target_curve: str = "xcurve-small"
    timbre: str = "neutral"  # "neutral": flatten the overall tilt; "cinema": keep it
    metadata: dict = field(default_factory=dict)


def q_to_exponent(q: np.ndarray) -> np.ndarray:
    """Exponent p of the ((1 + cos) / 2)^p pattern with directivity factor Q (Q = 2p + 1)."""
    return (q - 1.0) / 2.0


PRESETS = {
    # Middle seat (two thirds back) of a medium auditorium: screen L/R at ~18 deg.
    "center_medium": dict(name="中央席・中規模シアター", seat_depth=2 / 3, screen_half_width=5.0),
    # Front-middle seat of a large-format auditorium (wide screen, seat halfway
    # back): screen L/R at ~30 deg like a stereo triangle, more direct sound.
    "front_large": dict(name="前方中央席・大型スクリーン", seat_depth=0.45, screen_half_width=6.5),
}


def default_cinema(name: str = "中央席・中規模シアター", seat_depth: float = 2 / 3,
                   screen_half_width: float = 5.0) -> Cinema:
    w, l, h = 16.0, 24.0, 8.0
    wall = np.array([0.45, 0.50, 0.50, 0.50, 0.50, 0.50, 0.50, 0.45, 0.40])
    audience = np.array([0.35, 0.50, 0.65, 0.75, 0.80, 0.80, 0.80, 0.75, 0.70])
    ceiling = np.array([0.20, 0.25, 0.30, 0.35, 0.35, 0.35, 0.35, 0.30, 0.30])
    room = Room(
        size=np.array([w, l, h]),
        absorption={"x0": wall, "x1": wall, "y0": wall, "y1": wall, "z0": audience, "z1": ceiling},
    )
    # Centre seat two thirds back; stadium seating puts the ears ~3.2 m up.
    listener = np.array([w / 2, l * seat_depth, 3.2])

    # Directivity factor Q per band. Screen channels are horn-loaded cinema
    # speakers (DI ~10-13 dB in the speech range), which is what keeps dialogue
    # crisp in a real auditorium; surrounds are wide-dispersion boxes.
    screen_p = q_to_exponent(np.array([1.0, 1.5, 2.5, 5.0, 10.0, 16.0, 20.0, 25.0, 30.0]))
    surround_p = q_to_exponent(np.array([1.0, 1.2, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 6.0]))
    screen_y, screen_z = 0.6, 3.5
    feeds: dict[str, list[Speaker]] = {}
    for feed, x in (("L", w / 2 - screen_half_width), ("C", w / 2), ("R", w / 2 + screen_half_width)):
        feeds[feed] = [Speaker(feed, np.array([x, screen_y, screen_z]), listener, screen_p, 35.0)]

    # Side arrays from a third of the depth to the back, high on the walls,
    # aimed across the room at the audience; rear wall array split left/right.
    side_ys = np.linspace(l * 0.35, l - 1.5, 5)
    surround_z = 5.5
    left, right = [], []
    for i, y in enumerate(side_ys):
        left.append(Speaker(f"Lss{i}", np.array([0.3, y, surround_z]), np.array([w * 0.75, y, 2.5]), surround_p, 60.0))
        right.append(Speaker(f"Rss{i}", np.array([w - 0.3, y, surround_z]), np.array([w * 0.25, y, 2.5]), surround_p, 60.0))
    for i, x in enumerate(np.linspace(w * 0.15, w * 0.85, 4)):
        spk = Speaker(f"Rs{i}", np.array([x, l - 0.3, surround_z]), np.array([x, l * 0.4, 2.5]), surround_p, 60.0)
        (left if x < w / 2 else right).append(spk)
    feeds["Ls"] = left
    feeds["Rs"] = right
    return Cinema(name, room, listener, feeds)


# ----------------------------------------------------------------------- acoustics


def eyring_rt60(room: Room) -> np.ndarray:
    surfaces = room.surfaces()
    total = sum(surfaces.values())
    mean_alpha = sum(room.absorption[k] * s for k, s in surfaces.items()) / total
    return 0.161 * room.volume / (-total * np.log(1.0 - mean_alpha) + 4.0 * AIR_M * room.volume)


def directivity_gain(speaker: Speaker, direction: np.ndarray) -> np.ndarray:
    """Per-band amplitude gain of the speaker towards a unit direction."""
    axis = speaker.aim - speaker.position
    axis = axis / np.linalg.norm(axis)
    cos = float(np.clip(np.dot(axis, direction), -1.0, 1.0))
    return ((1.0 + cos) / 2.0) ** speaker.directivity


def directivity_factor(exponents: np.ndarray) -> np.ndarray:
    """Q per band: on-axis intensity over the spherical mean, for ((1+cos)/2)^p."""
    cos = np.linspace(-1.0, 1.0, 4001)
    mean = np.array([np.trapezoid(((1 + cos) / 2) ** (2 * p), cos) / 2.0 for p in exponents])
    return 1.0 / mean


def image_sources(room: Room, source: np.ndarray, listener: np.ndarray, max_time: float):
    """Yield (image position, reflection counts per wall, axis flips) up to max_time."""
    size = room.size
    reach = max_time * C_SOUND
    ranges = [range(-int(reach / (2 * s)) - 1, int(reach / (2 * s)) + 2) for s in size]
    for jx in ranges[0]:
        for qx in (0, 1):
            x = (1 - 2 * qx) * source[0] + 2 * jx * size[0]
            if abs(x - listener[0]) > reach:
                continue
            for jy in ranges[1]:
                for qy in (0, 1):
                    y = (1 - 2 * qy) * source[1] + 2 * jy * size[1]
                    if abs(y - listener[1]) > reach:
                        continue
                    for jz in ranges[2]:
                        for qz in (0, 1):
                            z = (1 - 2 * qz) * source[2] + 2 * jz * size[2]
                            pos = np.array([x, y, z])
                            if np.linalg.norm(pos - listener) > reach:
                                continue
                            hits = {
                                "x0": abs(jx - qx), "x1": abs(jx),
                                "y0": abs(jy - qy), "y1": abs(jy),
                                "z0": abs(jz - qz), "z1": abs(jz),
                            }
                            yield pos, hits, np.array([1 - 2 * qx, 1 - 2 * qy, 1 - 2 * qz])


# ------------------------------------------------------------------------- HRTFs


class Hrtf:
    def __init__(self, path: Path, fs: int):
        sofa = sofar.read_sofa(str(path), verify=False)
        if int(sofa.Data_SamplingRate) != fs:
            raise ValueError(f"HRTF is {sofa.Data_SamplingRate} Hz, need {fs} Hz")
        pos = np.asarray(sofa.SourcePosition, dtype=float)
        az, el = np.radians(pos[:, 0]), np.radians(pos[:, 1])
        # SOFA spherical: azimuth counter-clockwise from front (left = +90), x front, y left, z up.
        self.dirs = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], axis=1)
        irs = np.asarray(sofa.Data_IR, dtype=float)  # (M, 2, N)
        self.tree = cKDTree(self.dirs)
        # Symmetrise: average each ear with the opposite ear of the mirrored
        # direction. Removes measurement mismatch between the two ear capsules,
        # which would otherwise pull centred sources sideways.
        _, mirror = self.tree.query(self.dirs * np.array([1.0, -1.0, 1.0]))
        irs = 0.5 * (irs + irs[mirror][:, ::-1, :])
        self.irs = diffuse_field_equalise(irs, self.dirs, fs)

    def nearest(self, direction: np.ndarray) -> np.ndarray:
        _, index = self.tree.query(direction / np.linalg.norm(direction))
        return self.irs[index]


def diffuse_field_equalise(irs: np.ndarray, dirs: np.ndarray, fs: int, taps: int = 256) -> np.ndarray:
    """Remove the head's average response from every HRIR.

    Headphones are tuned so that some reference field sounds neutral; binaural
    signals should carry only the direction-dependent part of the HRTF, or the
    head's own average response colours everything. The reference here is the
    dB-average of the diffuse-field and the frontal response: pure diffuse-field
    equalisation leaves the frontal pinna dip (~8 kHz) on centre dialogue, pure
    frontal equalisation over-brightens the reverb. Normalised to 0 dB at 1 kHz;
    the correction is limited to -6..+12 dB and fixed below 100 Hz.
    """
    n_fft = 4096
    spectra = np.fft.rfft(irs, n_fft, axis=-1)
    diffuse = np.mean(np.abs(spectra) ** 2, axis=(0, 1))  # all directions, both ears
    front = int(np.argmax(dirs @ np.array([1.0, 0.0, 0.0])))
    frontal = np.mean(np.abs(spectra[front]) ** 2, axis=0)
    power = np.sqrt(diffuse * frontal)
    freqs = np.fft.rfftfreq(n_fft, 1.0 / fs)
    smooth = third_octave_smooth(freqs, power)
    ref = smooth[np.searchsorted(freqs, 1000.0)]
    correction_db = np.clip(-10 * np.log10(smooth / ref), -6.0, 12.0)
    correction_db[freqs < 100.0] = correction_db[np.searchsorted(freqs, 100.0)]
    fir = minimum_phase_fir(10 ** (correction_db / 20), n_fft, taps)
    eq = np.fft.rfft(fir, n_fft)
    out = np.fft.irfft(spectra * eq, n_fft, axis=-1)
    return out[..., : irs.shape[-1] + taps]


def to_head_frame(v: np.ndarray) -> np.ndarray:
    """Room vector -> listener frame. Listener faces the screen (-y); x=0 wall is on the left."""
    return np.array([-v[1], -v[0], v[2]])


# -------------------------------------------------------------------------- render


def band_curve(freqs: np.ndarray, band_values: np.ndarray) -> np.ndarray:
    """Interpolate per-octave values smoothly over frequency (log axis), held at the ends."""
    logf = np.log2(np.maximum(freqs, 1.0))
    return np.interp(logf, np.log2(BANDS), band_values)


def render_early(cinema: Cinema, speaker: Speaker, hrtf: Hrtf | None, fs: int, n_fft: int,
                 end_time: float) -> np.ndarray:
    """Early response (0 .. end_time) for one speaker; (2, n_fft) binaural or (1, n_fft) omni."""
    room, listener = cinema.room, cinema.listener
    freqs = np.fft.rfftfreq(n_fft, 1.0 / fs)
    channels = 2 if hrtf else 1
    spectrum = np.zeros((channels, freqs.size), dtype=complex)
    for image, hits, flips in image_sources(room, speaker.position, listener, end_time):
        vec = listener - image
        dist = float(np.linalg.norm(vec))
        delay = dist / C_SOUND
        if delay > end_time:
            continue
        # Direction the real speaker radiated in: mirror back through the flips.
        emitted = (vec / dist) * flips
        gains = directivity_gain(speaker, emitted) / dist
        gains = gains * np.exp(-AIR_M * dist / 2.0)
        for wall, count in hits.items():
            if count:
                gains = gains * np.sqrt(1.0 - room.absorption[wall]) ** count
        response = band_curve(freqs, gains) * np.exp(-2j * np.pi * freqs * delay)
        if hrtf:
            hrir = hrtf.nearest(to_head_frame(image - listener))
            spectrum += np.fft.rfft(hrir, n_fft) * response
        else:
            spectrum[0] += response
    return np.fft.irfft(spectrum, n_fft)


def fibonacci_sphere(n: int) -> np.ndarray:
    i = np.arange(n) + 0.5
    phi = np.arccos(1 - 2 * i / n)
    theta = np.pi * (1 + 5 ** 0.5) * i
    return np.stack([np.cos(theta) * np.sin(phi), np.sin(theta) * np.sin(phi), np.cos(phi)], axis=1)


def band_masks(freqs: np.ndarray) -> np.ndarray:
    """Zero-phase octave masks with raised-cosine crossovers in log frequency, summing to 1."""
    logf = np.log2(np.maximum(freqs, 1.0))
    centres = np.log2(BANDS)
    masks = np.zeros((BANDS.size, freqs.size))
    for b in range(BANDS.size):
        if b == 0:
            lower = np.ones_like(logf)
        else:
            t = np.clip((logf - centres[b - 1]) / (centres[b] - centres[b - 1]), 0, 1)
            lower = np.sin(np.pi / 2 * t) ** 2
        if b == BANDS.size - 1:
            upper = np.ones_like(logf)
        else:
            t = np.clip((logf - centres[b]) / (centres[b + 1] - centres[b]), 0, 1)
            upper = np.cos(np.pi / 2 * t) ** 2
        masks[b] = np.where(logf <= centres[b], lower, upper)
    return masks


class DiffuseField:
    """Second-order statistics of a diffuse field at the two ears.

    Ear spectra of a diffuse field are Gaussian with power P_L, P_R and cross
    spectrum C = E[H_L conj(H_R)] over uniformly spread directions. Drawing
    X_L = sqrt(P_L) N1 and X_R = conj(C)/sqrt(P_L) N1 + sqrt(P_R - |C|^2/P_L) N2
    from independent complex Gaussians N1, N2 reproduces those statistics
    exactly, without summing hundreds of filtered noise sources.
    """

    def __init__(self, hrtf: Hrtf, length: int, directions: int = 2048):
        spectra = np.array([np.fft.rfft(hrtf.nearest(d), length) for d in fibonacci_sphere(directions)])
        p_l = np.mean(np.abs(spectra[:, 0]) ** 2, axis=0)
        p_r = np.mean(np.abs(spectra[:, 1]) ** 2, axis=0)
        cross = np.mean(spectra[:, 0] * np.conj(spectra[:, 1]), axis=0)
        self.a = np.sqrt(p_l)
        self.b = np.conj(cross) / np.maximum(self.a, 1e-12)
        self.c = np.sqrt(np.maximum(p_r - np.abs(cross) ** 2 / np.maximum(p_l, 1e-24), 0.0))
        self.length = length

    def draw(self, rng: np.random.Generator) -> np.ndarray:
        bins = self.a.size
        # Complex Gaussian with the same per-bin power as the rfft of unit white noise.
        scale = math.sqrt(self.length / 2.0)
        n1 = (rng.standard_normal(bins) + 1j * rng.standard_normal(bins)) * scale
        n2 = (rng.standard_normal(bins) + 1j * rng.standard_normal(bins)) * scale
        return np.stack([np.fft.irfft(self.a * n1, self.length), np.fft.irfft(self.b * n1 + self.c * n2, self.length)])


def render_tail(cinema: Cinema, speaker: Speaker, diffuse: DiffuseField | None, fs: int, length: int,
                rng: np.random.Generator, fade_in: np.ndarray) -> np.ndarray:
    """Diffuse late field, already multiplied by fade_in (the crossfade from the early part)."""
    rt60 = eyring_rt60(cinema.room)
    q = directivity_factor(speaker.directivity)
    noise = diffuse.draw(rng) if diffuse else rng.standard_normal((1, length))

    t = np.arange(length) / fs
    freqs = np.fft.rfftfreq(length, 1.0 / fs)
    masks = band_masks(freqs)
    out = np.zeros_like(noise)
    # Expected energy of each ear's band for this noise model, so a single random
    # draw doesn't leave one ear or band louder than the other by chance.
    if diffuse:
        ear_power = [np.abs(diffuse.a) ** 2, np.abs(diffuse.b) ** 2 + diffuse.c ** 2]
    else:
        ear_power = [np.ones(freqs.size)]
    for ch in range(noise.shape[0]):
        spectrum = np.fft.rfft(noise[ch])
        for b in range(BANDS.size):
            # Squared-envelope 4*pi*c/(V*Q) per second -> per sample.
            level = math.sqrt(4 * math.pi * C_SOUND / (cinema.room.volume * q[b] * fs))
            envelope = level * 10 ** (-3.0 * t / rt60[b]) * fade_in
            band = np.fft.irfft(spectrum * masks[b], length) * envelope
            # Normalise the part actually used (after the crossfade, with the
            # decay applied): its energy sits in a short window right after the
            # mixing time, where a single random draw varies most.
            per_sample = 2.0 * np.sum(ear_power[ch] * masks[b] ** 2) / length
            expected = per_sample * float(np.sum(envelope * envelope))
            out[ch] += band * math.sqrt(expected / max(float(np.sum(band * band)), 1e-30))
    return out


def render_speaker(cinema: Cinema, speaker: Speaker, hrtf: Hrtf | None, diffuse: DiffuseField | None,
                   fs: int, length: int, rng: np.random.Generator, early_after_direct: float = 0.080,
                   crossfade: float = 0.020) -> np.ndarray:
    direct = float(np.linalg.norm(cinema.listener - speaker.position)) / C_SOUND
    mix_time = direct + early_after_direct
    n_early = 1 << int(math.ceil(math.log2((mix_time + crossfade + 0.01) * fs)))
    early = render_early(cinema, speaker, hrtf, fs, n_early, mix_time + crossfade)
    # Power-complementary crossfade from the image sources to the diffuse field.
    t = np.arange(length) / fs
    x = np.clip((t - (mix_time - crossfade / 2)) / crossfade, 0, 1)
    fade_in, fade_out = np.sin(np.pi / 2 * x), np.cos(np.pi / 2 * x)
    out = render_tail(cinema, speaker, diffuse, fs, length, rng, fade_in)
    n = min(n_early, length)
    out[:, :n] += early[:, :n] * fade_out[:n]
    return highpass(out, speaker.highpass_hz, fs)


def highpass(x: np.ndarray, hz: float, fs: int, order: int = 4) -> np.ndarray:
    from scipy.signal import butter, sosfilt

    sos = butter(order, hz, "highpass", fs=fs, output="sos")
    return sosfilt(sos, x, axis=-1)


# --------------------------------------------------------------------- calibration


def target_curve(freqs: np.ndarray, kind: str) -> np.ndarray:
    """Magnitude target in dB relative to the 500 Hz - 2 kHz region."""
    f = np.maximum(freqs, 1.0)
    if kind == "flat":
        return np.zeros_like(f)
    slope = {"xcurve": -3.0, "xcurve-small": -1.5}[kind]
    db = np.where(f > 2000.0, slope * np.log2(f / 2000.0), 0.0)
    return np.where(f > 10000.0, db + slope * np.log2(f / 10000.0), db)  # extra roll-off above 10 kHz


def third_octave_smooth(freqs: np.ndarray, power: np.ndarray) -> np.ndarray:
    out = np.empty_like(power)
    lo, hi = freqs / 2 ** (1 / 6), freqs * 2 ** (1 / 6)
    cumulative = np.concatenate([[0.0], np.cumsum(power)])
    left = np.searchsorted(freqs, lo)
    right = np.searchsorted(freqs, hi, side="right")
    right = np.maximum(right, left + 1)
    out[:] = (cumulative[right] - cumulative[left]) / (right - left)
    return out


def band_level(freqs: np.ndarray, power: np.ndarray, lo: float = 500.0, hi: float = 2000.0) -> float:
    sel = (freqs >= lo) & (freqs <= hi)
    return float(np.mean(power[sel]))


def minimum_phase_fir(magnitude: np.ndarray, n_fft: int, taps: int) -> np.ndarray:
    """Minimum-phase FIR for an rfft-grid magnitude (homomorphic method)."""
    log_mag = np.log(np.maximum(magnitude, 1e-6))
    cepstrum = np.fft.irfft(log_mag, n_fft)
    fold = np.zeros(n_fft)
    fold[0] = cepstrum[0]
    fold[1 : n_fft // 2] = 2 * cepstrum[1 : n_fft // 2]
    fold[n_fft // 2] = cepstrum[n_fft // 2]
    h = np.fft.irfft(np.exp(np.fft.rfft(fold)), n_fft)[:taps]
    return h * np.hanning(2 * taps)[taps:]


def calibration_filter(omni: np.ndarray, fs: int, kind: str, n_fft: int = 65536,
                       eq_range=(63.0, 16000.0), max_db: float = 10.0):
    """EQ that brings the omni steady-state response to the target, and its 500-2k level."""
    freqs = np.fft.rfftfreq(n_fft, 1.0 / fs)
    power = third_octave_smooth(freqs, np.abs(np.fft.rfft(omni, n_fft)) ** 2)
    ref = band_level(freqs, power)
    measured_db = 10 * np.log10(np.maximum(power / ref, 1e-12))
    correction = np.clip(target_curve(freqs, kind) - measured_db, -max_db, max_db)
    # Hold the correction flat outside the range we equalise.
    lo, hi = np.searchsorted(freqs, eq_range[0]), np.searchsorted(freqs, eq_range[1])
    correction[:lo] = correction[lo]
    correction[hi:] = correction[hi - 1]
    return minimum_phase_fir(10 ** (correction / 20), n_fft, 4096), ref


# How much each feed typically carries after upmixing film audio.
PROGRAMME_WEIGHTS = {"C": 0.5, "L": 0.2, "R": 0.2, "Ls": 0.05, "Rs": 0.05}


def timbre_correction(binaural: dict[str, np.ndarray], fs: int, n_fft: int = 65536,
                      max_db: float = 6.0) -> np.ndarray:
    """Common EQ making the programme-weighted binaural response flat to 8 kHz."""
    freqs = np.fft.rfftfreq(n_fft, 1.0 / fs)
    power = np.zeros(freqs.size)
    for feed, weight in PROGRAMME_WEIGHTS.items():
        if feed in binaural:
            power += weight * np.mean(np.abs(np.fft.rfft(binaural[feed], n_fft, axis=1)) ** 2, axis=0)
    smooth = third_octave_smooth(freqs, power)
    ref = band_level(freqs, smooth, 500.0, 2000.0)
    measured_db = 10 * np.log10(np.maximum(smooth / ref, 1e-12))
    f = np.maximum(freqs, 1.0)
    target_db = np.where(f > 8000.0, -2.0 * np.log2(f / 8000.0), 0.0)  # natural air loss above 8 kHz
    correction = np.clip(target_db - measured_db, -max_db, max_db)
    lo, hi = np.searchsorted(freqs, 40.0), np.searchsorted(freqs, 18000.0)
    correction[:lo] = correction[lo]
    correction[hi:] = correction[hi - 1]
    return minimum_phase_fir(10 ** (correction / 20), n_fft, 4096)


def apply_fir(x: np.ndarray, h: np.ndarray) -> np.ndarray:
    from scipy.signal import fftconvolve

    return fftconvolve(x, h[None, :], axes=-1)[:, : x.shape[-1]]


# ------------------------------------------------------------------------ analysis


def schroeder_rt60(ir: np.ndarray, fs: int) -> np.ndarray:
    """T30-based RT60 per band from an omni impulse response."""
    # Zero-pad so the zero-phase band filters' pre-ringing doesn't wrap onto the tail.
    n = ir.size
    pad = 2 * n
    freqs = np.fft.rfftfreq(pad, 1.0 / fs)
    spectrum = np.fft.rfft(ir, pad)
    out = []
    for mask in band_masks(freqs):
        band = np.fft.irfft(spectrum * mask, pad)[:n]
        edc = np.cumsum(band[::-1] ** 2)[::-1]
        edc_db = 10 * np.log10(np.maximum(edc / edc[0], 1e-12))
        i5, i35 = np.argmax(edc_db <= -5), np.argmax(edc_db <= -35)
        out.append(2.0 * (i35 - i5) / fs if i35 > i5 else float("nan"))
    return np.array(out)


def iacc(binaural: np.ndarray, fs: int, start: float, end: float) -> float:
    a, b = binaural[0, int(start * fs) : int(end * fs)], binaural[1, int(start * fs) : int(end * fs)]
    lag = int(0.001 * fs)
    norm = math.sqrt(np.sum(a * a) * np.sum(b * b)) + 1e-20
    full = np.correlate(a, b, "full")
    mid = len(full) // 2
    return float(np.max(np.abs(full[mid - lag : mid + lag + 1])) / norm)


# --------------------------------------------------------------------------- main


def generate(cinema: Cinema, hrtf_path: Path, out_dir: Path, fs: int = 48000, seed: int = 1) -> None:
    rng = np.random.default_rng(seed)
    hrtf = Hrtf(hrtf_path, fs)
    rt60 = eyring_rt60(cinema.room)
    length = int(math.ceil((min(float(rt60.max()) * 1.5, 3.0) + 0.1) * fs))
    diffuse = DiffuseField(hrtf, length)
    print(f"Room {cinema.room.size} m, V = {cinema.room.volume:.0f} m3")
    print("Eyring RT60  " + "  ".join(f"{b:>5.0f}:{t:.2f}" for b, t in zip(BANDS, rt60)))

    binaural, omni, first_arrival = {}, {}, {}
    for feed, speakers in cinema.feeds.items():
        b_sum = np.zeros((2, length))
        o_sum = np.zeros((1, length))
        for spk in speakers:
            print(f"  rendering {feed}/{spk.name}")
            b_sum += render_speaker(cinema, spk, hrtf, diffuse, fs, length, rng)
            o_sum += render_speaker(cinema, spk, None, None, fs, length, rng)
        binaural[feed], omni[feed] = b_sum, o_sum
        first_arrival[feed] = min(np.linalg.norm(cinema.listener - s.position) for s in speakers) / C_SOUND

    # Equalise and level-align each feed with its omni measurement.
    gains = {}
    for feed in cinema.feeds:
        fir, level = calibration_filter(omni[feed][0], fs, cinema.target_curve)
        offset_db = cinema.surround_level_db if feed in cinema.surround_feeds else 0.0
        gain = 10 ** (offset_db / 20) / math.sqrt(level)
        binaural[feed] = apply_fir(binaural[feed], fir) * gain
        omni[feed] = apply_fir(omni[feed], fir) * gain
        gains[feed] = gain

    # Surround arrays are delayed so they arrive with the screen sound; then the
    # common propagation delay is removed, keeping 2 ms ahead of the first arrival.
    screen_arrival = first_arrival["C"]
    shift = {}
    for feed in cinema.feeds:
        delay = max(0.0, screen_arrival - first_arrival[feed]) if feed in cinema.surround_feeds else 0.0
        shift[feed] = int(round((delay - (screen_arrival - 0.002)) * fs))

    def shifted(x: np.ndarray, s: int) -> np.ndarray:
        out = np.zeros_like(x)
        if s >= 0:
            out[:, s:] = x[:, : x.shape[-1] - s]
        else:
            out[:, : x.shape[-1] + s] = x[:, -s:]
        return out

    for feed in cinema.feeds:
        binaural[feed] = shifted(binaural[feed], shift[feed])
        omni[feed] = shifted(omni[feed], shift[feed])

    # Overall timbre: the room, the calibration curve and the head together tilt
    # the sound (bass up, top down), which reads as "muffled" on headphones,
    # especially with bass-heavy, centred mixes. One common EQ on all feeds
    # flattens the programme-weighted response (C counts most) to 8 kHz, keeping
    # a gentle natural roll-off above. Relative levels and spatial cues between
    # feeds are untouched.
    if cinema.timbre == "neutral":
        fir = timbre_correction(binaural, fs)
        for feed in cinema.feeds:
            binaural[feed] = apply_fir(binaural[feed], fir)
            omni[feed] = apply_fir(omni[feed], fir)

    # Overall gain: for pink noise (a stand-in for programme material) the C feed
    # passes half its input energy to each ear, so a centred source (whose L+R
    # energy all lands in C) plays back at its input level. The mean of |H|^2
    # over rfft bins equals the energy gain for white noise; weighting the bins
    # by 1/f gives it for pink noise.
    spectrum = np.fft.rfft(binaural["C"], axis=1)
    freqs = np.fft.rfftfreq(binaural["C"].shape[1], 1.0 / fs)
    band = (freqs >= 20.0) & (freqs <= 20000.0)
    pink = 1.0 / freqs[band]
    centre_gain = float(np.mean(np.sum(np.abs(spectrum[:, band]) ** 2 * pink, axis=1) / np.sum(pink)))
    norm = math.sqrt(0.5 / centre_gain)
    peak = 0.0
    out_dir.mkdir(parents=True, exist_ok=True)
    files = []
    fade = int(0.05 * fs)
    window = np.ones(length)
    window[-fade:] = np.cos(np.linspace(0, np.pi / 2, fade)) ** 2
    for feed in cinema.feeds:
        data = binaural[feed] * norm * window
        peak = max(peak, float(np.max(np.abs(data))))
        path = out_dir / f"{feed}.wav"
        sf.write(path, data.T.astype(np.float32), fs, subtype="FLOAT")
        files.append({"name": feed, "file": path.name})

    measured = schroeder_rt60(omni["C"][0], fs)
    early_end = 0.002 + 0.080
    report = {
        "eyring_rt60": dict(zip(map(int, BANDS), map(float, rt60))),
        "measured_rt60_C": dict(zip(map(int, BANDS), map(float, measured))),
        "iacc_early_C": iacc(binaural["C"], fs, 0.0, early_end),
        "iacc_late_C": iacc(binaural["C"], fs, early_end, 1.0),
        "surround_delay_ms": {f: (shift[f] / fs + screen_arrival - 0.002) * 1000 for f in cinema.feeds},
        "peak": peak,
    }
    meta = {
        "format": "brir-cinema/1",
        "name": cinema.name,
        "sampleRate": fs,
        "feeds": files,
        "room": {"size_m": cinema.room.size.tolist(), "listener_m": cinema.listener.tolist()},
        "targetCurve": cinema.target_curve,
        "timbre": cinema.timbre,
        "hrtf": hrtf_path.name,
        "report": report,
    }
    (out_dir / "cinema.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")

    print("Measured RT60 (C) " + "  ".join(f"{b:>5.0f}:{t:.2f}" for b, t in zip(BANDS, measured)))
    print(f"IACC early {report['iacc_early_C']:.2f}, late {report['iacc_late_C']:.2f}, peak {peak:.2f}")
    print(f"Wrote {out_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hrtf", type=Path, default=Path("Assets/HRTF/KU100_TH_Koeln/HRIR_FULL2DEG.sofa"))
    parser.add_argument("--preset", choices=sorted(PRESETS), default="center_medium")
    parser.add_argument("--out", type=Path, default=None, help="default: Generated/cinema/<preset>")
    parser.add_argument("--target", choices=["xcurve", "xcurve-small", "flat"], default="xcurve-small")
    parser.add_argument("--timbre", choices=["neutral", "cinema"], default="neutral")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    cinema = default_cinema(**PRESETS[args.preset])
    cinema.target_curve = args.target
    cinema.timbre = args.timbre
    out = args.out or Path("Generated/cinema") / args.preset
    v = cinema.feeds["L"][0].position - cinema.listener
    print(f"{cinema.name}: screen L/R at +-{np.degrees(np.arctan2(abs(v[0]), abs(v[1]))):.0f} deg")
    generate(cinema, args.hrtf, out, seed=args.seed)


if __name__ == "__main__":
    main()

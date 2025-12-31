import argparse
import math
import numpy as np
import pyroomacoustics as pra
import soundfile as sf
import os
from scipy.signal import fftconvolve
import sofar

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
ASSETS_DIR = os.path.join(REPO_ROOT, "Assets")
DEFAULT_OUTPUT_ROOT = os.path.join(REPO_ROOT, "Generated", "BRIR")
DEFAULT_SOFA_PATH = os.path.join(
    ASSETS_DIR,
    "HRTF",
    "D1_HRIR_SOFA",
    "D1_48K_24bit_256tap_FIR_SOFA.sofa"
)


def apply_low_shelf(signal, sample_rate, freq_hz=80.0, gain_db=5.0, q=0.707):
    # RBJ low-shelf biquad, modest boost for cinema-style low end.
    if abs(gain_db) < 1e-6:
        return signal
    a = 10 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * (freq_hz / sample_rate)
    cos_w0 = np.cos(w0)
    sin_w0 = np.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    sqrt_a = np.sqrt(a)

    b0 = a * ((a + 1) - (a - 1) * cos_w0 + 2.0 * sqrt_a * alpha)
    b1 = 2.0 * a * ((a - 1) - (a + 1) * cos_w0)
    b2 = a * ((a + 1) - (a - 1) * cos_w0 - 2.0 * sqrt_a * alpha)
    a0 = (a + 1) + (a - 1) * cos_w0 + 2.0 * sqrt_a * alpha
    a1 = -2.0 * ((a - 1) + (a + 1) * cos_w0)
    a2 = (a + 1) + (a - 1) * cos_w0 - 2.0 * sqrt_a * alpha

    b0 /= a0
    b1 /= a0
    b2 /= a0
    a1 /= a0
    a2 /= a0

    y = np.zeros_like(signal)
    x1 = x2 = y1 = y2 = 0.0
    for i in range(len(signal)):
        x0 = signal[i]
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        y[i] = y0
        x2, x1 = x1, x0
        y2, y1 = y1, y0
    return y


def apply_high_shelf(signal, sample_rate, freq_hz=4000.0, gain_db=-5.0, q=0.707):
    # RBJ high-shelf biquad, gentle roll-off for late tail.
    if abs(gain_db) < 1e-6:
        return signal
    a = 10 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * (freq_hz / sample_rate)
    cos_w0 = np.cos(w0)
    sin_w0 = np.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    sqrt_a = np.sqrt(a)

    b0 = a * ((a + 1) + (a - 1) * cos_w0 + 2.0 * sqrt_a * alpha)
    b1 = -2.0 * a * ((a - 1) + (a + 1) * cos_w0)
    b2 = a * ((a + 1) + (a - 1) * cos_w0 - 2.0 * sqrt_a * alpha)
    a0 = (a + 1) - (a - 1) * cos_w0 + 2.0 * sqrt_a * alpha
    a1 = 2.0 * ((a - 1) - (a + 1) * cos_w0)
    a2 = (a + 1) - (a - 1) * cos_w0 - 2.0 * sqrt_a * alpha

    b0 /= a0
    b1 /= a0
    b2 /= a0
    a1 /= a0
    a2 /= a0

    y = np.zeros_like(signal)
    x1 = x2 = y1 = y2 = 0.0
    for i in range(len(signal)):
        x0 = signal[i]
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        y[i] = y0
        x2, x1 = x1, x0
        y2, y1 = y1, y0
    return y


def apply_allpass(signal, delay_samples=12, feedback=0.45):
    # Simple all-pass for decorrelation; use small delay.
    if delay_samples <= 0:
        return signal
    y = np.zeros_like(signal)
    buf = np.zeros(delay_samples)
    idx = 0
    for i in range(len(signal)):
        x = signal[i]
        d = buf[idx]
        y[i] = -feedback * x + d
        buf[idx] = x + feedback * y[i]
        idx = (idx + 1) % delay_samples
    return y


def apply_mid_side(l, r, k):
    m = 0.5 * (l + r)
    s = 0.5 * (l - r) * k
    return m + s, m - s


def apply_low_crossfeed(l, r, fs, cutoff_hz=700.0, mix=0.08):
    # First-order low-pass crossfeed to soften extreme separation.
    if mix <= 0:
        return l, r
    alpha = math.exp(-2.0 * math.pi * cutoff_hz / fs)
    yl = 0.0
    yr = 0.0
    out_l = np.zeros_like(l)
    out_r = np.zeros_like(r)
    for i in range(len(l)):
        yl = (1.0 - alpha) * l[i] + alpha * yl
        yr = (1.0 - alpha) * r[i] + alpha * yr
        out_l[i] = l[i] + mix * yr
        out_r[i] = r[i] + mix * yl
    return out_l, out_r


def split_early_late(ir, fs, early_ms):
    split = int(round(fs * early_ms / 1000.0))
    split = max(0, min(split, len(ir)))
    early = ir[:split]
    late = ir[split:]
    return early, late


def process_early_late(ir_l, ir_r, fs, early_ms=60.0, late_gain=0.45,
                       late_high_shelf_db=-5.0, ap_delay_l=11, ap_delay_r=17, ap_feedback=0.45,
                       late_side_k=0.65, xfeed_cutoff=700.0, xfeed_mix=0.08):
    # Keep early reflections, smooth and soften late tail.
    early_l, late_l = split_early_late(ir_l, fs, early_ms)
    early_r, late_r = split_early_late(ir_r, fs, early_ms)

    if len(late_l) > 0:
        late_l = late_l * late_gain
        late_l = apply_high_shelf(late_l, fs, freq_hz=4000.0, gain_db=late_high_shelf_db, q=0.707)
        late_l = apply_allpass(late_l, delay_samples=ap_delay_l, feedback=ap_feedback)
    if len(late_r) > 0:
        late_r = late_r * late_gain
        late_r = apply_high_shelf(late_r, fs, freq_hz=4000.0, gain_db=late_high_shelf_db, q=0.707)
        late_r = apply_allpass(late_r, delay_samples=ap_delay_r, feedback=ap_feedback)

    if len(late_l) > 0:
        late_l, late_r = apply_mid_side(late_l, late_r, late_side_k)
        late_l, late_r = apply_low_crossfeed(late_l, late_r, fs, cutoff_hz=xfeed_cutoff, mix=xfeed_mix)

    out_l = np.concatenate([early_l, late_l])
    out_r = np.concatenate([early_r, late_r])
    return out_l, out_r


def apply_fade_out(signal, fs, fade_ms=170.0):
    fade_len = int(round(fs * fade_ms / 1000.0))
    if fade_len <= 0 or fade_len > len(signal):
        return signal
    fade = np.linspace(1.0, 0.0, fade_len, dtype=signal.dtype)
    signal = signal.copy()
    if signal.ndim == 1:
        signal[-fade_len:] *= fade
    else:
        signal[-fade_len:, :] *= fade[:, None]
    return signal


def pad_or_truncate(signal, target_len):
    if len(signal) < target_len:
        return np.pad(signal, (0, target_len - len(signal)), 'constant')
    return signal[:target_len]


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


def lerp(a, b, t):
    return a + (b - a) * t


def clamp01(value):
    return clamp(value, 0.0, 1.0)


def seat_z_from_profile(y, profile):
    if not profile:
        return 1.2

    y_stand0 = profile["y_stand0"]
    y_max = profile["y_max"]
    if y < y_stand0:
        return profile["floor_z"]

    stand_depth = max(y_max - y_stand0, 1e-6)
    s1 = y_stand0 + profile["lower_ratio"] * stand_depth
    s2 = y_stand0 + profile["middle_ratio"] * stand_depth
    s3 = y_max

    if y <= s1:
        t = clamp01((y - y_stand0) / max(s1 - y_stand0, 1e-6))
        return lerp(profile["floor_z"], profile["z_lower"], t)
    if y <= s2:
        t = clamp01((y - s1) / max(s2 - s1, 1e-6))
        return lerp(profile["z_lower"], profile["z_middle"], t)

    t = clamp01((y - s2) / max(s3 - s2, 1e-6))
    return lerp(profile["z_middle"], profile["z_upper"], t)


def compute_surface_areas(room_dim):
    width, length, height = room_dim
    return {
        "floor": width * length,
        "ceiling": width * length,
        "east": length * height,
        "west": length * height,
        "north": width * height,
        "south": width * height
    }


def build_materials_from_rt60(room_dim, rt60_bands, band_freqs, surface_weights, scattering=0.1):
    if len(rt60_bands) != len(band_freqs):
        raise ValueError("RT60 bands and frequency bands must match in length.")

    surface_keys = ["ceiling", "floor", "east", "west", "north", "south"]
    areas = compute_surface_areas(room_dim)
    total_area = sum(areas.values())
    volume = room_dim[0] * room_dim[1] * room_dim[2]

    weighted_area = 0.0
    for key in surface_keys:
        weight = surface_weights.get(key, 1.0)
        weighted_area += areas[key] * weight

    coeffs = {key: [] for key in surface_keys}
    for rt60 in rt60_bands:
        rt60 = max(0.1, float(rt60))
        avg_abs = 0.161 * volume / (total_area * rt60)
        avg_abs = clamp(avg_abs, 0.02, 0.95)
        scale = avg_abs * total_area / max(weighted_area, 1e-9)

        for key in surface_keys:
            weight = surface_weights.get(key, 1.0)
            alpha = clamp(scale * weight, 0.02, 0.95)
            coeffs[key].append(alpha)

    materials = {}
    for key in surface_keys:
        materials[key] = pra.Material(
            {
                "description": f"{key} rt60-fit",
                "coeffs": coeffs[key],
                "center_freqs": band_freqs
            },
            scattering=scattering
        )

    return materials


def make_mic_loc(head_pos):
    return np.array(head_pos, dtype=float).reshape(3, 1)


def compute_rir_mono(room_dim, fs, materials, max_order_is, mic_loc, source_pos):
    room = pra.ShoeBox(
        room_dim,
        fs=fs,
        materials=materials,
        max_order=max_order_is,
        air_absorption=True,
        ray_tracing=False
    )
    room.add_microphone_array(mic_loc)
    room.add_source(source_pos)
    room.compute_rir()
    return room.rir[0][0]


def linspace_or_center(min_v, max_v, count):
    if count <= 1:
        return np.array([(min_v + max_v) / 2.0])
    return np.linspace(min_v, max_v, count)


def _sofa_try_get(obj, key):
    if "." in key:
        cur = obj
        for part in key.split("."):
            if hasattr(cur, part):
                cur = getattr(cur, part)
            elif isinstance(cur, dict) and part in cur:
                cur = cur[part]
            else:
                return None
        return cur
    if hasattr(obj, key):
        return getattr(obj, key)
    if isinstance(obj, dict) and key in obj:
        return obj[key]
    if hasattr(obj, "__getitem__"):
        try:
            return obj[key]
        except Exception:
            return None
    return None


def sofa_get(obj, key, aliases=None):
    keys = [key] + (aliases or [])
    for candidate in keys:
        value = _sofa_try_get(obj, candidate)
        if value is not None:
            return value
    raise KeyError(f"Missing SOFA field: {key}")


def _read_sofa(path):
    if hasattr(sofar, "read_sofa"):
        return sofar.read_sofa(path)
    if hasattr(sofar, "read"):
        return sofar.read(path)
    if hasattr(sofar, "Sofa"):
        return sofar.Sofa(path)
    raise RuntimeError("Unsupported sofar API; expected read_sofa/read/Sofa.")


def _to_unit_vectors_from_spherical(az_el):
    az = az_el[:, 0]
    el = az_el[:, 1]
    cos_el = np.cos(el)
    x = cos_el * np.cos(az)
    y = cos_el * np.sin(az)
    z = np.sin(el)
    vecs = np.stack([x, y, z], axis=1)
    norms = np.linalg.norm(vecs, axis=1, keepdims=True)
    return vecs / np.maximum(norms, 1e-12)


def _normalize_rows(mat):
    norms = np.linalg.norm(mat, axis=1, keepdims=True)
    return mat / np.maximum(norms, 1e-12)


def load_sofa_hrir(sofa_path):
    if not os.path.isfile(sofa_path):
        raise FileNotFoundError(f"SOFA not found: {sofa_path}")

    sofa = _read_sofa(sofa_path)

    ir = sofa_get(sofa, "Data.IR", ["Data_IR", "DataIR"])
    ir = np.array(ir)
    if ir.ndim != 3 or ir.shape[1] < 2:
        raise ValueError(f"Unexpected Data.IR shape: {ir.shape}")

    hrir_l = ir[:, 0, :]
    hrir_r = ir[:, 1, :]

    pos = sofa_get(sofa, "SourcePosition", ["Source_Position"])
    pos = np.array(pos)
    if pos.ndim != 2 or pos.shape[1] < 2:
        raise ValueError(f"Unexpected SourcePosition shape: {pos.shape}")

    try:
        pos_type = sofa_get(
            sofa,
            "SourcePosition_Type",
            ["SourcePositionType", "SourcePosition.Type"]
        )
        pos_type = str(pos_type).lower()
    except KeyError:
        pos_type = "spherical"

    try:
        pos_units = sofa_get(
            sofa,
            "SourcePosition_Units",
            ["SourcePositionUnits", "SourcePosition.Units"]
        )
        pos_units = str(pos_units).lower()
    except KeyError:
        pos_units = "degree"

    if "spherical" in pos_type:
        az_el = pos[:, :2].astype(float)
        if "rad" not in pos_units:
            az_el = np.deg2rad(az_el)
        src_dirs = _to_unit_vectors_from_spherical(az_el)
    elif "cart" in pos_type:
        src_dirs = _normalize_rows(pos[:, :3].astype(float))
    else:
        raise ValueError(f"Unsupported SourcePosition_Type: {pos_type}")

    fs = sofa_get(
        sofa,
        "Data.SamplingRate",
        ["Data_SamplingRate", "SamplingRate", "DataSamplingRate"]
    )
    fs = float(np.array(fs).reshape(-1)[0])

    return {
        "fs": fs,
        "src_dirs": src_dirs,
        "hrir_l": hrir_l,
        "hrir_r": hrir_r
    }


def speaker_dir_to_sofa_vec(head_pos, speaker_pos):
    vec = np.array(speaker_pos, dtype=float) - np.array(head_pos, dtype=float)
    # Room: x=right, y=front, z=up -> SOFA: x=front, y=left, z=up.
    sofa_vec = np.array([vec[1], -vec[0], vec[2]], dtype=float).reshape(1, 3)
    return _normalize_rows(sofa_vec)[0]


def select_hrir(sofa_data, dir_vec):
    dots = sofa_data["src_dirs"] @ dir_vec
    idx = int(np.argmax(dots))
    return sofa_data["hrir_l"][idx], sofa_data["hrir_r"][idx]


def build_cinema_profile():
    grid_rows = 5
    grid_cols = 5
    fs = 48000
    target_len_s = 2.2
    fade_out_ms = 170.0
    peak_target = 0.707

    room_dim = [30, 40, 18]
    max_order_is = 15
    materials = {
        "ceiling": pra.Material(0.40),
        "floor": pra.Material(0.85),
        "east": pra.Material(0.34),
        "west": pra.Material(0.38),
        "north": pra.Material(0.30),
        "south": pra.Material(0.50)
    }

    width = room_dim[0]
    length = room_dim[1]
    center_x = width / 2
    center_y = length / 2
    listener = [center_x, center_y, 1.2]

    margin = 2.0
    max_r = min(width, length) / 2 - margin
    radius = max(5.0, max_r)

    def pos_from_angle(deg, z, r=radius):
        rad = math.radians(deg)
        x = listener[0] + r * math.sin(rad)
        y = listener[1] + r * math.cos(rad)
        x = clamp(x, margin, width - margin)
        y = clamp(y, margin, length - margin)
        return [x, y, z]

    speakers = {
        "C": pos_from_angle(0, 5.0),
        "FL": pos_from_angle(-30, 5.0),
        "FR": pos_from_angle(30, 5.0),
        "SL": pos_from_angle(-90, 6.0),
        "SR": pos_from_angle(90, 6.0),
        "BL": pos_from_angle(-150, 6.0),
        "BR": pos_from_angle(150, 6.0),
        "TFL": pos_from_angle(-45, 12.0, r=radius * 0.6),
        "TFR": pos_from_angle(45, 12.0, r=radius * 0.6),
        "TRL": pos_from_angle(-135, 12.0, r=radius * 0.6),
        "TRR": pos_from_angle(135, 12.0, r=radius * 0.6),
        "LFE": pos_from_angle(0, 1.0, r=radius - 2.0)
    }

    seat_x_min = room_dim[0] * 0.25
    seat_x_max = room_dim[0] * 0.75
    seat_y_min = room_dim[1] * 0.25
    seat_y_max = room_dim[1] * 0.75

    return {
        "name": "cinema",
        "grid_rows": grid_rows,
        "grid_cols": grid_cols,
        "fs": fs,
        "target_len_s": target_len_s,
        "fade_out_ms": fade_out_ms,
        "peak_target": peak_target,
        "room_dim": room_dim,
        "materials": materials,
        "max_order_is": max_order_is,
        "listener": listener,
        "speakers": speakers,
        "mix_left": {"FL", "SL", "BL", "TFL", "TRL"},
        "mix_right": {"FR", "SR", "BR", "TFR", "TRR"},
        "mix_both": {"C", "LFE"},
        "mix_both_gain": 0.707,
        "seat_grid": {
            "x_min": seat_x_min,
            "x_max": seat_x_max,
            "y_min": seat_y_min,
            "y_max": seat_y_max
        },
        "low_shelf": {
            "freq_hz": 80.0,
            "gain_db": 2.5,
            "q": 0.707
        },
        "early_late": {
            "early_ms": 60.0,
            "late_gain": 0.45,
            "late_high_shelf_db": -5.0,
            "ap_delay_l": 11,
            "ap_delay_r": 17,
            "ap_feedback": 0.45,
            "late_side_k": 0.65,
            "xfeed_cutoff": 700.0,
            "xfeed_mix": 0.08
        }
    }


def build_live_budokan_profile():
    grid_rows = 5
    grid_cols = 5
    fs = 48000
    target_len_s = 2.0
    fade_out_ms = 180.0
    peak_target = 0.707

    room_dim = [58, 58, 22]
    max_order_is = 15

    band_freqs = [125, 250, 500, 1000, 2000, 4000]
    rt60_bands = [2.4, 2.2, 2.0, 1.8, 1.6, 1.4]
    surface_weights = {
        "floor": 1.3,
        "ceiling": 0.95,
        "east": 1.0,
        "west": 1.0,
        "north": 0.95,
        "south": 1.05
    }
    materials = build_materials_from_rt60(
        room_dim,
        rt60_bands,
        band_freqs,
        surface_weights,
        scattering=0.12
    )

    seat_x_min = 6.0
    seat_x_max = 52.0
    seat_y_min = 12.0
    seat_y_max = 52.0
    listener = [
        (seat_x_min + seat_x_max) / 2.0,
        (seat_y_min + seat_y_max) / 2.0,
        1.2
    ]

    speakers = {
        "L": [16.0, 7.0, 12.0],
        "R": [42.0, 7.0, 12.0],
        "Sub": [29.0, 6.0, 1.0]
    }

    return {
        "name": "live_budokan",
        "grid_rows": grid_rows,
        "grid_cols": grid_cols,
        "fs": fs,
        "target_len_s": target_len_s,
        "fade_out_ms": fade_out_ms,
        "peak_target": peak_target,
        "room_dim": room_dim,
        "materials": materials,
        "max_order_is": max_order_is,
        "listener": listener,
        "speakers": speakers,
        "mix_left": {"L"},
        "mix_right": {"R"},
        "mix_both": {"Sub"},
        "mix_both_gain": 0.707,
        "seat_grid": {
            "x_min": seat_x_min,
            "x_max": seat_x_max,
            "y_min": seat_y_min,
            "y_max": seat_y_max
        },
        "low_shelf": {
            "freq_hz": 80.0,
            "gain_db": 1.5,
            "q": 0.707
        },
        "early_late": {
            "early_ms": 60.0,
            "late_gain": 0.6,
            "late_high_shelf_db": -3.0,
            "ap_delay_l": 13,
            "ap_delay_r": 19,
            "ap_feedback": 0.5,
            "late_side_k": 0.6,
            "xfeed_cutoff": 700.0,
            "xfeed_mix": 0.06
        }
    }


def build_live_k_arena_profile():
    grid_rows = 5
    grid_cols = 5
    fs = 48000
    target_len_s = 2.0
    fade_out_ms = 150.0
    peak_target = 0.707

    include_center_fill = False
    speakers_local = {
        "MainL": [-14.0, -2.0, 12.0],
        "MainR": [14.0, -2.0, 12.0],
        "OutfillL": [-24.0, -2.0, 12.0],
        "OutfillR": [24.0, -2.0, 12.0]
    }

    if include_center_fill:
        speakers_local["Center"] = [0.0, -2.0, 11.0]

    delay1_xs = [-30.0, -18.0, -6.0, 6.0, 18.0, 30.0]
    for idx, x in enumerate(delay1_xs, start=1):
        speakers_local[f"Delay1_{idx:02d}"] = [x, 35.0, 18.0]

    delay2_xs = [-35.0, -25.0, -15.0, -5.0, 5.0, 15.0, 25.0, 35.0]
    for idx, x in enumerate(delay2_xs, start=1):
        speakers_local[f"Delay2_{idx:02d}"] = [x, 55.0, 22.0]

    arena_floor_area = 4950.0
    arena_aspect_ratio = 0.78
    arena_width = math.sqrt(arena_floor_area * arena_aspect_ratio)
    arena_depth = math.sqrt(arena_floor_area / arena_aspect_ratio)

    side_margin = 15.0
    back_margin = 25.0
    stage_margin = 10.0

    room_width = arena_width + 2.0 * side_margin
    room_length = arena_depth + back_margin + stage_margin
    room_dim = [room_width, room_length, 26.0]

    stage_front_center = [room_width / 2.0, stage_margin, 0.0]

    def to_room(pos):
        return [
            pos[0] + stage_front_center[0],
            pos[1] + stage_front_center[1],
            pos[2] + stage_front_center[2]
        ]

    speakers_room = {name: to_room(pos) for name, pos in speakers_local.items()}

    seat_y_min = 10.0
    seat_y_max = arena_depth + back_margin
    wall_clearance = 3.0
    seat_x_min = -(arena_width / 2.0 + side_margin - wall_clearance)
    seat_x_max = (arena_width / 2.0 + side_margin - wall_clearance)

    seat_x_min_room = seat_x_min + stage_front_center[0]
    seat_x_max_room = seat_x_max + stage_front_center[0]
    seat_y_min_room = seat_y_min + stage_front_center[1]
    seat_y_max_room = min(seat_y_max + stage_front_center[1], room_dim[1] - 1e-3)

    y_stand0_room = arena_depth + stage_front_center[1]
    y_max_room = seat_y_max_room
    seat_z_profile = {
        "y_stand0": y_stand0_room,
        "y_max": y_max_room,
        "lower_ratio": 0.40,
        "middle_ratio": 0.75,
        "floor_z": 1.2,
        "z_lower": 7.0,
        "z_middle": 14.0,
        "z_upper": 18.5
    }

    listener = [
        (seat_x_min_room + seat_x_max_room) / 2.0,
        (seat_y_min_room + seat_y_max_room) / 2.0,
        seat_z_from_profile(
            (seat_y_min_room + seat_y_max_room) / 2.0,
            seat_z_profile
        )
    ]

    band_freqs = [125, 250, 500, 1000, 2000, 4000]
    rt60_bands = [1.4, 1.2, 1.0, 0.9, 0.85, 0.8]
    surface_weights = {
        "floor": 1.3,
        "ceiling": 0.95,
        "east": 1.0,
        "west": 1.0,
        "north": 0.95,
        "south": 1.05
    }
    materials = build_materials_from_rt60(
        room_dim,
        rt60_bands,
        band_freqs,
        surface_weights,
        scattering=0.12
    )

    mix_left = {name for name, pos in speakers_local.items() if pos[0] < 0}
    mix_right = {name for name, pos in speakers_local.items() if pos[0] > 0}
    mix_both = {name for name, pos in speakers_local.items() if pos[0] == 0}

    return {
        "name": "live_k_arena",
        "grid_rows": grid_rows,
        "grid_cols": grid_cols,
        "fs": fs,
        "target_len_s": target_len_s,
        "fade_out_ms": fade_out_ms,
        "peak_target": peak_target,
        "room_dim": room_dim,
        "materials": materials,
        "max_order_is": 15,
        "listener": listener,
        "speakers": speakers_room,
        "mix_left": mix_left,
        "mix_right": mix_right,
        "mix_both": mix_both,
        "mix_both_gain": 0.707,
        "seat_grid": {
            "x_min": seat_x_min_room,
            "x_max": seat_x_max_room,
            "y_min": seat_y_min_room,
            "y_max": seat_y_max_room
        },
        "seat_z_profile": seat_z_profile,
        "low_shelf": {
            "freq_hz": 80.0,
            "gain_db": 1.0,
            "q": 0.707
        },
        "early_late": {
            "early_ms": 60.0,
            "late_gain": 0.5,
            "late_high_shelf_db": -4.0,
            "ap_delay_l": 13,
            "ap_delay_r": 19,
            "ap_feedback": 0.5,
            "late_side_k": 0.6,
            "xfeed_cutoff": 700.0,
            "xfeed_mix": 0.08
        }
    }


def get_profile(profile_name):
    if profile_name == "cinema":
        return build_cinema_profile()
    if profile_name == "live_budokan":
        return build_live_budokan_profile()
    if profile_name == "live_k_arena":
        return build_live_k_arena_profile()
    raise ValueError(f"Unknown profile: {profile_name}")


def resolve_output_dirs(output_root, profile_name, grid_rows, grid_cols):
    per_speaker = os.path.join(output_root, profile_name, "speakers")
    grid = os.path.join(output_root, profile_name, f"grid_{grid_rows}x{grid_cols}")
    return per_speaker, grid


def generate_brir(profile_name="cinema", output_root=None, sofa_path=None, generate_per_speaker=None, generate_grid=None):
    profile = get_profile(profile_name)

    grid_rows = profile["grid_rows"]
    grid_cols = profile["grid_cols"]
    output_root = output_root or DEFAULT_OUTPUT_ROOT
    output_dir, grid_dir = resolve_output_dirs(output_root, profile["name"], grid_rows, grid_cols)
    profile_per_speaker = profile.get("generate_per_speaker", True)
    profile_grid = profile.get("generate_grid", True)
    generate_per_speaker = profile_per_speaker if generate_per_speaker is None else generate_per_speaker
    generate_grid = profile_grid if generate_grid is None else generate_grid

    print(f"Profile: {profile['name']}")
    if generate_per_speaker:
        os.makedirs(output_dir, exist_ok=True)
        print(f"Generating per-speaker BRIRs in: {output_dir}")
    if generate_grid:
        os.makedirs(grid_dir, exist_ok=True)
        print(f"Generating seat grid BRIRs in: {grid_dir}")

    fs = profile["fs"]
    target_len = int(fs * profile["target_len_s"])
    fade_out_ms = profile["fade_out_ms"]
    peak_target = profile["peak_target"]

    room_dim = profile["room_dim"]
    max_order_is = profile["max_order_is"]
    materials = profile["materials"]

    sofa_path = sofa_path or os.environ.get("SOFA_PATH") or DEFAULT_SOFA_PATH
    if not sofa_path:
        raise ValueError("SOFA path is required. Use --sofa-path or set SOFA_PATH.")
    if not os.path.isfile(sofa_path):
        raise FileNotFoundError(f"SOFA not found: {sofa_path}")

    sofa_data = load_sofa_hrir(sofa_path)
    if int(sofa_data["fs"]) != fs:
        raise ValueError(f"SOFA sample rate mismatch: {sofa_data['fs']} != {fs}")

    speakers = profile["speakers"]
    low_shelf = profile["low_shelf"]
    early_late = profile["early_late"]

    mix_left = set(profile["mix_left"])
    mix_right = set(profile["mix_right"])
    mix_both = set(profile["mix_both"])
    mix_both_gain = profile.get("mix_both_gain", 0.707)
    seat_z_profile = profile.get("seat_z_profile")

    unknown = set(speakers.keys()) - mix_left - mix_right - mix_both
    if unknown:
        mix_both.update(unknown)
        print(f"Warning: routing unassigned speakers to both: {sorted(unknown)}")

    def synth_brir(head_pos, speaker_pos):
        mic_loc = make_mic_loc(head_pos)
        rir_mono = compute_rir_mono(room_dim, fs, materials, max_order_is, mic_loc, speaker_pos)
        dir_vec = speaker_dir_to_sofa_vec(head_pos, speaker_pos)
        hrir_l, hrir_r = select_hrir(sofa_data, dir_vec)

        brir_l = fftconvolve(rir_mono, hrir_l)
        brir_r = fftconvolve(rir_mono, hrir_r)
        brir_l, brir_r = process_early_late(brir_l, brir_r, fs, **early_late)
        brir_l = pad_or_truncate(brir_l, target_len)
        brir_r = pad_or_truncate(brir_r, target_len)
        return brir_l, brir_r

    if generate_per_speaker:
        head_pos = profile["listener"]
        for name, pos in speakers.items():
            print(f"Simulating {name} at {pos}...")
            brir_l, brir_r = synth_brir(head_pos, pos)

            stereo_brir = np.zeros((target_len, 2))
            stereo_brir[:, 0] = apply_low_shelf(brir_l, fs, **low_shelf)
            stereo_brir[:, 1] = apply_low_shelf(brir_r, fs, **low_shelf)

            quad_rir = np.zeros((target_len, 4))
            quad_rir[:, 0] = stereo_brir[:, 0]  # LL
            quad_rir[:, 1] = stereo_brir[:, 1]  # LR
            quad_rir[:, 2] = stereo_brir[:, 0]  # RL
            quad_rir[:, 3] = stereo_brir[:, 1]  # RR
            quad_rir = apply_fade_out(quad_rir, fs, fade_ms=fade_out_ms)

            peak = np.max(np.abs(quad_rir))
            if peak > 0:
                quad_rir = quad_rir / peak * peak_target

            filename = os.path.join(output_dir, f"{name}.wav")
            sf.write(filename, quad_rir, fs, subtype='FLOAT')
            print(f"Saved {filename}")

    if generate_grid:
        seat_grid = profile["seat_grid"]
        xs = linspace_or_center(seat_grid["x_min"], seat_grid["x_max"], grid_cols)
        ys = linspace_or_center(seat_grid["y_min"], seat_grid["y_max"], grid_rows)

        for r_idx, y in enumerate(ys):
            for c_idx, x in enumerate(xs):
                head_pos = [float(x), float(y), 1.2]
                if seat_z_profile is not None:
                    head_pos[2] = seat_z_from_profile(head_pos[1], seat_z_profile)

                ll = np.zeros(target_len)
                lr = np.zeros(target_len)
                rl = np.zeros(target_len)
                rr = np.zeros(target_len)

                print(f"Seat r{r_idx} c{c_idx}: {head_pos}")

                for name, pos in speakers.items():
                    brir_l, brir_r = synth_brir(head_pos, pos)

                    if name in mix_left:
                        ll += brir_l
                        lr += brir_r
                    elif name in mix_right:
                        rl += brir_l
                        rr += brir_r
                    else:
                        gain = mix_both_gain
                        ll += brir_l * gain
                        lr += brir_r * gain
                        rl += brir_l * gain
                        rr += brir_r * gain

                ll = apply_low_shelf(ll, fs, **low_shelf)
                lr = apply_low_shelf(lr, fs, **low_shelf)
                rl = apply_low_shelf(rl, fs, **low_shelf)
                rr = apply_low_shelf(rr, fs, **low_shelf)

                quad_rir = np.stack([ll, lr, rl, rr], axis=1)
                quad_rir = apply_fade_out(quad_rir, fs, fade_ms=fade_out_ms)
                peak = np.max(np.abs(quad_rir))
                if peak > 0:
                    quad_rir = quad_rir / peak * peak_target

                filename = os.path.join(grid_dir, f"Seat_r{r_idx}_c{c_idx}.wav")
                sf.write(filename, quad_rir, fs, subtype='FLOAT')


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", default="cinema", choices=["cinema", "live_budokan", "live_k_arena"])
    parser.add_argument("--output-root", default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--sofa-path", default=None)
    parser.add_argument("--no-speakers", action="store_true")
    parser.add_argument("--no-grid", action="store_true")
    args = parser.parse_args()

    try:
        generate_brir(
            profile_name=args.profile,
            output_root=args.output_root,
            sofa_path=args.sofa_path,
            generate_per_speaker=not args.no_speakers,
            generate_grid=not args.no_grid
        )
        print("Done.")
    except Exception as e:
        print(f"Error: {e}")




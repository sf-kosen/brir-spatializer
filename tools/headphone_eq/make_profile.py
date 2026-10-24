"""Build a headphone correction profile (parametric EQ) for the app.

The renderer produces binaural signals for headphones with a neutral response:
diffuse-field at the eardrum, which is what the HRTF equalisation assumes. This
script takes published frequency-response measurements of a headphone (from the
AutoEq project, several measurers, median), computes the correction towards
that target, keeps a moderate bass shelf so the result doesn't sound thin, and
fits a cascade of biquads. The bass is corrected all the way to the neutral
target; the app adds an adjustable low shelf on top (diffuse-field alone sounds
thin on in-ears to most listeners). The output is a C++ header for the app.

    python tools/headphone_eq/make_profile.py "Final Audio E3000" final_e3000 "final E3000"
"""

from __future__ import annotations

import argparse
import io
import math
import urllib.request
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares

AUTOEQ = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master"
MEASURERS = ["oratory1990", "Kazi", "Harpo", "RikudouGoku", "Fahryst"]
TARGET = "targets/Diffuse field ISO 11904-1.csv"
FIT_FS = 48000.0

MAX_TREBLE_BOOST_DB = 3.0  # above 10 kHz couplers disagree and insertion depth dominates
MAX_CUT_DB = -15.0
MAX_BOOST_DB = 6.0


def fetch_csv(path: str) -> tuple[np.ndarray, np.ndarray] | None:
    url = f"{AUTOEQ}/{urllib.request.quote(path)}"
    try:
        text = urllib.request.urlopen(url, timeout=30).read().decode("utf-8-sig")
    except Exception:
        return None
    data = np.genfromtxt(io.StringIO(text), delimiter=",", skip_header=1)
    if data.ndim != 2 or data.shape[0] < 100:
        return None
    return data[:, 0], data[:, 1]


def correction_curve(model: str) -> tuple[np.ndarray, np.ndarray, list[str]]:
    freqs, target = fetch_csv(TARGET)
    target = target - np.interp(1000.0, freqs, target)
    deviations, sources = [], []
    for measurer in MEASURERS:
        got = fetch_csv(f"measurements/{measurer}/data/in-ear/{model}.csv")
        if got is None:
            continue
        f, raw = got
        raw = np.interp(freqs, f, raw)
        deviations.append(raw - np.interp(1000.0, freqs, raw) - target)
        sources.append(measurer)
    if not deviations:
        raise SystemExit(f"No measurements found for {model}")
    deviation = np.median(np.array(deviations), axis=0)

    correction = -deviation
    treble = freqs > 10000.0
    correction[treble] = np.minimum(correction[treble], MAX_TREBLE_BOOST_DB)
    fade = np.clip((freqs - 16000.0) / 4000.0, 0.0, 1.0)  # no correction near 20 kHz
    correction = np.clip(correction * (1.0 - fade), MAX_CUT_DB, MAX_BOOST_DB)
    return freqs, correction, sources


# ----------------------------------------------------------------- biquad model


def biquad(kind: str, f0: float, gain_db: float, q: float, fs: float):
    """RBJ cookbook coefficients (b0, b1, b2, a1, a2), normalised by a0."""
    a = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f0 / fs
    cos, sin = math.cos(w0), math.sin(w0)
    alpha = sin / (2 * q)
    if kind == "peak":
        b = [1 + alpha * a, -2 * cos, 1 - alpha * a]
        den = [1 + alpha / a, -2 * cos, 1 - alpha / a]
    else:
        sq = 2 * math.sqrt(a) * alpha
        if kind == "lowshelf":
            b = [a * ((a + 1) - (a - 1) * cos + sq), 2 * a * ((a - 1) - (a + 1) * cos), a * ((a + 1) - (a - 1) * cos - sq)]
            den = [(a + 1) + (a - 1) * cos + sq, -2 * ((a - 1) + (a + 1) * cos), (a + 1) + (a - 1) * cos - sq]
        else:  # highshelf
            b = [a * ((a + 1) + (a - 1) * cos + sq), -2 * a * ((a - 1) + (a + 1) * cos), a * ((a + 1) + (a - 1) * cos - sq)]
            den = [(a + 1) - (a - 1) * cos + sq, 2 * ((a - 1) - (a + 1) * cos), (a + 1) - (a - 1) * cos - sq]
    return [v / den[0] for v in b] + [den[1] / den[0], den[2] / den[0]]


def response_db(filters, freqs: np.ndarray, fs: float) -> np.ndarray:
    z = np.exp(-2j * np.pi * freqs / fs)
    total = np.zeros(freqs.size)
    for kind, f0, gain, q in filters:
        b0, b1, b2, a1, a2 = biquad(kind, f0, gain, q, fs)
        h = (b0 + b1 * z + b2 * z * z) / (1 + a1 * z + a2 * z * z)
        total += 20 * np.log10(np.abs(h))
    return total


def fit(freqs: np.ndarray, correction: np.ndarray):
    grid = np.geomspace(20.0, 18000.0, 400)
    want = np.interp(grid, freqs, correction)
    kinds = ["lowshelf"] + ["peak"] * 9 + ["highshelf"]
    f0 = [105, 180, 320, 600, 1100, 2000, 3000, 4500, 6500, 9000, 12000]
    q0 = [0.7] + [1.2] * 9 + [0.7]
    x0 = []
    for f, q in zip(f0, q0):
        x0 += [math.log(f), 0.0, q]

    def unpack(x):
        return [(k, math.exp(x[3 * i]), x[3 * i + 1], x[3 * i + 2]) for i, k in enumerate(kinds)]

    # A small penalty on band gains keeps neighbouring bands from fighting each
    # other with large opposite boosts and cuts (same magnitude, worse phase).
    def residual(x):
        error = response_db(unpack(x), grid, FIT_FS) - want
        gains = np.array(x[1::3])
        return np.concatenate([error, 0.15 * gains])

    lower, upper = [], []
    for f in f0:
        lower += [math.log(f / 1.6), -18.0, 0.4]
        upper += [math.log(f * 1.6), 12.0, 4.0]
    result = least_squares(residual, x0, bounds=(lower, upper))
    filters = unpack(result.x)
    error = response_db(filters, grid, FIT_FS) - want
    return filters, float(np.sqrt(np.mean(error ** 2))), float(np.max(np.abs(error)))


def write_header(path: Path, profiles: list[dict]) -> None:
    lines = [
        "#pragma once",
        "",
        "// Generated by tools/headphone_eq/make_profile.py -- do not edit.",
        "// Corrections towards a diffuse-field response (the app adds a bass shelf),",
        "// from the median of published measurements (AutoEq project).",
        "",
        "#include \"brir/headphone_eq.h\"",
        "",
        "#include <array>",
        "",
        "namespace headphone_profiles {",
        "",
    ]
    for p in profiles:
        filters = ",\n".join(
            f"    brir::EqBand{{brir::EqBand::{'LowShelf' if k == 'lowshelf' else 'HighShelf' if k == 'highshelf' else 'Peak'}, "
            f"{f:.1f}f, {g:.2f}f, {q:.3f}f}}"
            for k, f, g, q in p["filters"]
        )
        lines += [
            f"// {p['label']}: sources {', '.join(p['sources'])}; fit RMS {p['rms']:.2f} dB, max {p['max']:.2f} dB.",
            f"inline const std::array<brir::EqBand, {len(p['filters'])}> {p['id']} = {{{{",
            filters,
            "}};",
            f"inline constexpr float {p['id']}_preamp_db = {p['preamp']:.2f}f;",
            "",
        ]
    lines += ["} // namespace headphone_profiles", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("model", help='AutoEq measurement name, e.g. "Final Audio E3000"')
    parser.add_argument("id", help="C++ identifier for the profile")
    parser.add_argument("label", help="display name")
    parser.add_argument("--out", type=Path, default=Path("app/src/HeadphoneProfiles.h"))
    args = parser.parse_args()

    freqs, correction, sources = correction_curve(args.model)
    filters, rms, worst = fit(freqs, correction)
    fitted = response_db(filters, np.geomspace(20.0, 20000.0, 2000), FIT_FS)
    preamp = -max(0.0, float(np.max(fitted)))  # informational; the app recomputes it with its bass shelf
    print(f"{args.label}: {len(sources)} sources ({', '.join(sources)}), fit RMS {rms:.2f} dB, max {worst:.2f} dB, preamp {preamp:.1f} dB")
    for point in [30, 60, 100, 200, 500, 1000, 2000, 3000, 4000, 6000, 8000, 10000, 12000, 15000]:
        print(f"  {point:>6} Hz: correction {np.interp(point, freqs, correction):+6.1f} dB")
    write_header(args.out, [dict(id=args.id, label=args.label, sources=sources, filters=filters, rms=rms, max=worst,
                                 preamp=preamp)])
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()

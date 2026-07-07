# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""pack_jvox: モーラ単位 WAV 群から .jvox (単位連結 TTS 用音声 DB) を作る。

Usage:
    python3 pack_jvox.py <unit_dir> <out.jvox>

<unit_dir> には index.tsv (key_hex \t kana \t filename [\t f0_hint]) と
16 kHz mono i16 の WAV 群を置く。各単位についてピッチマーク (声門閉鎖近傍の
負ピーク列) を抽出してパックする。フォーマットは
components/jtts/include/jtts/jvox.hpp のコメントが正。
"""

import struct
import sys
import wave
from pathlib import Path

import numpy as np


def load_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2, path
        fs = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return x.astype(np.float64), fs


def estimate_f0(x: np.ndarray, fs: int, lo=70.0, hi=400.0) -> float:
    """発話中央部の自己相関で平均 F0 を推定する。無声なら 0。"""
    n = len(x)
    seg = x[n // 4 : n * 3 // 4].copy()
    if len(seg) < int(fs / lo) * 2:
        seg = x
    seg = seg - seg.mean()
    ac = np.correlate(seg, seg, "full")[len(seg) - 1 :]
    if ac[0] <= 0:
        return 0.0
    lag_lo, lag_hi = int(fs / hi), int(fs / lo)
    if lag_hi >= len(ac):
        lag_hi = len(ac) - 1
    lag = lag_lo + int(np.argmax(ac[lag_lo:lag_hi]))
    if ac[lag] < 0.25 * ac[0]:
        return 0.0
    return fs / lag


def pitch_marks(x: np.ndarray, fs: int, f0: float) -> list[int]:
    """周期ごとの負ピーク (声門閉鎖近傍) を前方走査でスナップして返す。

    有声区間の判定は「周期窓内のエネルギーがピーク値の 5% 以上」で緩く取り、
    先頭の子音部 (無声) はマークなしのまま残す。
    """
    if f0 <= 0:
        return []
    period = int(round(fs / f0))
    if period < 8:
        return []
    env = np.abs(x)
    # 移動 RMS でエネルギー包絡を出し、有声しきい値を決める。
    k = period
    kernel = np.ones(k) / k
    rms = np.sqrt(np.convolve(x * x, kernel, "same") + 1e-9)
    thresh = rms.max() * 0.10

    marks: list[int] = []
    # 最初の有声サンプルから開始し、以降 1 周期ずつ窓の最小値へスナップ。
    voiced = np.nonzero(rms > thresh)[0]
    if len(voiced) == 0:
        return []
    pos = int(voiced[0])
    # 最初のマーク: pos から 1.5 周期以内の最小値。
    w_end = min(len(x), pos + period + period // 2)
    if w_end - pos < period // 2:
        return []
    pos = pos + int(np.argmin(x[pos:w_end]))
    marks.append(pos)
    while True:
        center = marks[-1] + period
        lo = center - period // 3
        hi = center + period // 3
        if hi >= len(x):
            break
        if rms[min(center, len(rms) - 1)] <= thresh:
            break  # 有声区間の終わり
        m = lo + int(np.argmin(x[lo:hi]))
        if m <= marks[-1]:
            break
        marks.append(m)
    # u16 に収まることを検査 (unit ≤ ~65k サンプル)。
    if marks and marks[-1] > 0xFFFF:
        raise ValueError("unit too long for u16 pitch marks")
    return marks


def steady_start(marks: list[int]) -> int:
    """母音定常部の開始 (将来のクロスフェード用)。マーク列の 40% 地点。"""
    if not marks:
        return 0
    return marks[max(0, int(len(marks) * 0.4))]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 1
    unit_dir = Path(sys.argv[1])
    out_path = Path(sys.argv[2])

    entries = []
    for line in (unit_dir / "index.tsv").read_text().splitlines():
        parts = line.strip().split("\t")
        if len(parts) < 3:
            continue
        entries.append((int(parts[0], 16), parts[1], parts[2]))

    sample_rate = None
    units = []  # (key, pcm int16 array, marks, steady, f0)
    for key, kana, fname in entries:
        x, fs = load_wav(unit_dir / fname)
        if sample_rate is None:
            sample_rate = fs
        assert fs == sample_rate, f"sample-rate mismatch: {fname}"
        f0 = estimate_f0(x, fs)
        marks = pitch_marks(x, fs, f0)
        if len(marks) < 3:
            print(f"  [warn] {kana} ({fname}): voiced marks={len(marks)} f0={f0:.0f} — "
                  "無声単位として格納 (verbatim 再生)")
            marks = []
        units.append((key, x.astype(np.int16), marks, steady_start(marks), f0))
        print(f"  {kana:4s} key={key:04x} len={len(x):6d} f0={f0:6.1f} marks={len(marks)}")

    # パック
    marks_all: list[int] = []
    pcm_all: list[np.ndarray] = []
    recs = []
    pcm_off = 0
    for key, pcm, marks, steady, f0 in units:
        recs.append(struct.pack("<HHIIHH", key, len(marks), pcm_off, len(pcm),
                                steady, int(round(f0 * 10))))
        marks_all.extend(marks)
        pcm_all.append(pcm)
        pcm_off += len(pcm)

    blob = b"JVOX" + struct.pack("<BBHHH", 1, 0, sample_rate, len(units), 0)
    blob += b"".join(recs)
    blob += struct.pack("<I", len(marks_all))
    blob += np.asarray(marks_all, dtype="<u2").tobytes()
    blob += np.concatenate(pcm_all).astype("<i2").tobytes()

    out_path.write_bytes(blob)
    print(f"wrote {out_path} ({len(blob)} bytes, {len(units)} units, "
          f"{sum(len(p) for p in pcm_all)} samples)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

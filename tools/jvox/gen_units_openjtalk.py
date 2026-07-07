# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""gen_units_openjtalk: pyopenjtalk (Open JTalk) でモーラ単位 WAV を生成する。

Usage:
    <venv>/bin/python gen_units_openjtalk.py <ref_index.tsv> <outdir>

<ref_index.tsv> は jtts_gen_units が書く index.tsv (key_hex \t kana \t ...) —
キー採番の正本として流用する。各モーラは「<かな>ー」(長音付き) で合成する:
  - 語末短母音の無声化 (「す」→ s だけ) を避ける
  - 有声部が長くなり TD-PSOLA の伸縮材料が増える
出力: <outdir>/mora_<key>.wav (16 kHz mono i16) + index.tsv + ATTRIBUTION.md

音声モデル: pyopenjtalk 同梱の HTS Voice "Mei" (MMDAgent Project Team /
名古屋工業大学、CC BY 3.0)。生成した単位 DB は派生著作物として同ライセンスの
帰属表示が必要 — ATTRIBUTION.md を DB と一緒に配布すること。
"""

import struct
import sys
import wave
from pathlib import Path

import numpy as np
import pyopenjtalk


def mel_frames(x: np.ndarray, fs: int) -> np.ndarray:
    """粗い log-mel 風スペクトル列 (25 ms 窓 / 10 ms ホップ / 20 帯域)。

    キャリア発話からの単位切り出しのテンプレート マッチング用。同一合成声
    どうしの比較なので厳密な MFCC は不要。
    """
    fl, hop = int(fs * 0.025), int(fs * 0.010)
    n_bands = 20
    edges = np.geomspace(100.0, 7000.0, n_bands + 1)
    frames = []
    for i in range(0, len(x) - fl, hop):
        seg = x[i : i + fl] * np.hanning(fl)
        p = np.abs(np.fft.rfft(seg)) ** 2
        fr = np.fft.rfftfreq(fl, 1 / fs)
        bands = [np.log10(p[(fr >= edges[b]) & (fr < edges[b + 1])].sum() + 1e3)
                 for b in range(n_bands)]
        frames.append(bands)
    return np.asarray(frames)


def cut_from_carrier(carrier: np.ndarray, template: np.ndarray, fs: int) -> np.ndarray:
    """キャリア「あー<モーラ>ー」から <モーラ>ー 部分を切り出す。

    単発合成した同モーラをテンプレートに、キャリアの 25%〜85% の範囲で
    スペクトル距離が最小になる開始フレームを探す。8 ms 手前から切ることで
    先行母音 /a/ からの遷移 (コアーティキュレーション) を少しだけ残す。
    """
    car = mel_frames(carrier, fs)
    tpl = mel_frames(template, fs)
    k = min(len(tpl), 15)  # テンプレート先頭 ~150 ms
    if k < 3 or len(car) < k + 4:
        return template  # 短すぎ → 単発版へフォールバック
    lo = max(1, int(len(car) * 0.25))
    hi = max(lo + 1, min(len(car) - k, int(len(car) * 0.85)))
    costs = [np.mean((car[o : o + k] - tpl[:k]) ** 2) for o in range(lo, hi)]
    o_best = lo + int(np.argmin(costs))
    hop = int(fs * 0.010)
    start = max(0, o_best * hop)
    return carrier[start:]


def resample_48k_to_16k(x: np.ndarray) -> np.ndarray:
    """48 kHz → 16 kHz。1/3 間引きの前に簡易 FIR LPF (fc≈7.2 kHz) を掛ける。"""
    # windowed-sinc LPF (63 taps, cutoff 0.15 = 7.2 kHz @48k)
    n = 63
    t = np.arange(n) - (n - 1) / 2
    fc = 0.15
    h = 2 * fc * np.sinc(2 * fc * t) * np.hamming(n)
    h /= h.sum()
    y = np.convolve(x, h, "same")
    return y[::3]


def trim_silence(x: np.ndarray, fs: int, thresh_ratio=0.02, pad_ms=15) -> np.ndarray:
    env = np.abs(x)
    k = max(1, fs // 200)
    env = np.convolve(env, np.ones(k) / k, "same")
    thresh = env.max() * thresh_ratio
    idx = np.nonzero(env > thresh)[0]
    if len(idx) == 0:
        return x
    pad = int(fs * pad_ms / 1000)
    lo = max(0, int(idx[0]) - pad)
    hi = min(len(x), int(idx[-1]) + pad)
    return x[lo:hi]


ATTRIBUTION = """# Voice-unit DB attribution

This mora-unit voice database was generated with Open JTalk
(Modified BSD) using:

    HTS Voice "Mei" (mei_normal)
    Copyright (c) 2009-2013 Nagoya Institute of Technology,
    Department of Computer Science (MMDAgent Project Team)
    Licensed under the Creative Commons Attribution 3.0 license
    https://creativecommons.org/licenses/by/3.0/

The derived unit waveforms in this directory / the packed .jvox file
are redistributable under the same attribution requirement.
"""


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 1
    ref_index = Path(sys.argv[1])
    outdir = Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)

    entries = []
    for line in ref_index.read_text().splitlines():
        parts = line.strip().split("\t")
        if len(parts) >= 3:
            entries.append((parts[0], parts[1]))

    index_lines = []
    for key_hex, kana in entries:
        # half_tone=-6: Mei は単発モーラを ~370 Hz の高ピッチで読むため、
        # -6 半音 (~260 Hz) に下げて目標 F0 (200-230 Hz) との PSOLA シフト量を
        # 小さくする (シフトが大きいほどグレインの重なりが薄れ品質が落ちる)。
        #
        # 単発版 (テンプレート) とキャリア版「あー<モーラ>ー」の両方を合成し、
        # キャリアから切り出す: 単発だと子音が発話頭のコールド スタートに
        # なるが、キャリア内の子音は先行母音からの自然な遷移
        # (コアーティキュレーション) を持つ — 連結時の「途切れ」感が減る。
        t48, sr = pyopenjtalk.tts(kana + "ー", half_tone=-6.0)
        assert sr == 48000, sr
        tpl = trim_silence(resample_48k_to_16k(np.asarray(t48, dtype=np.float64)), 16000)
        c48, _ = pyopenjtalk.tts("あー" + kana + "ー", half_tone=-6.0)
        car = trim_silence(resample_48k_to_16k(np.asarray(c48, dtype=np.float64)), 16000)
        x16 = cut_from_carrier(car, tpl, 16000)
        peak = np.abs(x16).max()
        if peak > 0:
            x16 = x16 * (0.7 * 32767 / peak)
        fname = f"mora_{key_hex}.wav"
        with wave.open(str(outdir / fname), "w") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(x16.astype("<i2").tobytes())
        index_lines.append(f"{key_hex}\t{kana}\t{fname}")
        print(f"  {kana:4s} {fname} {len(x16)} samples")

    (outdir / "index.tsv").write_text("\n".join(index_lines) + "\n")
    (outdir / "ATTRIBUTION.md").write_text(ATTRIBUTION)
    print(f"wrote {len(index_lines)} units to {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compare one isolated authentic/extended native-audio effect in PCM dumps.

Run both captures with Music volume 0, SFX volume 100, native audio tracing,
and `AR_NATIVE_AUDIO_PCM=1`.  The script uses the request trace to locate one
completed effect, trims silence, aligns the two native-rate waveforms by a
small integer lag, and reports the residual error.  Natural replays can begin
on different DSP/interpolation phases, so the default acceptance threshold is
5% RMS rather than byte identity; the DSP unit test covers identical-state
byte parity separately.
"""

from __future__ import annotations

import argparse
import array
import csv
import math
from pathlib import Path
import wave


def parse_hex(value: str) -> int:
    return int(value.removeprefix("$").removeprefix("0x"), 16)


def find_request(run: Path, frame: int, site: int, effect_id: int) -> dict[str, str]:
    path = run / "native_audio_requests.csv"
    with path.open(newline="") as source:
        matches = [
            row for row in csv.DictReader(source)
            if int(row["frame"]) == frame
            and int(row["site"], 16) == site
            and int(row["id"], 16) == effect_id
            and row["outcome"] == "completed"
        ]
    if len(matches) != 1:
        raise ValueError(f"{path}: expected one completed match, found {len(matches)}")
    return matches[0]


def read_segment(run: Path, request: dict[str, str]) -> array.array:
    path = run / "native_audio_pcm.wav"
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 2 or source.getsampwidth() != 2:
            raise ValueError(f"{path}: expected stereo signed-16 PCM")
        samples = array.array("h")
        samples.frombytes(source.readframes(source.getnframes()))
    start = max(0, int(request["start_cycle"]) // 32 - 512)
    end = min(len(samples) // 2, int(request["end_cycle"]) // 32 + 4096)
    nonzero = [
        index for index in range(start, end)
        if samples[index * 2] or samples[index * 2 + 1]
    ]
    if not nonzero:
        raise ValueError(f"{path}: selected request window contains no PCM")
    first, last = nonzero[0], nonzero[-1] + 1
    return samples[first * 2:last * 2]


def aligned_metrics(
    authentic: array.array, extended: array.array, maximum_lag: int,
) -> tuple[int, int, int, float, float, int]:
    best: tuple[int, int, int, float, float, int] | None = None
    for lag in range(-maximum_lag, maximum_lag + 1):
        authentic_start = max(0, lag) * 2
        extended_start = max(0, -lag) * 2
        count = min(
            len(authentic) - authentic_start,
            len(extended) - extended_start,
        )
        if count <= 0:
            continue
        error_sq = 0
        signal_sq = 0
        maximum_error = 0
        for index in range(count):
            difference = (
                authentic[authentic_start + index]
                - extended[extended_start + index]
            )
            error_sq += difference * difference
            sample = extended[extended_start + index]
            signal_sq += sample * sample
            maximum_error = max(maximum_error, abs(difference))
        error_rms = math.sqrt(error_sq / count)
        signal_rms = math.sqrt(signal_sq / count)
        candidate = (
            lag, authentic_start, extended_start,
            error_rms, signal_rms, maximum_error,
        )
        if best is None or error_rms < best[3]:
            best = candidate
    assert best is not None
    return best


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("authentic_run", type=Path)
    parser.add_argument("extended_run", type=Path)
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--site", type=parse_hex, required=True)
    parser.add_argument("--id", type=parse_hex, default=0x07)
    parser.add_argument("--maximum-lag", type=int, default=64)
    parser.add_argument("--maximum-relative-rms", type=float, default=0.05)
    args = parser.parse_args()

    authentic_request = find_request(
        args.authentic_run, args.frame, args.site, args.id
    )
    extended_request = find_request(
        args.extended_run, args.frame, args.site, args.id
    )
    authentic = read_segment(args.authentic_run, authentic_request)
    extended = read_segment(args.extended_run, extended_request)
    lag, _, _, error_rms, signal_rms, maximum_error = aligned_metrics(
        authentic, extended, args.maximum_lag
    )
    relative = error_rms / signal_rms if signal_rms else math.inf
    print(
        f"authentic_frames={len(authentic) // 2} "
        f"extended_frames={len(extended) // 2} lag={lag}"
    )
    print(
        f"error_rms={error_rms:.3f} signal_rms={signal_rms:.3f} "
        f"relative_rms={relative:.6f} max_error={maximum_error}"
    )
    if relative > args.maximum_relative_rms:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

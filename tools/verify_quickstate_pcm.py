#!/usr/bin/env python3
"""Verify sample-exact native PCM across an in-process quick-state rewind.

New captures log the save/load PCM boundaries while holding the APU lock, so
the script compares the two continuations directly. Older captures can still
use a duplicated traced request to estimate the offset. The result reports a
non-silent, byte-identical stereo-frame prefix (or legacy aligned run).
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import wave


def parse_hex(value: str) -> int:
    return int(value.removeprefix("$").removeprefix("0x"), 16)


def find_duplicate_requests(
    run: Path, frame: int, site: int, effect_id: int,
) -> tuple[dict[str, str], dict[str, str]]:
    path = run / "native_audio_requests.csv"
    with path.open(newline="") as source:
        matches = [
            row for row in csv.DictReader(source)
            if int(row["frame"]) == frame
            and int(row["site"], 16) == site
            and int(row["id"], 16) == effect_id
        ]
    if len(matches) != 2:
        raise ValueError(
            f"{path}: expected two matching pre/post-load requests, "
            f"found {len(matches)}"
        )
    matches.sort(key=lambda row: int(row["posted_cycle"]))
    return matches[0], matches[1]


def read_pcm_frames(run: Path) -> list[bytes]:
    path = run / "native_audio_pcm.wav"
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 2 or source.getsampwidth() != 2:
            raise ValueError(f"{path}: expected stereo signed-16 PCM")
        if source.getframerate() != 32000:
            raise ValueError(f"{path}: expected 32000 Hz native trace PCM")
        data = source.readframes(source.getnframes())
    return [data[index:index + 4] for index in range(0, len(data), 4)]


def find_audio_boundaries(run: Path) -> tuple[int, int] | None:
    path = run / "console.log"
    pattern = re.compile(
        r"\[quickstate-audio\] (save|load) "
        r"trace-frame=(\d+) apu-sample-clock=(\d+)"
    )
    matches = pattern.findall(path.read_text(errors="replace"))
    if not matches:
        return None
    if len(matches) != 2 or [match[0] for match in matches] != ["save", "load"]:
        raise ValueError(f"{path}: expected one ordered save/load audio marker")
    if matches[0][2] != matches[1][2]:
        raise ValueError(
            f"{path}: restored APU sample clocks differ: "
            f"{matches[0][2]} != {matches[1][2]}"
        )
    return int(matches[0][1]), int(matches[1][1])


def exact_prefix(
    frames: list[bytes], first: int, second: int,
) -> tuple[int, int]:
    zero = b"\0\0\0\0"
    length = 0
    signal_frames = 0
    while (first + length < len(frames)
           and second + length < len(frames)
           and frames[first + length] == frames[second + length]):
        if frames[first + length] != zero:
            signal_frames += 1
        length += 1
    return length, signal_frames


def longest_exact_signal_run(
    frames: list[bytes], first_estimate: int, offset: int,
    search_before: int, search_after: int,
) -> tuple[int, int, int]:
    first = max(0, first_estimate - search_before)
    last = min(len(frames) - offset, first_estimate + offset + search_after)
    zero = b"\0\0\0\0"
    best_start = 0
    best_length = 0
    best_signal_frames = 0
    run_start = first
    run_signal_frames = 0

    for index in range(first, last + 1):
        equal = index < last and frames[index] == frames[index + offset]
        if equal:
            if frames[index] != zero:
                run_signal_frames += 1
            continue
        length = index - run_start
        if run_signal_frames and length > best_length:
            best_start = run_start
            best_length = length
            best_signal_frames = run_signal_frames
        run_start = index + 1
        run_signal_frames = 0
    return best_start, best_length, best_signal_frames


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path)
    parser.add_argument("--frame", type=int)
    parser.add_argument("--site", type=parse_hex)
    parser.add_argument("--id", type=parse_hex)
    parser.add_argument("--search-before", type=int, default=512)
    parser.add_argument("--search-after", type=int, default=4096)
    parser.add_argument("--minimum-exact-frames", type=int, default=512)
    args = parser.parse_args()

    boundaries = find_audio_boundaries(args.run)
    frames = read_pcm_frames(args.run)
    if boundaries is not None:
        first, second = boundaries
        if second <= first:
            raise ValueError("load PCM boundary must follow save boundary")
        length, signal_frames = exact_prefix(frames, first, second)
        print(
            f"audio_boundaries={first},{second} "
            f"pcm_frame_offset={second - first}"
        )
        print(
            f"exact_start_frames={first},{second} exact_frames={length} "
            f"signal_frames={signal_frames} seconds={length / 32000:.6f}"
        )
        if length < args.minimum_exact_frames or signal_frames == 0:
            raise SystemExit(1)
        return

    if args.frame is None or args.site is None or args.id is None:
        raise ValueError(
            "run has no quick-state audio markers; --frame, --site, and --id "
            "are required for legacy request-based alignment"
        )
    before, after = find_duplicate_requests(
        args.run, args.frame, args.site, args.id
    )
    first_cycle = int(before["posted_cycle"])
    second_cycle = int(after["posted_cycle"])
    cycle_delta = second_cycle - first_cycle
    if cycle_delta <= 0 or cycle_delta % 32:
        raise ValueError(
            f"request cycle delta {cycle_delta} is not a positive whole "
            "DSP-frame interval"
        )
    frame_offset = cycle_delta // 32
    start, length, signal_frames = longest_exact_signal_run(
        frames,
        first_cycle // 32,
        frame_offset,
        args.search_before,
        args.search_after,
    )
    print(
        f"request_cycles={first_cycle},{second_cycle} "
        f"pcm_frame_offset={frame_offset}"
    )
    print(
        f"exact_start_frames={start},{start + frame_offset} "
        f"exact_frames={length} signal_frames={signal_frames} "
        f"seconds={length / 32000:.6f}"
    )
    if length < args.minimum_exact_frames:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

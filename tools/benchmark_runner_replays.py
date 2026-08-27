#!/usr/bin/env python3
"""Repeatable headless replay benchmarks for runner/ABI changes.

The suite runs outside the repository with run-directory diagnostics disabled,
pins every gameplay-affecting input, and hashes the final machine state.  This
keeps timing data useful without allowing a faster-but-wrong candidate through.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = (
    "dump_wram.bin",
    "dump_sram.bin",
    "dump_state.txt",
    "dump_dispatch_log.json",
)


@dataclass(frozen=True)
class Workload:
    description: str
    replay: str
    frames: int
    settings: str | None = None
    environment: dict[str, str] = field(default_factory=dict)


WIDE_DISPLAY_ENV = {
    "AR_WS_HEADLESS": "1",
    "AR_EXTENDED_ASPECT_RATIO": "16:10",
    "AR_ASPECT_PAR": "Square pixels",
    "AR_DISPLAY_MODE": "2",
}


WIDE_ACTION_ENV = {
    **WIDE_DISPLAY_ENV,
    "AR_ACTION_BG_HLE": "1",
    "AR_DIORAMA": "0",
    "AR_SIM3D": "0",
    "AR_MOONJUMP": "1",
    "AR_NO_KNOCKBACK": "1",
    "AR_RANGED_SWORD": "1",
}


WORKLOADS = {
    "mode7_worldmap": Workload(
        description="Repeated authentic-width Mode 7 and world-map transitions",
        replay="saves/fillmore-r1-natural.rec",
        frames=6000,
    ),
    "sky_palace_wide": Workload(
        description="Wide Sky Palace margin patch and restore transaction",
        replay="saves/fillmore-r1-natural.rec",
        frames=1200,
        environment=WIDE_DISPLAY_ENV,
    ),
    "sim_actions": Workload(
        description="Simulation-mode actions with the replay's pinned settings",
        replay="saves/sim-actions.rec",
        frames=6000,
        settings="tests/fixtures/sim3d/sim-actions-settings.ini",
    ),
    "aitos_wide": Workload(
        description="Wide action-mode traversal with background HLE enabled",
        replay="saves/aitos-r4-natural.rec",
        frames=4000,
        environment=WIDE_ACTION_ENV,
    ),
    "death_heim_wide": Workload(
        description="Wide late-game action and effects workload",
        replay="saves/death-heim-r8-r10-natural.rec",
        frames=4000,
        environment=WIDE_ACTION_ENV,
    ),
}


# Preserve the established four-workload ABI gate and its stored baselines.
# Targeted seams can opt into additional workloads with --workload.
DEFAULT_WORKLOADS = (
    "mode7_worldmap",
    "sim_actions",
    "aitos_wide",
    "death_heim_wide",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(*args: str) -> str:
    result = subprocess.run(
        ("git", *args), cwd=ROOT, text=True, capture_output=True, check=False
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def relative(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def clean_environment() -> dict[str, str]:
    prefixes = ("AR_", "SNESRECOMP_", "SNESREF_")
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(prefixes)
    }
    environment.update(
        {
            "LC_ALL": "C",
            "SDL_AUDIODRIVER": "dummy",
            "SDL_VIDEODRIVER": "dummy",
            "AR_NO_RUN_DIR": "1",
            "AR_HEADLESS": "1",
            "AR_SAVE_EDIT": "0",
            "AR_MUSIC_REPLACEMENTS": "0",
            "AR_HD_REPLACEMENTS": "0",
            "AR_DIORAMA": "0",
            "AR_SIM3D": "0",
        }
    )
    return environment


def parse_final_frame(state_path: Path) -> int:
    match = re.search(r"^frame=(\d+)\b", state_path.read_text(), re.MULTILINE)
    if not match:
        raise RuntimeError(f"missing final frame in {state_path}")
    return int(match.group(1))


def run_once(
    binary: Path,
    rom: Path,
    config: Path,
    save: Path,
    workload: Workload,
) -> tuple[float, dict[str, str], str]:
    replay = ROOT / workload.replay
    environment = clean_environment()
    environment.update(workload.environment)
    environment.update(
        {
            "AR_INPUT_REPLAY": str(replay),
            "AR_QUIT_FRAMES": str(workload.frames),
            "AR_SAVE_NATIVE_PATH": str(save),
        }
    )
    if workload.settings:
        environment["AR_SETTINGS_PATH"] = str(ROOT / workload.settings)

    with tempfile.TemporaryDirectory(prefix="actraiser-runner-bench-") as temp:
        workdir = Path(temp)
        (workdir / "saves").mkdir()
        command = (str(binary), str(rom), "--config", str(config))
        started = time.perf_counter_ns()
        result = subprocess.run(
            command,
            cwd=workdir,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        elapsed = (time.perf_counter_ns() - started) / 1_000_000_000.0
        if result.returncode != 0:
            raise RuntimeError(
                f"runner exited {result.returncode}\n--- runner output ---\n"
                f"{result.stdout[-12000:]}"
            )

        artifact_paths = {name: workdir / "saves" / name for name in ARTIFACTS}
        missing = [name for name, path in artifact_paths.items() if not path.is_file()]
        if missing:
            raise RuntimeError(
                f"runner did not emit {', '.join(missing)}\n--- runner output ---\n"
                f"{result.stdout[-12000:]}"
            )
        final_frame = parse_final_frame(artifact_paths["dump_state.txt"])
        if final_frame != workload.frames:
            raise RuntimeError(
                f"expected final frame {workload.frames}, observed {final_frame}"
            )
        hashes = {name: sha256(path) for name, path in artifact_paths.items()}
        return elapsed, hashes, result.stdout


def file_identity(path: Path) -> dict[str, Any]:
    return {
        "path": relative(path),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def workload_identity(workload: Workload) -> dict[str, Any]:
    replay = ROOT / workload.replay
    identity: dict[str, Any] = {
        "description": workload.description,
        "frames": workload.frames,
        "replay": file_identity(replay),
        "settings": file_identity(ROOT / workload.settings)
        if workload.settings
        else None,
        "environment": dict(sorted(workload.environment.items())),
    }
    return identity


def summarize(durations: list[float], frames: int) -> dict[str, Any]:
    median = statistics.median(durations)
    absolute_deviations = [abs(value - median) for value in durations]
    return {
        "durations_seconds": [round(value, 6) for value in durations],
        "median_seconds": round(median, 6),
        "mean_seconds": round(statistics.mean(durations), 6),
        "stdev_seconds": round(statistics.stdev(durations), 6)
        if len(durations) > 1
        else 0.0,
        "median_absolute_deviation_seconds": round(
            statistics.median(absolute_deviations), 6
        ),
        "min_seconds": round(min(durations), 6),
        "max_seconds": round(max(durations), 6),
        "median_emulated_fps": round(frames / median, 2),
    }


def cmake_identity(binary: Path) -> dict[str, str]:
    cache = binary.parent / "CMakeCache.txt"
    wanted = {
        "CMAKE_BUILD_TYPE",
        "CMAKE_C_COMPILER",
        "CMAKE_C_COMPILER_VERSION",
        "SNESRECOMP_ENABLE_SIMD",
        "SNESRECOMP_BIT_WORDS",
        "AR_WATCHDOG",
        "AR_SANITIZE",
    }
    values: dict[str, str] = {}
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("//") or line.startswith("#") or "=" not in line:
                continue
            key_type, value = line.split("=", 1)
            key = key_type.split(":", 1)[0]
            if key in wanted:
                values[key] = value
    return values


def compare_results(
    current: dict[str, Any],
    baseline_path: Path,
    maximum_regression: float,
    maximum_suite_regression: float,
) -> bool:
    baseline = json.loads(baseline_path.read_text())
    failed = False
    print(f"\nComparison with {baseline_path}:")
    if current["inputs"] != baseline.get("inputs"):
        print("  global ROM/config/save identity changed  FAIL")
        failed = True
    for name, result in current["workloads"].items():
        old = baseline.get("workloads", {}).get(name)
        if not old:
            print(f"  {name:18s} missing from baseline")
            failed = True
            continue
        old_input = old["input"]
        new_input = result["input"]
        if old_input != new_input:
            print(f"  {name:18s} input identity changed")
            failed = True
            continue
        old_seconds = old["performance"]["median_seconds"]
        new_seconds = result["performance"]["median_seconds"]
        delta = (new_seconds / old_seconds - 1.0) * 100.0
        state_matches = old["final_artifact_sha256"] == result["final_artifact_sha256"]
        status = "PASS"
        if delta > maximum_regression or not state_matches:
            status = "FAIL"
            failed = True
        print(
            f"  {name:18s} {delta:+6.2f}%  state={'match' if state_matches else 'DIFF'}  {status}"
        )
    old_suite_fps = baseline.get("suite_geometric_mean_emulated_fps")
    new_suite_fps = current["suite_geometric_mean_emulated_fps"]
    same_suite = set(current["workloads"]) == set(baseline.get("workloads", {}))
    if old_suite_fps and same_suite:
        suite_regression = (old_suite_fps / new_suite_fps - 1.0) * 100.0
        status = "PASS"
        if suite_regression > maximum_suite_regression:
            status = "FAIL"
            failed = True
        print(
            f"  {'suite geometric mean':18s} {suite_regression:+6.2f}%  {status}"
        )
    return not failed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument(
        "--reference-binary",
        type=Path,
        help="run adjacent alternating A/B pairs against this frozen binary",
    )
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--config", type=Path, default=ROOT / "config.ini")
    parser.add_argument("--save", type=Path, default=ROOT / "saves/save.srm")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument(
        "--workload",
        action="append",
        choices=tuple(WORKLOADS),
        help="run only this workload (repeatable; defaults to the full suite)",
    )
    parser.add_argument("--label", default="runner-baseline")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--max-regression-percent", type=float, default=5.0)
    parser.add_argument("--max-suite-regression-percent", type=float, default=3.0)
    args = parser.parse_args()
    if args.runs < 3:
        parser.error("--runs must be at least 3")
    if args.warmups < 0:
        parser.error("--warmups cannot be negative")
    if args.max_regression_percent < 0:
        parser.error("--max-regression-percent cannot be negative")
    if args.max_suite_regression_percent < 0:
        parser.error("--max-suite-regression-percent cannot be negative")
    return args


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    reference_binary = (
        args.reference_binary.resolve() if args.reference_binary else None
    )
    rom = args.rom.resolve()
    config = args.config.resolve()
    save = args.save.resolve()
    required = (binary, rom, config, save)
    if reference_binary is not None:
        required += (reference_binary,)
    missing = [str(path) for path in required if not path.is_file()]
    selected = args.workload or list(DEFAULT_WORKLOADS)
    missing.extend(
        str(ROOT / WORKLOADS[name].replay)
        for name in selected
        if not (ROOT / WORKLOADS[name].replay).is_file()
    )
    if missing:
        print("Missing benchmark input(s):\n  " + "\n  ".join(missing), file=sys.stderr)
        return 2

    print(f"Binary: {binary}")
    if reference_binary is not None:
        print(f"Reference: {reference_binary}")
    print(f"Suite:  {', '.join(selected)}")
    print(f"Method: {args.warmups} warmup(s), {args.runs} measured runs, alternating order")

    # Warm every workload before measuring. Alternating suite order each round
    # prevents one workload from always receiving the coolest or hottest host.
    for warmup_index in range(args.warmups):
        for name in selected:
            warmup_binaries = [binary]
            if reference_binary is not None:
                warmup_binaries.append(reference_binary)
                if warmup_index % 2 != 0:
                    warmup_binaries.reverse()
            for warmup_binary in warmup_binaries:
                role = "candidate" if warmup_binary == binary else "reference"
                print(
                    f"warmup {warmup_index + 1}/{args.warmups}: "
                    f"{name} ({role})",
                    flush=True,
                )
                run_once(warmup_binary, rom, config, save, WORKLOADS[name])

    durations: dict[str, list[float]] = {name: [] for name in selected}
    artifact_hashes: dict[str, dict[str, str]] = {}
    reference_durations: dict[str, list[float]] = {
        name: [] for name in selected
    }
    reference_artifact_hashes: dict[str, dict[str, str]] = {}
    for round_index in range(args.runs):
        order = selected if round_index % 2 == 0 else list(reversed(selected))
        for workload_index, name in enumerate(order):
            if reference_binary is None:
                elapsed, hashes, _ = run_once(
                    binary, rom, config, save, WORKLOADS[name]
                )
                previous = artifact_hashes.setdefault(name, hashes)
                if hashes != previous:
                    raise RuntimeError(
                        f"{name} final artifacts changed between measured runs"
                    )
                durations[name].append(elapsed)
                print(
                    f"run {round_index + 1}/{args.runs}: "
                    f"{name:18s} {elapsed:8.4f} s",
                    flush=True,
                )
                continue

            pair = [("candidate", binary), ("reference", reference_binary)]
            if (round_index + workload_index) % 2 != 0:
                pair.reverse()
            observations: dict[str, tuple[float, dict[str, str]]] = {}
            for role, pair_binary in pair:
                elapsed, hashes, _ = run_once(
                    pair_binary, rom, config, save, WORKLOADS[name]
                )
                observations[role] = (elapsed, hashes)
            candidate_elapsed, candidate_hashes = observations["candidate"]
            reference_elapsed, reference_hashes = observations["reference"]
            previous = artifact_hashes.setdefault(name, candidate_hashes)
            reference_previous = reference_artifact_hashes.setdefault(
                name, reference_hashes
            )
            if candidate_hashes != previous or reference_hashes != reference_previous:
                raise RuntimeError(
                    f"{name} final artifacts changed between measured runs"
                )
            if candidate_hashes != reference_hashes:
                raise RuntimeError(
                    f"{name} final artifacts differ between candidate and reference"
                )
            durations[name].append(candidate_elapsed)
            reference_durations[name].append(reference_elapsed)
            delta = (candidate_elapsed / reference_elapsed - 1.0) * 100.0
            first = pair[0][0][0].upper()
            print(
                f"pair {round_index + 1}/{args.runs}: {name:18s} "
                f"candidate={candidate_elapsed:8.4f} s "
                f"reference={reference_elapsed:8.4f} s "
                f"delta={delta:+6.2f}% first={first}",
                flush=True,
            )

    results: dict[str, Any] = {
        "schema_version": 1,
        "label": args.label,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "source": {
            "git_commit": git_output("rev-parse", "HEAD"),
            "git_describe": git_output("describe", "--always", "--dirty"),
            "working_tree_clean": not bool(git_output("status", "--porcelain")),
        },
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "logical_cpus": os.cpu_count(),
        },
        "method": {
            "warmups": args.warmups,
            "measured_runs": args.runs,
            "clock": "time.perf_counter_ns",
            "order": "alternating workload order by measured round",
            "diagnostics": "AR_NO_RUN_DIR=1; no trace/watch instrumentation",
            "drivers": {"audio": "dummy", "video": "dummy"},
        },
        "build": {
            "binary": file_identity(binary),
            "cmake": cmake_identity(binary),
        },
        "inputs": {
            "rom": file_identity(rom),
            "config": file_identity(config),
            "save": file_identity(save),
        },
        "workloads": {},
    }
    for name in selected:
        results["workloads"][name] = {
            "input": workload_identity(WORKLOADS[name]),
            "performance": summarize(durations[name], WORKLOADS[name].frames),
            "final_artifact_sha256": artifact_hashes[name],
        }

    medians = [
        result["performance"]["median_emulated_fps"]
        for result in results["workloads"].values()
    ]
    results["suite_geometric_mean_emulated_fps"] = round(
        math.exp(sum(math.log(value) for value in medians) / len(medians)), 2
    )

    paired_matches = True
    if reference_binary is not None:
        reference_workloads: dict[str, Any] = {}
        paired_deltas: dict[str, float] = {}
        for name in selected:
            reference_workloads[name] = {
                "performance": summarize(
                    reference_durations[name], WORKLOADS[name].frames
                ),
                "final_artifact_sha256": reference_artifact_hashes[name],
            }
            ratios = [
                candidate / reference - 1.0
                for candidate, reference in zip(
                    durations[name], reference_durations[name], strict=True
                )
            ]
            paired_deltas[name] = round(statistics.median(ratios) * 100.0, 3)
        reference_fps = [
            result["performance"]["median_emulated_fps"]
            for result in reference_workloads.values()
        ]
        reference_suite_fps = round(
            math.exp(
                sum(math.log(value) for value in reference_fps) /
                len(reference_fps)
            ),
            2,
        )
        suite_regression = round(
            (math.exp(
                sum(math.log1p(delta / 100.0) for delta in paired_deltas.values()) /
                len(paired_deltas)
            ) - 1.0) * 100.0,
            3,
        )
        results["paired_reference"] = {
            "build": {
                "binary": file_identity(reference_binary),
                "cmake": cmake_identity(reference_binary),
            },
            "workloads": reference_workloads,
            "suite_geometric_mean_emulated_fps": reference_suite_fps,
            "median_adjacent_delta_percent": paired_deltas,
            "suite_regression_percent": suite_regression,
        }
        full_suite = set(selected) == set(WORKLOADS)
        paired_matches = (
            all(
                delta <= args.max_regression_percent
                for delta in paired_deltas.values()
            )
            and (not full_suite or
                 suite_regression <= args.max_suite_regression_percent)
        )

    print("\nMedians:")
    for name, result in results["workloads"].items():
        performance = result["performance"]
        print(
            f"  {name:18s} {performance['median_seconds']:8.4f} s  "
            f"{performance['median_emulated_fps']:9.2f} emulated fps"
        )
    print(
        "  suite geometric mean: "
        f"{results['suite_geometric_mean_emulated_fps']:.2f} emulated fps"
    )
    if reference_binary is not None:
        paired = results["paired_reference"]
        print("\nAdjacent paired comparison:")
        for name, delta in paired["median_adjacent_delta_percent"].items():
            status = "PASS" if delta <= args.max_regression_percent else "FAIL"
            print(f"  {name:18s} {delta:+6.2f}%  {status}")
        if set(selected) == set(WORKLOADS):
            suite_delta = paired["suite_regression_percent"]
            suite_status = (
                "PASS"
                if suite_delta <= args.max_suite_regression_percent
                else "FAIL"
            )
            print(
                f"  {'suite geometric mean':18s} "
                f"{suite_delta:+6.2f}%  {suite_status}"
            )

    if args.output:
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
        print(f"Wrote {output}")

    matches = paired_matches
    if args.compare:
        matches = compare_results(
            results,
            args.compare.resolve(),
            args.max_regression_percent,
            args.max_suite_regression_percent,
        )
    return 0 if matches else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(2)

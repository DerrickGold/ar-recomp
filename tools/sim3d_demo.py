#!/usr/bin/env python3
"""Run bounded, deterministic SIM 3D implementation checkpoints."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from sim3d.metadata import read_metadata
from sim3d.metadata_report import validate_expectations
from sim3d.trace import (
    summarize, validate, read_trace, read_picker_routine_calls, read_object_evidence
)
from sim3d.artifacts import (
    compare_d1_framebuffer_hashes, validate_d2_artifacts, validate_voxel_visual, validate_d3_artifact
)
from sim3d.checkpoints import (
    load_manifest, resolve, check_stage_pinning
)



ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests" / "fixtures" / "sim3d" / "checkpoints.json"
VOXEL_REFERENCE_ROOT = ROOT / "runs" / "sim3d-voxel-reference"
def command_for(checkpoint: dict, binary: Path, rom: Path,
                config: Path) -> list[str]:
    return [str(binary), str(rom), "--config", str(config)]



def run_suite(args: argparse.Namespace, checkpoints: dict) -> int:
    for name, checkpoint in checkpoints.items():
        check_stage_pinning(name, checkpoint)

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    suite_root = args.artifact_root / f"{stamp}-suite"
    if not args.dry_run:
        suite_root.mkdir(parents=True, exist_ok=False)

    reports = []
    failed = []
    for name in checkpoints:
        command = [
            sys.executable, str(Path(__file__).resolve()),
            "--manifest", str(args.manifest),
            "--checkpoint", name,
            "--rom", str(args.rom),
            "--artifact-root", str(suite_root),
        ]
        # An explicit suite override applies to every checkpoint. Without one,
        # let an individual checkpoint select the build/configuration its
        # contract requires (D7 validates the player-facing release build).
        if args.binary is not None:
            command.extend(("--binary", str(args.binary)))
        if args.config is not None:
            command.extend(("--config", str(args.config)))
        if args.dry_run:
            command.append("--dry-run")
        result = subprocess.run(command, cwd=ROOT, check=False)
        if result.returncode != 0:
            failed.append(name)
            continue
        if args.dry_run:
            continue
        candidates = sorted(suite_root.glob(f"*-{name}/summary.json"))
        if not candidates:
            failed.append(name)
            continue
        reports.append(json.loads(candidates[-1].read_text(encoding="utf-8")))

    if args.dry_run:
        return 1 if failed else 0

    coverage = {
        "schema": "actraiser-sim3d-checkpoint-suite-v1",
        "checkpoint_count": len(checkpoints),
        "passed": [report["checkpoint"] for report in reports],
        "failed": failed,
        "towns": sorted({town for report in reports
                         for town in report["summary"]["towns"]}),
        "picker_operations": sorted({operation for report in reports
                                     for operation in report["summary"][
                                         "picker_operations"]}),
        "miracle_kinds": sorted({kind for report in reports
                                 for kind in report["summary"][
                                     "miracle_kinds"]}),
        "object_evidence": sorted({category for report in reports
                                   for category in report["summary"][
                                       "object_evidence"]}),
        "reports": [str(candidate) for report in reports
                    for candidate in sorted(
                        suite_root.glob(
                            f"*-{report['checkpoint']}/summary.json"))[-1:]],
    }
    coverage_path = suite_root / "coverage.json"
    coverage_path.write_text(json.dumps(coverage, indent=2) + "\n",
                             encoding="utf-8")
    print(json.dumps(coverage, indent=2))
    print(f"[suite] {'PASS' if not failed else 'FAIL'} -> {coverage_path}")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--checkpoint")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--artifact-root", type=Path,
                        default=ROOT / "runs" / "sim3d-checkpoints")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    checkpoints = manifest["checkpoints"]
    if args.list:
        for name, item in checkpoints.items():
            print(f"{name:24} {item['description']}")
        return 0
    if args.all and args.checkpoint:
        parser.error("--all and --checkpoint are mutually exclusive")
    if args.all:
        return run_suite(args, checkpoints)
    if not args.checkpoint:
        parser.error("--checkpoint or --all is required unless --list is used")
    if args.checkpoint not in checkpoints:
        parser.error(f"unknown checkpoint {args.checkpoint!r}")

    checkpoint = checkpoints[args.checkpoint]
    check_stage_pinning(args.checkpoint, checkpoint)
    binary = (args.binary.resolve() if args.binary is not None else
              resolve(ROOT, checkpoint.get(
                  "default_binary", "build/ActRaiserRecomp")).resolve())
    rom = args.rom.resolve()
    config = (args.config.resolve() if args.config is not None else
              resolve(ROOT, checkpoint.get(
                  "default_config", "config.ini")).resolve())
    replay = resolve(ROOT, checkpoint["replay"]).resolve()
    sram_key = "sram_base64" if checkpoint.get("sram_base64") else "sram"
    sram = resolve(ROOT, checkpoint[sram_key]).resolve()
    settings = (resolve(ROOT, checkpoint["settings"]).resolve()
                if checkpoint.get("settings") else None)
    for label, path in (("binary", binary), ("ROM", rom),
                        ("config", config), ("replay", replay),
                        ("SRAM seed", sram)):
        if not path.is_file():
            raise FileNotFoundError(f"{label} not found: {path}")
    if settings is not None and not settings.is_file():
        raise FileNotFoundError(f"settings fixture not found: {settings}")
    if sram_key == "sram_base64":
        try:
            encoded = "".join(sram.read_text(encoding="ascii").split())
            sram_bytes = base64.b64decode(encoded, validate=True)
        except (ValueError, UnicodeError) as error:
            raise ValueError(f"invalid base64 SRAM seed {sram}: {error}") from error
    else:
        sram_bytes = sram.read_bytes()
    if len(sram_bytes) != 8192:
        raise ValueError(
            f"SRAM seed must decode to 8192 bytes, got {len(sram_bytes)} from {sram}")
    sram_sha256 = hashlib.sha256(sram_bytes).hexdigest()
    expected_sram_sha256 = checkpoint.get("sram_sha256")
    if expected_sram_sha256 and sram_sha256 != expected_sram_sha256:
        raise ValueError(
            f"SRAM seed changed: expected {expected_sram_sha256}, "
            f"got {sram_sha256} from {sram}")

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = args.artifact_root / f"{stamp}-{args.checkpoint}"
    trace = output / "state.jsonl"
    function_trace = output / "picker-routines.jsonl"
    d1_trace = output / "d1-metadata.jsonl"
    d1_authentic_trace = output / "d1-authentic-metadata.jsonl"
    console = output / "console.log"
    d1_authentic_console = output / "d1-authentic-console.log"
    routine_console = output / "picker-routines-console.log"
    isolated_sram = output / "seed.srm"
    d1_authentic_sram = output / "authentic-seed.srm"
    summary_path = output / "summary.json"
    d3_visual = bool(checkpoint.get("d3a_visual") or
                     checkpoint.get("d3b_visual") or
                     checkpoint.get("d3c_visual") or
                     checkpoint.get("d4a_visual") or
                     checkpoint.get("d4b_visual") or
                     checkpoint.get("d4c_visual") or
                     checkpoint.get("d5a_visual") or
                     checkpoint.get("effect_visual"))
    d3_label = (str(checkpoint.get("effect_visual_label", "Lightning"))
                if checkpoint.get("effect_visual")
                else "D5a" if checkpoint.get("d5a_visual")
                else "D4c" if checkpoint.get("d4c_visual")
                else "D4b" if checkpoint.get("d4b_visual")
                else "D4a" if checkpoint.get("d4a_visual")
                else "D3c" if checkpoint.get("d3c_visual")
                else "D3b" if checkpoint.get("d3b_visual") else "D3a")
    voxel_visual = bool(checkpoint.get("voxel_visual"))
    # The voxel gate needs the authentic framebuffer hashes to prove the town
    # renderer changed nothing the game computes, and needs the comparison
    # pass that `baseline_env` configures. Both ride the metadata plumbing.
    metadata_checkpoint = bool(
        checkpoint.get("d1_metadata") or checkpoint.get("d2_flat") or
        d3_visual or voxel_visual)
    # A manifest that declares metadata expectations without enabling the
    # metadata pass would silently validate nothing and report PASS.
    if checkpoint.get("expect", {}).get("d1_metadata") and \
            not metadata_checkpoint:
        raise ValueError(
            f"checkpoint {args.checkpoint} expects d1_metadata but enables no "
            "metadata pass; set d1_metadata/d2_flat/d3*_visual/d5a_visual")
    d2_artifact_prefix = (output / "D2-flat").resolve()

    state_env = {key: value for key, value in os.environ.items()
                 if not key.startswith(("AR_", "SNESRECOMP_"))}
    state_env.update({
        "AR_HEADLESS": "1",
        # Visual evidence is collected from runs/*, independent of build preset.
        "AR_ENABLE_RUN_DIR": "1",
        "AR_USER_DATA_DIR": str(output.resolve()),
        "AR_INPUT_REPLAY": str(replay),
        "AR_QUIT_FRAMES": str(int(checkpoint["quit_frames"])),
        "AR_SIM3D_TRACE": str(trace.resolve()),
        "AR_SAVE_NATIVE_PATH": str(isolated_sram.resolve()),
        # Disable the ambient anomaly ring without enabling detailed tracing in
        # the visual-state pass. Old input streams can take a different menu
        # transition path when the much heavier function trace is active.
        "SNESRECOMP_TRACE_FILE": str((output / "disabled-general-trace.jsonl").resolve()),
        "SNESRECOMP_TRACE_HARDWARE_LOW": "999999",
        "SNESRECOMP_TRACE_HARDWARE_HIGH": "999999",
    })
    if settings is not None:
        state_env["AR_SETTINGS_PATH"] = str(settings)
    state_env.update({str(key): str(value)
                      for key, value in checkpoint.get("env", {}).items()})
    if metadata_checkpoint:
        state_env["AR_SIM3D_D1_TRACE"] = str(d1_trace.resolve())
    if checkpoint.get("d2_flat") or d3_visual or voxel_visual or \
            checkpoint.get("headless_video"):
        # Visual checkpoints exercise the same mandatory GPU/D32 path as a
        # normal run. Do not substitute SDL's dummy/software renderer: that
        # would validate a visibility model the game can no longer use.
        state_env["AR_HEADLESS_VIDEO"] = "1"
        state_env.pop("SDL_VIDEODRIVER", None)
        state_env.pop("SDL_RENDER_DRIVER", None)
    if checkpoint.get("d2_flat"):
        state_env.update({
            "AR_SIM3D_D2_DUMP_PREFIX": str(d2_artifact_prefix),
            "AR_SIM3D_D2_DUMP_AT_GF": str(
                int(checkpoint.get("d2_dump_game_frame", 0))),
        })
    if voxel_visual:
        # A series rather than one frame: the town scrolls, so successive
        # frames cover different terrain, different mountain stacks and a
        # different set of models than any single capture would.
        state_env["AR_SHOT_EVERY"] = str(
            int(checkpoint.get("composite_shot_interval", 100)))
        state_env["AR_SHOT_FROM"] = str(
            int(checkpoint.get("composite_shot_from", 900)))
        state_env["AR_SHOT_TO"] = str(
            int(checkpoint.get("composite_shot_to", 2000)))
    if d3_visual:
        state_env["AR_SHOT_AT_GF"] = str(
            int(checkpoint.get("d3a_shot_game_frame", 700)))
        state_env["AR_SHOT_EVERY"] = "1"
        state_env["AR_SHOT_FROM"] = "1000"
        state_env["AR_SHOT_TO"] = "1000"
        state_env["AR_SIM3D_D2_DUMP_PREFIX"] = str(
            (output / f"{d3_label}-planes").resolve())
        state_env["AR_SIM3D_D2_DUMP_AT_GF"] = str(
            int(checkpoint.get("d3a_shot_game_frame", 700)))
    state_env.pop("SNESRECOMP_TRACE_WATCH_FILE", None)

    routine_env = state_env.copy()
    routine_env.pop("AR_SIM3D_TRACE", None)
    routine_env.pop("AR_SIM3D_D1_TRACE", None)
    routine_env.pop("AR_SIMCAT", None)
    routine_env.pop("AR_SHOT_AT_GF", None)
    routine_env.pop("AR_SHOT_EVERY", None)
    routine_env.pop("AR_SHOT_FROM", None)
    routine_env.pop("AR_SHOT_TO", None)
    routine_env.update({
        # A separate evidence pass captures only bank-$01 function entries
        # beginning with 9. Filtering that stream to the three proven picker
        # routines distinguishes Direct the People / Building Direction from
        # targeted miracles,
        # even though both stage pending world type $0009.
        "SNESRECOMP_TRACE_FILE": str(function_trace.resolve()),
        "SNESRECOMP_TRACE_HARDWARE_LOW": "0",
        "SNESRECOMP_TRACE_HARDWARE_HIGH": str(int(checkpoint["quit_frames"])),
        "SNESRECOMP_TRACE_CHANNELS": "func",
        "SNESRECOMP_TRACE_FUNCTION": "bank_01_9",
    })
    command = command_for(checkpoint, binary, rom, config)

    if args.dry_run:
        print(json.dumps({
            "checkpoint": args.checkpoint,
            "command": command,
            "output": str(output),
            "working_directory": str(output),
            "state_env": {key: state_env[key] for key in sorted(state_env)
                          if key.startswith("AR_")},
            "routine_env": {key: routine_env[key] for key in sorted(routine_env)
                            if key.startswith("AR_")},
        }, indent=2))
        return 0

    output.mkdir(parents=True, exist_ok=False)
    isolated_sram.write_bytes(sram_bytes)
    print(f"[{args.checkpoint}] running bounded replay -> {output}")
    result = subprocess.run(
        command, cwd=output, env=state_env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=int(checkpoint.get("timeout_seconds", 60)), check=False)
    console.write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        raise RuntimeError(
            f"checkpoint process exited {result.returncode}; see {console}")
    if not trace.is_file():
        raise RuntimeError(f"checkpoint produced no state trace; see {console}")

    if metadata_checkpoint:
        d1_authentic_sram.write_bytes(sram_bytes)
        authentic_env = state_env.copy()
        authentic_env.pop("AR_SIM3D_TRACE", None)
        authentic_env.pop("AR_SIMCAT", None)
        authentic_env.update({
            "AR_SIM3D": "1" if (d3_visual or voxel_visual) else "0",
            "AR_SIM3D_D1_TRACE": str(d1_authentic_trace.resolve()),
            "AR_SAVE_NATIVE_PATH": str(d1_authentic_sram.resolve()),
        })
        if d3_visual or voxel_visual:
            # The visual gate compares the checkpoint's profile against the
            # stage below it, rendered by a second run of the same replay with
            # that stage switched off by name. There is no in-frame A/B view:
            # one frame renders one profile, and `baseline_env` is what makes
            # the comparison run differ.
            authentic_env.update(
                {str(key): str(value)
                 for key, value in checkpoint.get("baseline_env", {}).items()})
        authentic_result = subprocess.run(
            command, cwd=output, env=authentic_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=int(checkpoint.get("timeout_seconds", 60)), check=False)
        d1_authentic_console.write_text(
            authentic_result.stdout, encoding="utf-8")
        if authentic_result.returncode != 0:
            print(authentic_result.stdout, file=sys.stderr)
            raise RuntimeError(
                "D1 authentic comparison pass exited "
                f"{authentic_result.returncode}; see {d1_authentic_console}")
        if not d1_authentic_trace.is_file():
            raise RuntimeError(
                "D1 authentic comparison produced no metadata trace; see "
                f"{d1_authentic_console}")

    if voxel_visual:
        # A third pass with the SAME configuration. Determinism is not a
        # nicety here: the reference gate below compares this host's pixels
        # against its own recorded pixels, and a capture that varies run to
        # run would report a regression on every invocation.
        repeat_sram = output / "repeat-seed.srm"
        repeat_sram.write_bytes(sram_bytes)
        repeat_env = state_env.copy()
        # Redirected, NOT removed. Arming the metadata trace makes the frame
        # spend a full-surface hash it otherwise skips, so a repeat pass with
        # tracing off is not the same run -- and a determinism gate that
        # varies anything is measuring the variation.
        repeat_env["AR_SIM3D_TRACE"] = str((output / "repeat-state.jsonl").resolve())
        repeat_env["AR_SIM3D_D1_TRACE"] = str(
            (output / "repeat-d1-metadata.jsonl").resolve())
        repeat_env["AR_SAVE_NATIVE_PATH"] = str(repeat_sram.resolve())
        repeat_result = subprocess.run(
            command, cwd=output, env=repeat_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=int(checkpoint.get("timeout_seconds", 60)), check=False)
        (output / "voxel-repeat-console.log").write_text(
            repeat_result.stdout, encoding="utf-8")
        if repeat_result.returncode != 0:
            print(repeat_result.stdout, file=sys.stderr)
            raise RuntimeError(
                "voxel repeat pass exited "
                f"{repeat_result.returncode}; see "
                f"{output / 'voxel-repeat-console.log'}")

    expected_operations = checkpoint.get("expect", {}).get(
        "picker_operations", [])
    if expected_operations or checkpoint.get("trace_picker_routines"):
        routine_result = subprocess.run(
            command, cwd=output, env=routine_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=int(checkpoint.get("timeout_seconds", 60)), check=False)
        routine_console.write_text(routine_result.stdout, encoding="utf-8")
        if routine_result.returncode != 0:
            print(routine_result.stdout, file=sys.stderr)
            raise RuntimeError(
                "picker routine pass exited "
                f"{routine_result.returncode}; see {routine_console}")

    frames = read_trace(trace)
    routine_calls = read_picker_routine_calls(function_trace)
    object_evidence = read_object_evidence(console)
    summary = summarize(frames, routine_calls, object_evidence)
    errors = validate(summary, checkpoint.get("expect", {}))
    d1_summary = None
    if metadata_checkpoint:
        if not d1_trace.is_file():
            raise RuntimeError(
                f"checkpoint produced no D1 metadata trace; see {console}")
        d1_expected = checkpoint.get("expect", {}).get("d1_metadata", {})
        d1_summary, d1_errors = read_metadata(
            d1_trace, d1_expected.get("allowed_unpacked_atlas_objects"))
        # A voxel checkpoint borrows the metadata pass for its framebuffer
        # hashes and its comparison run. It does not adopt D1's own contracts
        # about effect anchors, object planes and pickers -- those belong to
        # the checkpoints that assert them, and reporting them here would bury
        # the one result this checkpoint is about.
        if not voxel_visual or checkpoint.get("d1_metadata"):
            errors.extend(d1_errors)
            errors.extend(validate_expectations(d1_summary, d1_expected))
        hash_compare = compare_d1_framebuffer_hashes(
            d1_trace, d1_authentic_trace)
        if hash_compare["mismatch_count"]:
            errors.append(
                "D1 authentic framebuffer comparison: "
                f"{hash_compare['mismatch_count']} mismatch(es); first="
                f"{hash_compare['first_mismatch']!r}")
        d1_summary["authentic_hash_compare"] = hash_compare
        if voxel_visual and not checkpoint.get("d1_metadata"):
            # Report only what this checkpoint asserts. Carrying D1's full
            # summary here puts thousands of unasserted accounting errors in
            # a PASSING report, which reads as a failure nobody acted on.
            summary["d1_metadata"] = {
                "authentic_hash_compare": hash_compare,
                "note": "borrowed for framebuffer hashes only; D1's own "
                        "contracts belong to the D1 checkpoints",
            }
        else:
            summary["d1_metadata"] = d1_summary
    if checkpoint.get("d2_flat"):
        d2_artifacts, d2_errors = validate_d2_artifacts(d2_artifact_prefix)
        errors.extend(d2_errors)
        summary["d2_flat"] = d2_artifacts
    if voxel_visual:
        voxel_artifact, voxel_errors = validate_voxel_visual(
            output, checkpoint.get("expect", {}).get("voxel_visual", {}),
            VOXEL_REFERENCE_ROOT)
        errors.extend(voxel_errors)
        summary["voxel_visual"] = voxel_artifact
    if d3_visual:
        picker_topdown = bool((d1_summary or {}).get("picker_topdown_build",
                                                     True))
        d3_artifact, d3_errors = validate_d3_artifact(output, d3_label,
                                                      picker_topdown,
                                                      checkpoint.get(
                                                          "max_differing_pixels"),
                                                      bool(checkpoint.get(
                                                          "expect_picker_unchanged")))
        errors.extend(d3_errors)
        summary[f"{d3_label.lower()}_visual"] = d3_artifact
    report = {
        "schema": "actraiser-sim3d-checkpoint-result-v1",
        "checkpoint": args.checkpoint,
        "description": checkpoint["description"],
        "command": command,
        "sram_seed": {
            "source_path": str(sram),
            "isolated_path": str(isolated_sram),
            "sha256": sram_sha256,
        },
        "settings_fixture": ({
            "path": str(settings),
            "sha256": hashlib.sha256(settings.read_bytes()).hexdigest(),
        } if settings is not None else None),
        "summary": summary,
        "validation_errors": errors,
    }
    summary_path.write_text(json.dumps(report, indent=2) + "\n",
                            encoding="utf-8")
    print(json.dumps(report, indent=2))
    if errors:
        print(f"[{args.checkpoint}] FAIL -> {summary_path}", file=sys.stderr)
        return 1
    print(f"[{args.checkpoint}] PASS -> {summary_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError,
            subprocess.TimeoutExpired) as error:
        print(f"sim3d_demo.py: {error}", file=sys.stderr)
        raise SystemExit(2)

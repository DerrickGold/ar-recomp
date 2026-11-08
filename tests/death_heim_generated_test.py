#!/usr/bin/env python3
"""Optional local-ROM Death Heim integration using production generated code.

Requires a current Unix Makefiles game build and the user's supported USA ROM.
Relinks only the diagnostic frame observer, then cold-boots the real application
with copied SRAM and controller input. All player files remain untouched.

This tests loaders, arenas, retries, death animations, hub returns and final-boss
phases. It uses invulnerability and controlled lethal completion after 600 no-input
combat frames, so it is not a player-controlled combat or balance playthrough.
"""

import argparse
import base64
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from act_content import high_bit, iter_script  # noqa: E402
from quintet_lzss import decompress  # noqa: E402

ROSTERS = {"standard": 0, "act1": 1, "extended": 2}
ACT1 = {
    0: (1, 1, 0xAD45), 2: (2, 1, 0xB786), 4: (3, 2, 0xC66F),
    6: (4, 3, 0xD646), 8: (5, 3, 0xD974), 10: (6, 4, 0xE7C6),
}

# A completed rush cannot excuse a generated-code fallback or failed draw.
# These diagnostics can continue execution, so exit status alone misses them.
RUNTIME_FAILURE = re.compile(
    r"\[(?:fatal-session|missing-mx-variant|unresolved-stub|unresolved-goto|"
    r"indirect-unresolved|dispatch-oob|garbage-variant|entry-mx|exit-mx|"
    r"call-mx|exit-stack|rts_dispatch_width|rts_dispatch_miss)\]|"
    r"\bERROR:|\bVK_ERROR_|\[gpu-bg-decode\] native resolve failed|"
    r"\[gpu-bg-capture\] (?:mismatch |skybox mismatch)")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_log(log):
    failures = [line for line in log.splitlines() if RUNTIME_FAILURE.search(line)]
    assert not failures, ("runtime/presentation diagnostics invalidate this run", failures[:12])


def word(memory, address):
    return int.from_bytes(memory[address:address + 2], "little")


def build_probe(build_dir, output):
    flags = (build_dir / "CMakeFiles/ActRaiserRecomp.dir/flags.make").read_text()
    options = []
    for key in ("C_DEFINES", "C_INCLUDES", "C_FLAGS"):
        options += shlex.split(re.search(r"^" + key + r" = (.*)$", flags, re.M)[1])
    compiler = re.search(r"^CMAKE_C_COMPILER:FILEPATH=(.*)$",
                         (build_dir / "CMakeCache.txt").read_text(), re.M)[1]
    hook = ROOT / "tests/fixtures/death_heim_generated_hooks.c"
    obj = output / "death_heim_generated_hooks.o"
    subprocess.run([compiler, *options, "-c", str(hook), "-o", str(obj)], check=True)
    link = shlex.split((build_dir / "CMakeFiles/ActRaiserRecomp.dir/link.txt").read_text())
    link[link.index("CMakeFiles/ActRaiserRecomp.dir/src/dev/oracle_trace.c.o")] = str(obj)
    binary = output / "death-heim-generated"
    link[link.index("-o") + 1] = str(binary)
    subprocess.run(link, cwd=build_dir, check=True)
    return binary


def prepare(output, frames, roster, shuffle, seed, retry_id, captures,
            pip_video=False, settings=None):
    output.mkdir(parents=True)
    (output / "game-assets").symlink_to(ROOT / "game-assets", target_is_directory=True)
    fixtures = ROOT / "tests/fixtures/benchmark"
    (output / "seed.srm").write_bytes(base64.b64decode(
        (fixtures / "action-routes-seed.srm.b64").read_bytes()))
    (output / "config.ini").write_text("# Isolated generated-code integration.\n")
    settings_text = settings.read_text() + (
        "\n# Isolated integration save policy.\nsave_backend = native-srm\n"
        "save_edit_armed = Off\nsave_autobackup = Off\nperformance_overlay = Off\n") \
        if settings else (fixtures / "action-render-settings.ini").read_text() + \
        "\nwindow_scale = 1\ndiorama_mode = Off\nperformance_overlay = Off\n"
    (output / "settings.ini").write_text(settings_text)
    inputs = [0] * (frames + 1)
    boot = json.loads((fixtures / "aitos-r4-natural-inputs.json").read_text())
    for start, length, buttons in boot["pulses"]:
        if start <= 420:
            for frame in range(start, min(start + length, len(inputs))):
                inputs[frame] = buttons
    (output / "input.rec").write_bytes(b"".join(
        struct.pack("<II", frame, buttons) for frame, buttons in enumerate(inputs)))
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("AR_", "SNESRECOMP_", "SDL_"))}
    environment.update(
        AR_USER_DATA_DIR=str(output), AR_SAVE_NATIVE_PATH=str(output / "seed.srm"),
        AR_SAVE_INI_PATH=str(output / "save.ini"),
        AR_SETTINGS_PATH=str(output / "settings.ini"),
        AR_INPUT_REPLAY=str(output / "input.rec"), AR_HEADLESS="1",
        AR_ENABLE_RUN_DIR="1", AR_REPLAY_NOSTOP="1", AR_QUIT_FRAMES=str(frames),
        AR_REFRESH_MODE="Unlimited", AR_WARP="0701", AR_WARP_AT="500",
        AR_INF_HP="1", AR_NATIVE_AUDIO_TRACE="1", AR_MUSICLOG="1",
        SDL_AUDIODRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
        AR_DEATH_HEIM_TEST_EVENTS=str(output / "events.jsonl"),
        AR_DEATH_HEIM_TEST_DIRECTORY=str(output),
        AR_DEATH_HEIM_TEST_ROSTER=str(roster),
        AR_DEATH_HEIM_TEST_SHUFFLE=str(int(shuffle)),
        AR_DEATH_HEIM_TEST_SEED=str(seed), AR_DEATH_HEIM_TEST_RETRY=str(retry_id))
    if pip_video:
        environment.update(AR_HEADLESS_VIDEO="1", AR_TEST_RENDER_VIEW="pip")
    if captures:
        environment.update(AR_SHOT_EVERY="4", AR_SHOT_FROM="600",
                           AR_SHOT_TO=str(frames - 100))
        if pip_video:
            environment.update(AR_SHOT_REQUIRE_COMPOSITE="1",
                               AR_FRAME_CAPTURE_TICK_CLOCK="1")
    (output / "environment.json").write_text(json.dumps(
        {key: value for key, value in environment.items()
         if key.startswith(("AR_", "SDL_"))}, indent=2) + "\n")
    return environment


def expected_assets(rom, city, room):
    expected = {}
    for group, current, commands in iter_script(rom):
        if group != city or not 1 <= current <= room:
            continue
        for command, data in commands:
            if high_bit(command) == 0:
                address = 0x5000 if data[0] else 0x4000
                expected[address] = decompress(rom, int.from_bytes(data[3:6], "little"))[0]
    return expected


def expected_background_animation(rom, city, room):
    """Reconstruct the ordinary room chain's native CHR capture from ROM.

    Video-profile byte24 bit7 preserves an earlier room's animation cache.
    Replaying uploads alone does not establish that cache: native $02:BAF5
    captures 4 KiB after each room whose profile starts a new animation.
    """
    banks = {}
    cache = None
    profile = None
    for group, current, commands in iter_script(rom):
        if group != city or not 1 <= current <= room:
            continue
        for command, data in commands:
            if high_bit(command) == 7 and data[2] in (0x00, 0x10):
                blob = decompress(rom, int.from_bytes(data[3:6], "little"))[0]
                assert len(blob) == word(data, 0) * 2 == 8192
                banks[data[2] << 8] = blob
            elif high_bit(command) == 3:
                start = 0x1093e + data[0] * 28
                profile = rom[start:start + 28]
        assert profile is not None, (city, current, "missing video profile")
        phases = profile[23] & 15
        stride = ((profile[23] >> 4) & 7) << 7
        target = 0x1000 if profile[23] & 0x80 else 0
        if phases and stride and not profile[24] & 0x80:
            cache = banks[target][:4096]
    if not phases or not stride:
        return None
    assert cache is not None and phases * stride <= len(cache), (city, room)
    return cache, target, stride, phases


def validate_background_animation(path, rom, city, room, memory):
    expected = expected_background_animation(rom, city, room)
    if expected is None:
        return None
    cache, target, stride, phases = expected
    assert memory[0x1b800:0x1c800] == cache, \
        (path, "native background animation cache differs from prerequisite room")
    vram = path.with_name(path.name.replace(".wram.bin", ".vram.bin")).read_bytes()
    live = vram[target * 2:target * 2 + stride]
    matching = [phase for phase in range(phases)
                if live == cache[phase * stride:(phase + 1) * stride]]
    assert matching, (path, "animated terrain tiles differ from every native phase")
    return {"cache_sha256": hashlib.sha256(cache).hexdigest(), "cache_bytes": len(cache),
            "vram_word": target, "phase_bytes": stride, "matching_phases": matching}


def validate_presentation(output, events, required, captures):
    """Native behavior/presentation evidence independent of host Combat flags."""
    failures = [row for row in events if row["event"] == "acceptance_failed"]
    assert not failures, ("boss did not visibly engage without input within 120 frames", failures)
    inputs = (output / "input.rec").read_bytes()
    assert all(buttons == 0 for frame, buttons in struct.iter_unpack("<II", inputs)
               if frame >= 500), "arrival/combat probe contains controller input"
    runs = [path for path in (output / "runs").iterdir()
            if path.is_dir() and not path.is_symlink()]
    run = max(runs, key=lambda path: path.stat().st_mtime)
    song_path = run / "native_audio_song_events.csv"
    assert song_path.exists(), "native APU song trace is missing"
    with song_path.open() as stream:
        song_events = list(csv.DictReader(stream))
    assert song_events, "empty APU trace cannot prove music continuity"
    # This trace is bounded; a dropped song event would invalidate absence proof.
    assert "song-overflow=" not in (output / "capture.log").read_text() or \
        "song-overflow=0" in (output / "capture.log").read_text()
    samples = [row for row in events if row["event"] == "sample"]
    evidence = []
    for fight in [row for row in events if row["event"] == "fight" and
                  row["age"] == 600 and row["id"] in ACT1]:
        encounter_id = fight["id"]
        window = [row for row in samples if row["id"] == encounter_id and
                  row["phase"] == 3 and row["gf"] <= fight["gf"]][-600:]
        assert len(window) == 600, ("missing no-input observation window", encounter_id)
        assert all(row["controller"] == 0 for row in window), ("unexpected input", encounter_id)
        assert all(row["input_mask"] == 0xffff for row in window), \
            ("player control was not restored after arrival", encounter_id)
        assert any(row["native_active"] for row in window[:120]), \
            ("boss still waits on its native introduction", encounter_id)
        assert any(row["attack"] for row in window), ("no native attack observed", encounter_id)
        assert 0 < fight["first_visible_age"] <= 120 and fight["visible_frames"] >= 30, \
            ("boss body stayed offscreen", encounter_id, fight)
        begin = next(index for index, row in enumerate(events) if row is window[0])
        previous = max((index for index, row in enumerate(events[:begin])
                        if row["event"] == "transition" and
                        row["id"] == encounter_id and row["phase"] == 2), default=-1)
        assert previous >= 0, ("arrival preparation was not observed", encounter_id)
        # Include the hub departure before loading starts, not only the
        # post-load transition carrying the same Preparing phase.
        earlier = [index for index, row in enumerate(events[:previous])
                   if row["event"] == "transition"]
        for index in reversed(earlier):
            if events[index]["id"] != encounter_id or events[index]["phase"] != 2:
                break
            previous = index
        arrival = [row for row in events[previous:begin] if row["event"] == "sample" and
                   row["id"] == encounter_id and row["animation"] == 0x4000 and
                   row["state"] == 14]
        assert len(arrival) >= 2 and len({row["visual"] for row in arrival}) >= 2, \
            ("native arrival beam did not visibly animate", encounter_id)
        assert all(row["input_mask"] == 0 and row["player_status"] & 0x0800
                   for row in arrival), ("arrival beam did not protect/lock player", encounter_id)
        assert all(not row["native_active"] for row in arrival)
        # Captured cache identity must agree with the last hub; APU writes
        # independently prove the same image was not halted/restarted.
        hub = next(row for row in reversed(events[:previous])
                   if row["group"] == 7 and row["map"] == 1)
        assert hub["music_source"] != 0
        assert all(row["music_source"] == hub["music_source"]
                   for row in arrival + window), ("Act1 changed the rush music image", encounter_id)
        interruptions = [row for row in song_events
                         if events[previous]["host"] <= int(row["frame"]) <= fight["host"] and
                         (int(row["value"], 16) in (0xf1, 0xf0, 0xff, 1) or
                          row["event"] == "image_uploaded")]
        assert not interruptions, ("Act1 restarted/uploaded music", encounter_id, interruptions)
        if captures:
            assert any((run / f"shot_{row['gf']}.png").exists() for row in arrival), \
                ("arrival beam framebuffer capture missing", encounter_id)
        evidence.append({"id": encounter_id, "combat_gf": window[0]["gf"],
                         "observed_frames": len(window),
                         "first_visible_age": fight["first_visible_age"],
                         "visible_frames": fight["visible_frames"],
                         "arrival_beam_frames": len(arrival), "music_source": hub["music_source"]})
    assert {row["id"] for row in evidence} == set(required) & set(ACT1), \
        ("missing 600-frame arena acceptance", evidence)
    for lethal in [row for row in events if row["event"] == "lethal" and row["id"] in ACT1]:
        begin = next(index for index, row in enumerate(events) if row is lethal)
        finish = next(index for index, row in enumerate(events[begin + 1:], begin + 1)
                      if row["group"] == 7 and row["map"] == 1)
        departure = [row for row in events[begin:finish] if row["event"] == "sample" and
                     row["animation"] == 0x4000 and row["state"] == 15]
        assert len(departure) >= 2 and len({row["visual"] for row in departure}) >= 2, \
            ("native departure beam did not visibly animate", lethal["id"])
        assert all(row["input_mask"] == 0 and row["player_status"] & 0x0800
                   for row in departure), ("departure beam did not lock player", lethal["id"])
        assert all(row["progress"] == lethal["progress"] and row["eyes"] == lethal["eyes"]
                   for row in departure), \
            ("completion committed before teleport finished", lethal["id"])
        # Death and the return-room loader must preserve playback too. A
        # preserved AB/AD cache alone would miss a halt/play of the same song.
        returning = events[begin:finish + 1]
        assert all(row["music_source"] == lethal["music_source"] for row in returning), \
            ("Act1 defeat/return changed the rush music image", lethal["id"])
        interruptions = [row for row in song_events
                         if lethal["host"] <= int(row["frame"]) <= events[finish]["host"] and
                         (int(row["value"], 16) in (0xf1, 0xf0, 0xff, 1) or
                          row["event"] == "image_uploaded")]
        assert not interruptions, ("Act1 defeat/return interrupted music",
                                   lethal["id"], interruptions)
        if captures:
            assert any((run / f"shot_{row['gf']}.png").exists() for row in departure), \
                ("departure beam framebuffer capture missing", lethal["id"])
    return evidence


def validate(output, rom, roster, shuffle, retry_id, captures=False, pip_video=False):
    log = (output / "capture.log").read_text()
    validate_log(log)
    events = [json.loads(line) for line in (output / "events.jsonl").read_text().splitlines()]
    assert sum(row["event"] == "configure" for row in events) == 1
    compositor = None
    if pip_video:
        cadence = re.search(r"\[present-cadence\] tick-presents=(\d+) re-presents=(\d+)", log)
        assert cadence and int(cadence[1]) > 0, "PiP run did not submit compositor frames"
        rush = [row for row in events if row["event"] == "sample" and row["id"] <= 12]
        assert rush and all(row["comparison_view"] == 2 for row in rush), \
            "rush did not remain in actual PiP comparison mode"
        compositor = {"view": "pip", "tick_presents": int(cadence[1]),
                      "re_presents": int(cadence[2]), "window": "hidden; PiP inset"}
    required = list(range(1, 12, 2)) if roster == 0 else \
        list(range(0, 12, 2)) if roster == 1 else list(range(12))
    presentation = validate_presentation(output, events, required, captures)
    lethal = [row for row in events if row["event"] == "lethal"]
    sequence = [row["id"] for row in lethal]
    assert sequence[-2:] == [12, 12], ("final boss must finish both forms", sequence)
    assert len(sequence) == len(required) + 2, sequence
    assert sorted(sequence[:-2]) == required, sequence
    if not shuffle:
        assert sequence[:-2] == required, sequence
    assert [row["postdeath"] for row in lethal[-2:]] == [0xF8F5, 0xFE89]
    assert lethal[-2]["owner"] == lethal[-1]["owner"], \
        "diagnostic damage switched from Tanzra's root to an inherited-source child"
    # No borrowed Act 1 arena may return to the campaign clear-card engine.
    start = next(index for index, row in enumerate(events) if row["event"] == "lethal")
    finish = next(index for index, row in enumerate(events)
                  if index > start and row["progress"] == 7)
    assert all(row["group"] != 0 for row in events[start:finish])
    assert any(row["group"] == 7 and row["map"] == 1 and row["progress"] == 7
               for row in events[finish:]), "final native completion hub was not reached"
    if roster or shuffle:
        assert any(row["phase"] == 7 for row in events[finish:]), "custom run not complete"

    assets = []
    seen = set()
    raw_rom = rom.read_bytes()
    for path in sorted(output.glob("arena*-e*-age*.wram.bin")):
        if int(re.search(r"-age(\d+)", path.name)[1]) not in (60, 600):
            continue  # The temporary beam deliberately borrows resident graphics.
        encounter_id = int(re.search(r"-e(\d+)-", path.name)[1])
        if encounter_id not in ACT1:
            continue
        city, room, source = ACT1[encounter_id]
        memory = path.read_bytes()
        assert list(memory[0x18:0x1a]) == [city, room], path
        assert word(memory, 0x1312) == source, path
        assert memory[0x1d] > 0 and word(memory, 0x8a) == 0x8a0, path
        # Native camera must already contain the player; delayed following
        # after a raw coordinate jump misses Northwall's vertical arena.
        px, py = word(memory, 0x8a2), word(memory, 0x8a4)
        cx, cy = word(memory, 0x22), word(memory, 0x24)
        assert 0 <= px - cx <= 256 and 0 <= py - cy <= 224, (path, px, py, cx, cy)
        matches = {}
        for address, blob in expected_assets(raw_rom, city, room).items():
            actual = memory[address:address + len(blob)]
            assert actual == blob, (path, hex(address), "inherited native assets differ")
            matches[hex(address)] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
        animation = validate_background_animation(path, raw_rom, city, room, memory)
        assets.append({"file": path.name, "sha256": digest(path), "id": encounter_id,
                       "player": [px, py], "camera": [cx, cy], "assets": matches,
                       "background_animation": animation})
        seen.add(encounter_id)
    assert seen == set(required) & set(ACT1), ("missing natural arenas", seen)

    if retry_id >= 0:
        timeout = [row for row in events if row["event"] == "timeout"]
        assert len(timeout) == 1 and timeout[0]["id"] == retry_id
        retry = next(row for row in events if row["gf"] > timeout[0]["gf"] and row["phase"] == 5)
        renewed = next(row for row in events if row["gf"] > retry["gf"] and
                       row["id"] == retry_id and row["phase"] in (3, 6))
        assert renewed["progress"] == timeout[0]["progress"], (timeout, renewed)
        assert renewed["eyes"] == timeout[0]["eyes"], (timeout, renewed)
        assert (renewed["hp"], renewed["lives"], renewed["magic"]) == (24, 2, 3), renewed
        assert (renewed["group"], renewed["map"]) == \
            (timeout[0]["group"], timeout[0]["map"]), renewed
    return {"sequence": sequence, "retry_id": retry_id, "assets": assets,
            "presentation": presentation, "compositor": compositor,
            "settings_sha256": digest(output / "settings.ini"),
            "final_completion_gf": events[finish]["gf"], "event_count": len(events)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-release")
    parser.add_argument("--output", type=Path, default=ROOT / "build-check/death-heim")
    parser.add_argument("--rom", type=Path, default=ROOT / "ar.sfc")
    parser.add_argument("--roster", choices=ROSTERS, default="act1")
    parser.add_argument("--shuffle", action="store_true")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--retry-id", type=int, default=-1,
                        help="Force one native timeout: encounter IDs 0..11 or final boss 12")
    parser.add_argument("--matrix", action="store_true",
                        help="Run all three rosters with shuffle off/on")
    parser.add_argument("--frames", type=int, help="Override the bounded host-frame budget")
    parser.add_argument("--captures", action="store_true",
                        help="Also capture native framebuffer PNGs every four frames")
    parser.add_argument("--pip-video", action="store_true",
                        help="Exercise real PiP compositor draws in a hidden GPU window")
    parser.add_argument("--settings", type=Path,
                        help="Read-only settings source to copy into each isolated run")
    args = parser.parse_args()
    if not 0 <= args.seed <= 999999999:
        parser.error("seed must be 0..999999999")
    if not -1 <= args.retry_id <= 12:
        parser.error("retry ID must be 0..12, or -1 to disable the timeout probe")
    if args.frames is not None and args.frames < 1:
        parser.error("frames must be positive")
    if args.settings is not None and not args.settings.is_file():
        parser.error("settings source must be a readable file")
    cases = [(name, shuffle) for name in ROSTERS for shuffle in (False, True)] \
        if args.matrix else [(args.roster, args.shuffle)]
    if args.retry_id >= 0:
        required = set(range(1, 12, 2)) if args.roster == "standard" else \
            set(range(0, 12, 2)) if args.roster == "act1" else set(range(12))
        if args.matrix or args.retry_id not in required | {12}:
            parser.error("retry requires one selected roster and one of its encounter IDs")
        if args.roster == "standard" and not args.shuffle:
            parser.error("native Standard/off has no custom retry token; "
                         "use --shuffle for this probe")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    occupied = [output / (name + ("-shuffled" if shuffle else "-ordered"))
                for name, shuffle in cases]
    if any(path.exists() for path in occupied):
        parser.error("case output already exists; choose a new --output directory")
    binary = build_probe(args.build_dir.resolve(), output)
    reports = []
    for (name, shuffle), case in zip(cases, occupied):
        frames = args.frames or (26000 if name == "extended" else 17000)
        environment = prepare(case, frames, ROSTERS[name], shuffle, args.seed,
                              args.retry_id, args.captures, args.pip_video, args.settings)
        with (case / "capture.log").open("w") as log:
            result = subprocess.run([str(binary), str(args.rom.resolve()), "--config",
                                     str(case / "config.ini")], cwd=ROOT, env=environment,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    timeout=max(120, frames / 40 + 30))
        assert result.returncode == 0, (result.returncode, case / "capture.log")
        report = validate(case, args.rom.resolve(), ROSTERS[name], shuffle,
                          args.retry_id, args.captures, args.pip_video)
        report.update(roster=name, shuffle=shuffle, seed=args.seed, output=str(case))
        (case / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        reports.append(report)
        print(f"PASS {name} shuffle={shuffle} retry={args.retry_id}: "
              f"{report['sequence']}", flush=True)
    manifest = {"binary_sha256": digest(binary), "rom_sha256": digest(args.rom),
                "script_sha256": digest(__file__),
                "observer_sha256": digest(ROOT / "tests/fixtures/death_heim_generated_hooks.c"),
                "pip_video": args.pip_video,
                "scope": ("Cold boot; 600 no-input frames, native boss attacks/body visibility, "
                          "animated entry/exit beams and continuous rush music in each Act1 arena; "
                          "inherited assets/camera/background animation, controlled native deaths, retries, hub return "
                          "and final two forms. Invulnerability is active; "
                          "this is not combat balance or the full ending tour."),
                "cases": reports}
    (output / "results.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Evidence: {output / 'results.json'}")


if __name__ == "__main__":
    main()

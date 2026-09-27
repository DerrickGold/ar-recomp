"""Real-game last-town and soft-reset regression, using an isolated save copy.

Usage: python3 tests/world_resume_boot_test.py <game> <rom> <developed-save.srm>
Requires a save with Northwall available and an acknowledged regional companion.
For a legacy test seed, pass --prepare-with <actraiser_regional_campaign_test>:
that helper creates metadata only for the isolated copy. Run from the project's asset directory.
No replay is used: replay deliberately disables town-bookmark persistence.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("game", "rom", "seed"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--prepare-with", type=Path)
    args = parser.parse_args()
    game, rom, seed = (getattr(args, name).resolve() for name in ("game", "rom", "seed"))
    original = seed.read_bytes()
    if len(original) != 8192:
        parser.error("seed must be an 8192-byte native SRAM image")
    progress = original[0x120a] * 2 + (original[0x13c0] & 1)
    if progress not in (2, 3, 4):
        parser.error(f"Northwall must be unlocked in the saved image (state={progress}); "
                     "a debug warp does not unlock a town for Continue")
    checkpoint = Path(str(seed) + ".archeckpoint")
    if not checkpoint.exists() and not args.prepare_with:
        parser.error("legacy seed needs --prepare-with; headless Continue cannot acknowledge history")
    with tempfile.TemporaryDirectory(prefix="actraiser-world-resume-") as directory:
        root = Path(directory)
        save = root / "save.srm"
        if checkpoint.exists():
            shutil.copyfile(seed, save)
            shutil.copyfile(checkpoint, Path(str(save) + ".archeckpoint"))
        else:
            subprocess.run([str(args.prepare_with.resolve()), "--prepare-world-resume",
                            str(seed), str(save)], check=True, cwd=root)
        config = root / "config.ini"
        config.write_text("# Isolated world-resume fixture.\n")
        # Optional presentation assets are read-only; all settings and saves
        # belong to this temporary data root, never the supplied campaign.
        assets = Path("game-assets").resolve()
        if assets.is_dir():
            (root / "game-assets").symlink_to(assets, target_is_directory=True)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("AR_", "SNESRECOMP_"))}
        env.update(SDL_AUDIODRIVER="dummy", AR_HEADLESS="1", AR_SIM3D="0",
                   AR_USER_DATA_DIR=str(root), AR_SAVE_NATIVE_PATH=str(save),
                   AR_REMEMBER_LAST_TOWN="1", AR_ENABLE_RUN_DIR="1",
                   AR_FORCE_INPUT_MASK="1", AR_SIM3D_WORLD_NAV="1",
                   AR_SIM3D_SKY_PALACE="1")

        def run(**options):
            result = subprocess.run([str(game), str(rom), "--config", str(config)], cwd=root,
                                    env=dict(env, **options), capture_output=True,
                                    text=True, timeout=30)
            output = result.stdout + result.stderr
            assert result.returncode == 0, output[-10000:]
            assert "[fatal-session]" not in output, output[-10000:]
            assert "[missing-mx-variant]" not in output, output[-10000:]
            assert "[regional] continued campaign rules session ready" in output, output[-10000:]
            return output

        output = run(AR_FORCE_PULSES="60,180,300,600,900",
                     AR_WARP="0006", AR_WARP_AT="1000", AR_QUIT_FRAMES="1250")
        assert "after town entry" in output, output[-10000:]
        bookmark = Path(str(save) + ".artown").read_bytes()
        assert len(bookmark) == 25 and bookmark[16] == 6, bookmark
        assert save.read_bytes() == original, "A visit must not save gameplay"

        # Restart while the Palace is waiting for dialogue input. The abandoned
        # inline RDNMI wait used to leave its reentrancy guard set and hang the
        # next Palace load. A title-only restart test cannot catch that bug.
        output = run(AR_FORCE_PULSES="60,180,300,600",
                     AR_SETTING_SET="restart_game=run", AR_SETTING_AT_GF="1100",
                     AR_QUIT_FRAMES="1600")
        assert output.count("[lifecycle] game session ready;") == 2, output[-10000:]
        assert output.count("[lifecycle] soft reset;") == 1, output[-10000:]
        assert output.count("restored town=6 focus=384,128") == 2, output[-10000:]
        assert output.count("after Sky Palace entry") == 2, output[-10000:]
        assert "non-yieldable-context spin" not in output, output[-10000:]
        assert Path(str(save) + ".artown").read_bytes() == bookmark
        assert save.read_bytes() == original and seed.read_bytes() == original
        print("Northwall visit, cold Continue and Palace soft reset: pass (source save untouched)")


if __name__ == "__main__":
    main()

"""Real-game last-town and soft-reset regression, using an isolated save copy.

Usage: python3 tests/world_resume_boot_test.py <game> <rom> <developed-save.srm>
Requires a save with Northwall available. Run from the project's asset directory.
No replay is used: replay deliberately disables town-bookmark persistence.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    game, rom, seed = (Path(arg).resolve() for arg in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="actraiser-world-resume-") as directory:
        root = Path(directory)
        save = root / "save.srm"
        shutil.copyfile(seed, save)
        checkpoint = Path(str(seed) + ".archeckpoint")
        if checkpoint.exists():
            shutil.copyfile(checkpoint, Path(str(save) + ".archeckpoint"))
        # Optional presentation assets are read-only; all settings and saves
        # belong to this temporary data root, never the supplied campaign.
        assets = Path("game-assets").resolve()
        if assets.is_dir():
            (root / "game-assets").symlink_to(assets, target_is_directory=True)
        env = {key: value for key, value in os.environ.items() if not key.startswith("AR_")}
        env.update(SDL_AUDIODRIVER="dummy", AR_HEADLESS="1", AR_SIM3D="0",
                   AR_USER_DATA_DIR=str(root), AR_SAVE_NATIVE_PATH=str(save),
                   AR_REMEMBER_LAST_TOWN="1", AR_ENABLE_RUN_DIR="1",
                   AR_FORCE_INPUT_MASK="1", AR_SIM3D_WORLD_NAV="1",
                   AR_SIM3D_SKY_PALACE="1")

        def run(**options):
            result = subprocess.run([str(game), str(rom)], cwd=root,
                                    env=dict(env, **options), capture_output=True,
                                    text=True, timeout=30)
            output = result.stdout + result.stderr
            assert result.returncode == 0, output[-10000:]
            assert "[fatal-session]" not in output, output[-10000:]
            return output

        output = run(AR_FORCE_PULSES="60,180,300,600,900",
                     AR_WARP="0006", AR_WARP_AT="1000", AR_QUIT_FRAMES="1250")
        assert "after town entry" in output, output[-10000:]
        bookmark = Path(str(save) + ".artown").read_bytes()
        assert len(bookmark) == 25 and bookmark[16] == 6, bookmark
        assert save.read_bytes() == seed.read_bytes(), "A visit must not save gameplay"

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
        print("Northwall visit, cold Continue and Palace soft reset: pass (source save untouched)")


if __name__ == "__main__":
    main()

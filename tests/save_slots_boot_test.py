"""Exercise prepared-slot acknowledgement with the real game and a local ROM.

Usage: python3 tests/save_slots_boot_test.py <game> <save-slots-test> <rom>
Requires the game's normal GPU driver; briefly opens the game five times.
All settings, save metadata and captures stay in a fresh temporary directory.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    game, fixture, rom = (str(Path(arg).resolve()) for arg in sys.argv[1:])
    env = {key: value for key, value in os.environ.items() if not key.startswith("AR_")}
    env.update(SDL_AUDIODRIVER="dummy", AR_QUIT_FRAMES="60", AR_NO_RUN_DIR="1")
    with tempfile.TemporaryDirectory(prefix="actraiser-slots-boot-") as directory:
        root = str(Path(directory) / "saves")
        subprocess.run([fixture, "--prepare-boot", root], check=True, env=env, cwd=directory)
        for restart in range(2):
            result = subprocess.run([game, rom], cwd=directory, env=env,
                                    capture_output=True, text=True, timeout=45)
            output = result.stdout + result.stderr
            if result.returncode or "seed 424242 applied" not in output:
                raise AssertionError(f"Boot {restart + 1} failed ({result.returncode}):\n{output[-12000:]}")
            subprocess.run([fixture, "--verify-boot", root], check=True, env=env, cwd=directory)
        print("Save slots: real game boot and unsaved restart preserve seed 424242 and destination.")
    with tempfile.TemporaryDirectory(prefix="actraiser-legacy-boot-") as directory:
        root = str(Path(directory) / "saves")
        subprocess.run([fixture, "--prepare-legacy-boot", root], check=True, env=env, cwd=directory)
        result = subprocess.run([game, rom], cwd=directory, env=env,
                                capture_output=True, text=True, timeout=45)
        output = result.stdout + result.stderr
        if result.returncode or "legacy native adoption" not in output:
            raise AssertionError(f"Legacy boot failed ({result.returncode}):\n{output[-12000:]}")
        subprocess.run([fixture, "--verify-legacy-boot", root], check=True, env=env, cwd=directory)
        print("Save slots: legacy raw save adopts into Slot 1 without inventing a checkpoint requirement.")
    # Native launchers pass their resolved portable/custom/global root. The
    # game must honor it even when launched from an unrelated directory, and
    # all slot metadata must survive relocating that complete data tree.
    with tempfile.TemporaryDirectory(prefix="actraiser-data-root-") as directory:
        base = Path(directory)
        caller = base / "caller"
        caller.mkdir()
        profile = base / "Profile é セーブ"
        profile.mkdir()
        root = profile / "saves"
        subprocess.run([fixture, "--prepare-boot", str(root)], check=True, env=env, cwd=caller)
        for moved in range(2):
            selected = dict(env, AR_USER_DATA_DIR=os.path.relpath(profile, caller))
            result = subprocess.run([game, rom], cwd=caller, env=selected,
                                    capture_output=True, text=True, timeout=45)
            output = result.stdout + result.stderr
            if result.returncode or "seed 424242 applied" not in output:
                raise AssertionError(f"Selected-root boot failed ({result.returncode}):\n{output[-12000:]}")
            subprocess.run([fixture, "--verify-boot", str(root)], check=True, env=env, cwd=caller)
            assert not (caller / "saves").exists(), "save data escaped into the launch directory"
            assert not (caller / "settings.ini").exists(), "settings escaped into the launch directory"
            if not moved:
                destination = base / "Moved profile"
                profile.rename(destination)
                profile = destination
                root = profile / "saves"
        print("Save slots: launcher-selected Unicode data root and relocated collection retain the prepared campaign.")


if __name__ == "__main__":
    main()

"""Exercise save setup, legacy import and soft resets with the real game.

Usage: python3 tests/save_slots_boot_test.py <game> <save-slots-test> <rom>
Requires a local ROM and the game's normal GPU driver; briefly opens the game.
All settings, save metadata and captures stay in a fresh temporary directory.
"""

import os
import re
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    game, fixture, rom = (str(Path(arg).resolve()) for arg in sys.argv[1:])
    env = {key: value for key, value in os.environ.items() if not key.startswith("AR_")}
    env.update(SDL_AUDIODRIVER="dummy", AR_QUIT_FRAMES="60", AR_NO_RUN_DIR="1")
    with tempfile.TemporaryDirectory(prefix="actraiser-slots-boot-") as directory:
        root = str(Path(directory) / "saves")
        subprocess.run([fixture, "--prepare-boot", root], check=True, env=env, cwd=directory)
        for restart in range(2):
            selected = dict(env, AR_SETTING_SET="restart_game=run", AR_SETTING_AT_GF="8",
                            AR_SETTING_SET_2="restart_game=run", AR_SETTING_AT_GF_2="200",
                            AR_QUIT_FRAMES="360", AR_SFXCENSUS="1")
            result = subprocess.run([game, rom], cwd=directory, env=selected,
                                    capture_output=True, text=True, timeout=60)
            output = result.stdout + result.stderr
            if result.returncode or "seed 424242 applied" not in output:
                raise AssertionError(f"Boot {restart + 1} failed ({result.returncode}):\n{output[-12000:]}")
            sessions = re.findall(r"game session ready; window (\d+); frame (\d+)", output)
            assert len(sessions) == 3 and len(set(sessions)) == 1, output[-12000:]
            assert sessions[0][0] != "0" and sessions[0][1] == "0", sessions
            assert output.count("[lifecycle] soft reset;") == 2, output[-12000:]
            assert output.count("[sfx-census] enabled") == 3, output[-12000:]
            assert "[sfx-census] audio observer unavailable" not in output, output[-12000:]
            assert "opened save setup" not in output, "prepared campaigns must bypass first-run setup"
            subprocess.run([fixture, "--verify-boot", root], check=True, env=env, cwd=directory)
        print("Save slots: real game boot and unsaved restart preserve seed 424242 and destination.")
        imports = Path(root) / "imports"
        source = Path(directory) / "legacy-source"
        subprocess.run([fixture, "--prepare-legacy-boot", str(source)], check=True, env=env)
        shutil.copyfile(source / "actraiser.srm", imports / "import.srm")
        selected = dict(env, AR_SETTING_SET="save_import=run", AR_SETTING_AT_GF="8",
                        AR_QUIT_FRAMES="360")
        result = subprocess.run([game, rom], cwd=directory, env=selected,
                                capture_output=True, text=True, timeout=45)
        output = result.stdout + result.stderr
        assert result.returncode == 0 and "[save-editor] imported" in output, output[-12000:]
        sessions = re.findall(r"game session ready; window (\d+); frame (\d+)", output)
        assert len(sessions) == 2 and len(set(sessions)) == 1, output[-12000:]
        subprocess.run([fixture, "--verify-import-boot", root], check=True, env=env)
        print("Save import: imported campaign reloads in the existing process and window.")
    with tempfile.TemporaryDirectory(prefix="actraiser-empty-boot-") as directory:
        with open(Path(directory) / "boot.log", "w+") as log:
            process = subprocess.Popen([game, rom], cwd=directory, env=env,
                                       stdout=log, stderr=subprocess.STDOUT, text=True)
            try:
                deadline = time.monotonic() + 30
                output = ""
                while time.monotonic() < deadline:
                    log.seek(0)
                    output = log.read()
                    if "game session ready;" in output or process.poll() is not None:
                        break
                    time.sleep(0.05)
                assert "opened save setup before the first game tick" in output, output[-12000:]
                time.sleep(2)
                assert process.poll() is None, "first-run setup must pause before the title starts"
                assert not list(Path(directory).glob("saves/slots/*/save.*"))
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)
        if os.name != "nt":
            dump = (Path(directory) / "saves/dump_state.txt").read_text()
            assert "frame=0 " in dump, dump[:1000]
        print("Fresh install: save setup opens and holds the game before its first tick.")
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
    with tempfile.TemporaryDirectory(prefix="actraiser-root-save-boot-") as directory:
        root = Path(directory) / "saves"
        subprocess.run([fixture, "--prepare-legacy-boot", str(root)], check=True, env=env)
        (root / "actraiser.srm").rename(root / "save.srm")
        original = (root / "save.srm").read_bytes()
        (Path(directory) / "settings.ini").write_text("save_backend = ini\n")
        result = subprocess.run([game, rom], cwd=directory, env=env,
                                capture_output=True, text=True, timeout=45)
        output = result.stdout + result.stderr
        assert result.returncode == 0 and "opened save setup" not in output, output[-12000:]
        assert (root / "slots/01/save.srm").read_bytes() == original
        assert (root / "legacy-layout/save.srm").read_bytes() == original
        assert not (root / "slots/01/save.ini").exists()
        print("Legacy save.srm: automatically imported despite the INI preference; original retained.")
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

#!/usr/bin/env python3
"""Prepare native AR2 generated sources from the player's validated content pack."""
import argparse
import hashlib
import json
from pathlib import Path
import os
import struct
import subprocess
from ar2_metadata import ROM_SHA256, ROM_BYTES
import ar2_coverage


def generation_inputs(root, pack):
    return sorted({pack, Path(__file__).resolve(), root / "tools/ar2_metadata.py",
                   root / "tools/ar2_coverage.py", root / "recomp/ar2/coverage.json",
                   *(root / "recomp/ar2").glob("*.cfg"),
                   *(root / "snesrecomp-go").glob("go.*"),
                   *(path for directory in ("cmd/v2regen", "internal")
                     for path in (root / "snesrecomp-go" / directory).rglob("*.go")
                     if not path.name.endswith("_test.go"))})


def prepare(root, pack, output, go, force=False):
    inputs = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in generation_inputs(root, pack)}
    stamp = output / "prepared.json"
    if not force and stamp.exists():
        try:
            previous = json.loads(stamp.read_text())
        except (ValueError, OSError):
            previous = {}
        if (previous.get("inputs") == inputs and previous.get("outputs") and
                all((output / name).is_file() for name in previous["outputs"])):
            print("AR2 generated sources are current")
            stamp.touch()  # Acknowledge timestamp-only dependency changes too.
            return
    data = pack.read_bytes()
    digest = bytes.fromhex(ROM_SHA256)
    if (len(data) != 48 + ROM_BYTES or data[:8] != b"AR2WORLD"
            or struct.unpack_from("<II", data, 8) != (1, ROM_BYTES)
            or data[16:48] != digest or hashlib.sha256(data[48:]).digest() != digest):
        raise ValueError("invalid or unsupported AR2 world pack; import the USA ROM again")
    output.mkdir(parents=True, exist_ok=True)
    coverage = ar2_coverage.load(root / "recomp/ar2/coverage.json")
    ar2_coverage.validate(coverage, data[48:])
    ar2_coverage.sync(coverage, root / "recomp/ar2", check=True)
    stamp.unlink(missing_ok=True)  # A failed generation cannot leave a valid cache claim.
    rom = output / "ar2.sfc"
    if not rom.exists() or rom.read_bytes() != data[48:]:
        rom.write_bytes(data[48:])
    env = dict(os.environ, GOCACHE=str(output / "go-cache"))
    tool = output / ("v2regen.exe" if os.name == "nt" else "v2regen")
    subprocess.run([go, "build", "-o", str(tool), "./cmd/v2regen"],
                   cwd=root / "snesrecomp-go", env=env, check=True)
    subprocess.run([str(tool), "regen", "--rom", str(rom), "--cfg-dir", str(root / "recomp/ar2"),
                    "--out-dir", str(output / "generated"), "--experimental-proven-analysis",
                    "--experimental-stored-targets", "--experimental-internal-tails", "--allow-stubs"],
                   cwd=output, check=True)
    outputs = ["ar2.sfc", *(str(path.relative_to(output))
                            for path in sorted((output / "generated").glob("*.c")))]
    if len(outputs) == 1:
        raise ValueError("AR2 generator produced no sources")
    temporary = stamp.with_suffix(".tmp")
    temporary.write_text(json.dumps({"inputs": inputs, "outputs": outputs}, indent=2) + "\n")
    temporary.replace(stamp)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--go", default="go")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        prepare(root, args.pack.resolve(), args.output.resolve(), args.go, args.force)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()

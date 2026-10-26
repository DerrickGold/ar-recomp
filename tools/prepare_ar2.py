#!/usr/bin/env python3
"""Prepare native AR2 generated sources from the player's validated content pack."""
import argparse
import hashlib
from pathlib import Path
import os
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    data = args.pack.read_bytes()
    digest = bytes.fromhex("71bdd448a30b88725864e55594ebb67a118b1f197a3f9e5dd39dbf23399efa15")
    if (len(data) != 48 + 0x180000 or data[:8] != b"AR2WORLD"
            or struct.unpack_from("<II", data, 8) != (1, 0x180000)
            or data[16:48] != digest or hashlib.sha256(data[48:]).digest() != digest):
        parser.error("invalid or unsupported AR2 world pack; import the USA ROM again")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    rom = output / "ar2.sfc"
    rom.write_bytes(data[48:])
    env = dict(os.environ, GOCACHE=str(output / "go-cache"))
    tool = output / ("v2regen.exe" if os.name == "nt" else "v2regen")
    subprocess.run(["go", "build", "-o", str(tool), "./cmd/v2regen"],
                   cwd=root / "snesrecomp-go", env=env, check=True)
    subprocess.run([str(tool), "regen", "--rom", str(rom), "--cfg-dir", str(root / "recomp/ar2"),
                    "--out-dir", str(output / "generated"), "--experimental-proven-analysis",
                    "--experimental-stored-targets", "--experimental-internal-tails", "--allow-stubs"],
                   cwd=output, check=True)


if __name__ == "__main__":
    main()

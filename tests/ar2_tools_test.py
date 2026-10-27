#!/usr/bin/env python3
"""ROM-free checks of incremental generation and reviewed coverage ownership."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ar2_coverage as coverage
import ar2_metadata as metadata
import prepare_ar2 as prepare


class CoverageTests(unittest.TestCase):
    def test_record_requires_matching_diagnostics_and_audited_operands(self):
        rom = bytearray(coverage.ROM_BYTES)
        rom[0x2d6ea:0x2d6ee] = bytes((2, 0x80, 1, 0xea))
        digest = hashlib.sha256(rom).hexdigest()
        with tempfile.TemporaryDirectory() as temporary, patch.object(coverage, "ROM_SHA256", digest):
            folder = Path(temporary)
            (folder / "stderr.log").write_text("Uncovered AR2 code 809605 -> 82d6ea (M0 X0)\n")
            # Native evidence is valid even before a scene has a capture profile.
            cpu = {"failed": True, "m": 0, "x_flag": 0, "scene": 0x04}
            (folder / "exit.cpu.json").write_text(json.dumps(cpu))
            report = {"rom_sha256": digest, "cases": [{"artifacts": temporary,
                      "missing_dispatch": ["809605", "82d6ea", "0", "0"]}]}
            manifest = {"version": 1, "rom_sha256": digest, "entries": []}
            recorded = coverage.record(manifest, report, rom, expand=True)
            self.assertEqual([e["target"] for e in recorded["entries"]], ["82d6ea", "82d6ed"])
            self.assertEqual(manifest["entries"], [])
            bad = copy.deepcopy(recorded)
            bad["entries"][1]["target"] = "82d6ec"  # Inline operand, not the return PC.
            with self.assertRaises(ValueError): coverage.validate(bad, rom)
            cpu["x_flag"] = 1
            (folder / "exit.cpu.json").write_text(json.dumps(cpu))
            with self.assertRaises(ValueError): coverage.record(manifest, report, rom)

    def test_checked_catalog_and_cfg(self):
        manifest = coverage.load(coverage.DEFAULT_MANIFEST)
        coverage.sync(manifest, metadata.ROOT / "recomp/ar2", check=True)
        profiles = metadata.scene_profiles()
        self.assertEqual([scene for scene, (entrance, public) in profiles.items()
                          if scene == entrance and public], [0x14, 0x16])
        self.assertEqual(profiles[0x34], (0x3a, False))

    def test_evidence_validation_and_atomic_cfg_update(self):
        for pc in ("008000", "808000", "c08000"):
            self.assertEqual(coverage.address(pc), int(pc, 16))
        manifest = coverage.load(coverage.DEFAULT_MANIFEST)
        first = next(e for e in manifest["entries"] if e["kind"] == "observed")
        manifest["entries"] = [copy.deepcopy(first)]
        for field, value in (("m", 2), ("target", "7e8000"), ("evidence_sha256", "")):
            bad = copy.deepcopy(manifest)
            bad["entries"][0][field] = value
            with self.assertRaises(ValueError): coverage.validate(bad)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            bank = first["target"][:2]
            cfg = directory / f"bank{bank}.cfg"
            cfg.write_text(f"bank = {bank}\n# Authored annotations stay here.\n")
            coverage.sync(manifest, directory)
            expected = cfg.read_bytes()
            coverage.sync(manifest, directory, check=True)
            self.assertEqual(cfg.read_bytes(), expected)
            cfg.write_text(cfg.read_text().replace("entry_mx:0,0", "entry_mx:1,1"))
            with self.assertRaises(ValueError): coverage.sync(manifest, directory, check=True)
            coverage.sync(manifest, directory)
            self.assertEqual(cfg.read_bytes(), expected)
            bad = copy.deepcopy(manifest)
            bad["entries"][0]["target"] = "978000"
            with self.assertRaises(ValueError): coverage.sync(bad, directory)
            self.assertEqual(cfg.read_bytes(), expected)


class PreparationTests(unittest.TestCase):
    def test_generator_changes_and_missing_outputs_invalidate_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("tools/ar2_metadata.py", "tools/ar2_coverage.py", "snesrecomp-go/go.mod",
                         "snesrecomp-go/cmd/v2regen/main.go", "snesrecomp-go/internal/emitter/emit.go",
                         "recomp/ar2/bank80.cfg"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("initial")
            payload = bytes(32)
            digest = hashlib.sha256(payload).digest()
            pack = root / "world.argame"
            pack.write_bytes(b"AR2WORLD" + struct.pack("<II", 1, len(payload)) + digest + payload)
            output = root / "build"
            calls = []

            def run(command, **kwargs):
                calls.append(command)
                if command[1] == "regen":
                    (output / "generated").mkdir(exist_ok=True)
                    (output / "generated/bank80_v2.c").write_text("generated")

            manifest = root / "recomp/ar2/coverage.json"
            manifest.write_text(json.dumps({"version": 1, "rom_sha256": digest.hex(), "entries": []}))
            with patch.object(prepare, "ROM_BYTES", len(payload)), \
                 patch.object(prepare, "ROM_SHA256", digest.hex()), \
                 patch.object(coverage, "ROM_BYTES", len(payload)), \
                 patch.object(coverage, "ROM_SHA256", digest.hex()), \
                 patch.object(prepare.subprocess, "run", run):
                prepare.prepare(root, pack, output, "go")
                self.assertEqual(len(calls), 2)
                prepare.prepare(root, pack, output, "go")
                self.assertEqual(len(calls), 2)
                (root / "snesrecomp-go/internal/emitter/emit.go").write_text("fixed generator")
                prepare.prepare(root, pack, output, "go")
                self.assertEqual(len(calls), 4)
                (output / "generated/bank80_v2.c").unlink()
                prepare.prepare(root, pack, output, "go")
                self.assertEqual(len(calls), 6)
                pack.write_bytes(b"corrupt")
                with self.assertRaises(ValueError): prepare.prepare(root, pack, output, "go")


if __name__ == "__main__":
    unittest.main()

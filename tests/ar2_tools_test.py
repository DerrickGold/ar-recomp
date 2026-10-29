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
        self.assertEqual([scene for scene, entrance in profiles.items() if scene == entrance],
                         [0x14, 0x16, 0x18, 0x1a, 0x1e, 0x21, 0x28, 0x2b, 0x2f,
                          0x3a, 0x3d, 0x41, 0x49, 0x51])
        self.assertEqual(profiles[0x34], 0x3a)

    def test_clear_task_operands_do_not_become_code(self):
        rom = bytearray(coverage.ROM_BYTES)
        # Native-shaped task call, delay, loop setup, loop exit, sound and
        # departure. Each inline operand must be skipped, even when it is $02.
        code = bytes.fromhex('02 00 55 9b 82 02 25 02 00 02 03 02 00 02 04 02 17 02 02 26 '
                             '02 86 02 02 02 02 8c 02 02 94 02 a9 02 02 02 b0 02 c9 02 a2 6b')
        rom[0x2efa2:0x2efa2 + len(code)] = code
        digest = hashlib.sha256(rom).hexdigest()
        with tempfile.TemporaryDirectory() as temporary, patch.object(coverage, 'ROM_SHA256', digest):
            folder = Path(temporary)
            (folder / 'stderr.log').write_text('Uncovered AR2 code 809605 -> 82efa2 (M0 X0)\n')
            (folder / 'exit.cpu.json').write_text(json.dumps(
                {'failed': True, 'm': 0, 'x_flag': 0, 'scene': 0x15}))
            report = {'rom_sha256': digest, 'cases': [{'artifacts': temporary,
                      'missing_dispatch': ['809605', '82efa2', '0', '0']}]}
            result = coverage.record({'version': 1, 'rom_sha256': digest, 'entries': []},
                                     report, rom, expand=True)
            targets = {e['target'] for e in result['entries']}
            self.assertEqual(targets, {'82efa2', '829b55', '82efa7', '82efab',
                                       '82efaf', '82efb1', '82efb4', '82efb6', '82efbb',
                                       '82efbe', '82efc0', '82efc4', '82efc6', '82efc8', '82efca'})
            bad = copy.deepcopy(result)
            next(e for e in bad['entries'] if e['kind'] == 'cop_task')['target'] = '829b56'
            with self.assertRaises(ValueError): coverage.validate(bad, rom)

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
            mode = f"entry_mx:{first['m']},{first['x']}"
            cfg.write_text(cfg.read_text().replace(mode, f"entry_mx:{1-first['m']},{first['x']}"))
            with self.assertRaises(ValueError): coverage.sync(manifest, directory, check=True)
            coverage.sync(manifest, directory)
            self.assertEqual(cfg.read_bytes(), expected)
            bad = copy.deepcopy(manifest)
            bad["entries"][0]["target"] = "978000"
            with self.assertRaises(ValueError): coverage.sync(bad, directory)
            self.assertEqual(cfg.read_bytes(), expected)

    def test_observed_width_variant_can_share_an_authored_address(self):
        manifest = coverage.load(coverage.DEFAULT_MANIFEST)
        observed = copy.deepcopy(next(e for e in manifest['entries'] if e['kind'] == 'observed'))
        observed.update(target='8082c3', m=1, x=0)
        manifest['entries'] = [observed]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cfg = directory / 'bank80.cfg'
            authored = 'bank = 80\nfunc bank_80_82C3 82C3 entry_mx:0,0\n'
            cfg.write_text(authored)
            coverage.sync(manifest, directory)
            expected = cfg.read_bytes()
            self.assertTrue(cfg.read_text().startswith(authored))
            self.assertIn('func bank_80_82C3 82C3 entry_mx:1,0', cfg.read_text())
            observed['m'] = 0
            with self.assertRaises(ValueError): coverage.sync(manifest, directory)
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

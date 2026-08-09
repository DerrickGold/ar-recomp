#!/usr/bin/env python3
"""ROM-free contract tests for tools/bg_hle_census.py."""

import json
import os
import struct
import sys
import tempfile
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bg_hle_census as census  # noqa: E402
import bg_hle_matrix as matrix  # noqa: E402


def write_u16(data, address, value):
    data[address] = value & 0xFF
    data[address + 1] = value >> 8


class CensusFixture:
    def __init__(self, directory):
        self.prefix = os.path.join(directory, "snap_gf123")
        self.wram = bytearray(census.WRAM_BYTES)
        self.vram = [0] * census.VRAM_WORDS
        self.wram[0x18] = 1
        self.wram[0x19] = 2
        write_u16(self.wram, 0x88, 123)
        self._build_layer(0, 0x2000, 0x1000, 0x6000)
        self._build_layer(1, 0x2100, 0x1800, 0x7000)

    def _build_layer(self, layer, map_start, table_start, tilemap_base):
        offset = layer * census.LAYER_STRIDE
        write_u16(self.wram, 0x22 + offset, 13 + layer * 8)
        write_u16(self.wram, 0x24 + offset, 7 + layer * 8)
        write_u16(self.wram, 0x2E + offset, 256)
        write_u16(self.wram, 0x30 + offset, 256)
        write_u16(self.wram, 0x46 + offset, map_start)
        write_u16(self.wram, 0x48 + offset, tilemap_base)
        write_u16(self.wram, 0x52 + offset, table_start)
        write_u16(self.wram, 0x54 + offset, 0xFFFF)
        self.wram[0x6B + offset] = layer * 0x20
        for index in range(256):
            self.wram[map_start + index] = index
            for quadrant in range(4):
                word = ((index * 4 + quadrant) & 0x3FF) | (layer * 0x2000)
                write_u16(self.wram, table_start + index * 8 + quadrant * 2,
                          word)
        layout = census.decode_layout(self.wram, layer)
        for tile_y in range(32):
            for tile_x in range(32):
                address = census.ring_address(tilemap_base, tile_x, tile_y)
                self.vram[address] = census.decoded_word(
                    self.wram, layout, tile_x, tile_y)

    def write(self, include_ppu=True):
        with open(self.prefix + ".wram.bin", "wb") as output:
            output.write(self.wram)
        with open(self.prefix + ".vram.bin", "wb") as output:
            output.write(struct.pack("<%dH" % len(self.vram), *self.vram))
        if include_ppu:
            with open(self.prefix + ".ppu.json", "w", encoding="utf-8") as output:
                json.dump({
                    "format": 1,
                    "bgmode": 1,
                    "inidisp": 15,
                    "bg_sc": [0x63, 0x73, 0x58, 0x00],
                    "bg_tile_adr": 0,
                    "screen_main": 3,
                    "screen_sub": 0,
                }, output)


class BgHleCensusTest(unittest.TestCase):
    def test_matching_snapshot_and_ppu_eligibility(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = CensusFixture(directory)
            fixture.write()
            record = census.analyze_snapshot(fixture.prefix, "rom-hash")
            self.assertEqual(record["map_group"], 1)
            self.assertEqual(record["map_number"], 2)
            self.assertEqual(record["rom_sha256"], "rom-hash")
            self.assertTrue(record["ppu_metadata"])
            for layer in record["layers"]:
                self.assertTrue(layer["valid"])
                self.assertTrue(layer["ppu"]["eligible"])
                self.assertEqual(layer["comparison"]["mismatches"], 0)
                self.assertGreater(layer["comparison"]["compared"], 0)
            self.assertFalse(census.snapshot_failed(record, True))
            summary = census.build_summary([record])
            self.assertEqual(summary["action_maps"], 1)
            self.assertEqual(summary["groups"]["01/02/BG1"]["map_variants"], 1)
            self.assertEqual(summary["groups"]["01/02/BG1"]["mismatches"], 0)

    def test_matrix_parsers_pin_verified_targets_and_summary(self):
        self.assertEqual(matrix.parse_targets("0201,0202,0201"),
                         ["0201", "0202"])
        self.assertEqual(matrix.parse_targets("0701,0708"), ["0701", "0708"])
        self.assertEqual(matrix.parse_targets_as_frames("900,0x4b0"),
                         [900, 1200])
        log = (
            "[run-dir] runs/20260809-123456 (console.log)\n"
            "[action-bg-hle] summary frames=10 activations=2 layers=8 "
            "tiles=7000 mismatches=0 outside=0 "
            "fallbacks={blank:4,mode:0,disabled:0,native:3,invalid:0,"
            "alloc:0,compare:0}\n")
        self.assertEqual(matrix.parse_run_directory(log),
                         "runs/20260809-123456")
        summary = matrix.parse_comparator_summary(log)
        self.assertEqual(summary["tiles"], 7000)
        self.assertEqual(summary["native"], 3)
        self.assertEqual(summary["phase"], 0)
        self.assertEqual(summary["edge"], 0)
        log = log.replace("alloc:0,compare:0", "alloc:0,phase:2,edge:1,compare:0")
        summary = matrix.parse_comparator_summary(log)
        self.assertEqual(summary["phase"], 2)
        self.assertEqual(summary["edge"], 1)
        provider_log = (
            "[action-bg-hle] provider-summary frames=10 "
            "preflight={layers:8,tiles:7000,mismatches:0,outside:0} "
            "eligible=8 layers=8 lookups=9000 tiles=8900 outside=100\n")
        provider = matrix.parse_provider_summary(provider_log)
        self.assertEqual(provider["preflight_tiles"], 7000)
        self.assertEqual(provider["layers"], 8)
        self.assertEqual(provider["outside"], 100)
        matrix.validate_provider_summary(provider, True)
        bad_provider = dict(provider, preflight_mismatches=1)
        with self.assertRaises(matrix.MatrixError):
            matrix.validate_provider_summary(bad_provider, True)
        with self.assertRaises(matrix.MatrixError):
            matrix.validate_provider_summary(None, True)
        with self.assertRaises(matrix.MatrixError):
            matrix.validate_provider_summary(provider, False)

    def test_matrix_inspects_framebuffer_header_and_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "shot.ppm")
            with open(path, "wb") as output:
                output.write(b"P6\n2 1\n255\n\x00\x01\x02\x03\x04\x05")
            metadata = matrix.inspect_ppm(path)
            self.assertEqual((metadata["width"], metadata["height"]), (2, 1))
            self.assertEqual(len(metadata["sha256"]), 64)

    def test_matrix_error_retains_failed_run_artifact_path(self):
        error = matrix.MatrixError("wrong map", "/tmp/run-1")
        self.assertEqual(str(error), "wrong map")
        self.assertEqual(error.run_directory, "/tmp/run-1")

    def test_positive_mismatch_and_missing_ppu_are_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = CensusFixture(directory)
            layout = census.decode_layout(fixture.wram, 0)
            address = census.ring_address(
                layout["tilemap_base"], layout["camera"][0] // 8,
                layout["camera"][1] // 8)
            fixture.vram[address] ^= 1
            fixture.write(include_ppu=False)
            record = census.analyze_snapshot(fixture.prefix)
            self.assertFalse(record["ppu_metadata"])
            self.assertIsNone(record["layers"][0]["ppu"]["eligible"])
            self.assertEqual(
                record["layers"][0]["comparison"]["mismatches"], 1)
            self.assertTrue(census.snapshot_failed(record, False))

    def test_discovery_accepts_prefix_file_and_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = CensusFixture(directory)
            fixture.write()
            expected = [fixture.prefix]
            self.assertEqual(census.discover_prefixes([fixture.prefix]), expected)
            self.assertEqual(census.discover_prefixes(
                [fixture.prefix + ".wram.bin"]), expected)
            self.assertEqual(census.discover_prefixes([directory]), expected)

    def test_ineligible_native_layer_does_not_make_strict_ring_claim(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = CensusFixture(directory)
            layout = census.decode_layout(fixture.wram, 0)
            address = census.ring_address(
                layout["tilemap_base"], layout["camera"][0] // 8,
                layout["camera"][1] // 8)
            fixture.vram[address] ^= 1
            fixture.write()
            with open(fixture.prefix + ".ppu.json", "r", encoding="utf-8") as source:
                ppu = json.load(source)
            ppu["screen_main"] = 2
            with open(fixture.prefix + ".ppu.json", "w", encoding="utf-8") as output:
                json.dump(ppu, output)
            record = census.analyze_snapshot(fixture.prefix)
            self.assertFalse(record["layers"][0]["ppu"]["eligible"])
            self.assertEqual(record["layers"][0]["comparison"]["mismatches"], 1)
            self.assertFalse(census.snapshot_failed(record, True))


if __name__ == "__main__":
    unittest.main()

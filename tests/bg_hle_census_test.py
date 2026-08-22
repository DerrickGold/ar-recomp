#!/usr/bin/env python3
"""ROM-free contract tests for tools/bg_hle_census.py."""

import argparse
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
import bg_hle_artifact_compare as artifact_compare  # noqa: E402


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
    @staticmethod
    def _write_artifact_run(directory, payload):
        os.makedirs(os.path.join(directory, "snapshots"))
        for name in artifact_compare.FINAL_ARTIFACTS:
            with open(os.path.join(directory, name), "wb") as output:
                output.write(payload + name.encode("ascii"))
        for suffix in artifact_compare.SNAPSHOT_SUFFIXES:
            with open(os.path.join(directory, "snapshots", "vd_gf10" + suffix),
                      "wb") as output:
                output.write(payload + suffix.encode("ascii"))

    @staticmethod
    def _write_artifact_manifest(path, run_directory, **overrides):
        manifest = {
            "format": 1,
            "rom_sha256": "0" * 64,
            "replay": "/fixtures/action.rec",
            "warp_frame": 4,
            "capture_frames": [10],
            "quit_frames": 20,
            "settings_fixture": {
                "display_mode": "43",
                "diorama": False,
                "diorama_vertical_extend": 0,
            },
            "provider_enabled": True,
            "provider_binding_expected": True,
            "provider_setting": "default",
            "results": [{
                "target": "0101",
                "status": "pass",
                "run_directory": run_directory,
            }],
        }
        manifest.update(overrides)
        with open(path, "w", encoding="utf-8") as output:
            json.dump(manifest, output)

    @staticmethod
    def _write_ppm(path, width, height, pixels):
        with open(path, "wb") as output:
            output.write(b"P6\n%d %d\n255\n" % (width, height))
            output.write(pixels)

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
                first_x, last_x, first_y, last_y = (
                    census.authentic_tile_bounds(*layer["camera"]))
                sampled = ((last_x - first_x + 1) *
                           (last_y - first_y + 1))
                self.assertEqual(
                    layer["comparison"]["compared"] +
                    layer["comparison"]["outside_world"], sampled)
            self.assertFalse(census.snapshot_failed(record, True))
            summary = census.build_summary([record])
            self.assertEqual(summary["action_maps"], 1)
            self.assertEqual(summary["groups"]["01/02/BG1"]["map_variants"], 1)
            self.assertEqual(summary["groups"]["01/02/BG1"]["mismatches"], 0)

    def test_marahna_bg2_uses_decoded_world_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = CensusFixture(directory)
            fixture.wram[0x18] = 5
            fixture.wram[0x19] = 2
            write_u16(fixture.wram, 0x22, 503)
            write_u16(fixture.wram, 0x2E, 768)
            write_u16(fixture.wram, 0x26, 503)
            write_u16(fixture.wram, 0x32, 512)
            layout = census.decode_layout(fixture.wram, 1)
            for tile_y in range(32):
                for tile_x in range(64):
                    address = census.ring_address(
                        layout["tilemap_base"], tile_x, tile_y)
                    fixture.vram[address] = census.decoded_word(
                        fixture.wram, layout, tile_x, tile_y)
            fixture.write()

            record = census.analyze_snapshot(fixture.prefix)
            bg2 = record["layers"][1]
            self.assertTrue(bg2["wrap_world_x"])
            self.assertEqual(bg2["comparison"]["compared"], 33 * 28)
            self.assertEqual(bg2["comparison"]["mismatches"], 0)
            self.assertEqual(bg2["comparison"]["outside_world"], 0)

    def test_authentic_tile_bounds_pin_all_vertical_scroll_phases(self):
        expected_first_rows = [10, 10, 10, 10, 10, 10, 10, 11]
        expected_row_counts = [29, 29, 29, 29, 29, 29, 29, 28]
        for phase in range(8):
            with self.subTest(phase=phase):
                first_x, last_x, first_y, last_y = (
                    census.authentic_tile_bounds(13, 80 + phase))
                self.assertEqual((first_x, last_x), (1, 33))
                self.assertEqual(first_y, expected_first_rows[phase])
                self.assertEqual(last_y, 38)
                self.assertEqual(last_y - first_y + 1,
                                 expected_row_counts[phase])

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

        room_log = (
            "[action-room-stage] summary layers=2 bytes=8200 "
            "mismatches=0\n")
        room = matrix.parse_room_stage_comparator_summary(room_log)
        self.assertEqual(room["layers"], 2)
        self.assertEqual(room["bytes"], 8200)
        self.assertEqual(room["mismatches"], 0)
        loader_log = (
            "[action-room-load-hle] summary command5=2 command4=2 "
            "bytes=8192\n")
        loader = matrix.parse_room_load_hle_summary(loader_log)
        self.assertEqual(loader["command5"], 2)
        self.assertEqual(loader["command4"], 2)
        self.assertEqual(loader["bytes"], 8192)
        graphics_log = (
            "[action-room-gfx-hle] summary command7=4 command6=3 "
            "bytes=33152\n")
        graphics = matrix.parse_room_graphics_hle_summary(graphics_log)
        self.assertEqual(graphics["command7"], 4)
        self.assertEqual(graphics["command6"], 3)
        self.assertEqual(graphics["bytes"], 33152)
        video_log = (
            "[action-room-video-hle] summary command3=2 bytes=56\n")
        video = matrix.parse_room_video_hle_summary(video_log)
        self.assertEqual(video["command3"], 2)
        self.assertEqual(video["bytes"], 56)

        default_args = matrix.parse_args([])
        self.assertEqual(default_args.provider_mode, "default")
        self.assertEqual(default_args.room_graphics_mode, "default")
        self.assertEqual(default_args.room_video_mode, "default")
        self.assertTrue(matrix.room_graphics_setting_enabled(default_args))
        self.assertTrue(matrix.room_video_setting_enabled(default_args))
        self.assertTrue(matrix.provider_setting_enabled(default_args))
        self.assertTrue(matrix.provider_binding_expected(default_args))
        off_args = matrix.parse_args(["--disable-provider"])
        self.assertEqual(off_args.provider_mode, "disabled")
        self.assertFalse(matrix.provider_setting_enabled(off_args))
        self.assertFalse(matrix.provider_binding_expected(off_args))
        on_args = matrix.parse_args(["--enable-provider"])
        self.assertEqual(on_args.provider_mode, "enabled")
        self.assertTrue(matrix.provider_setting_enabled(on_args))
        self.assertTrue(matrix.provider_binding_expected(on_args))
        graphics_off_args = matrix.parse_args([
            "--disable-room-graphics-hle"])
        self.assertFalse(matrix.room_graphics_setting_enabled(
            graphics_off_args))
        video_off_args = matrix.parse_args([
            "--disable-room-video-hle"])
        self.assertFalse(matrix.room_video_setting_enabled(video_off_args))

        raw_args = matrix.parse_args(["--display-mode", "raw"])
        self.assertTrue(matrix.provider_setting_enabled(raw_args))
        self.assertFalse(matrix.provider_binding_expected(raw_args))
        full_args = matrix.parse_args([
            "--display-mode", "full", "--diorama",
            "--vertical-extend", "32",
        ])
        self.assertTrue(matrix.provider_binding_expected(full_args))
        fixture = matrix.settings_fixture(full_args)
        self.assertIn("display_mode = Widescreen full\n", fixture)
        self.assertIn("extended_aspect = 16:10\n", fixture)
        self.assertIn("diorama_mode = On\n", fixture)
        self.assertIn("diorama_vertical_extend = 32\n", fixture)
        with self.assertRaises(argparse.ArgumentTypeError):
            matrix.parse_vertical_extend("33")

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

    def test_matrix_artifact_compare_pins_complete_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            left_run = os.path.join(directory, "left")
            right_run = os.path.join(directory, "right")
            self._write_artifact_run(left_run, b"same-")
            self._write_artifact_run(right_run, b"same-")
            left_manifest = os.path.join(directory, "left.json")
            right_manifest = os.path.join(directory, "right.json")
            self._write_artifact_manifest(left_manifest, left_run)
            self._write_artifact_manifest(right_manifest, right_run)
            result = artifact_compare.compare_manifests(
                left_manifest, right_manifest)
            self.assertEqual(result["targets"], 1)
            self.assertEqual(result["artifacts"], 11)
            self.assertEqual(result["mismatches"], [])

            with open(os.path.join(right_run, "shot.ppm"), "wb") as output:
                output.write(b"different")
            result = artifact_compare.compare_manifests(
                left_manifest, right_manifest)
            self.assertEqual(len(result["mismatches"]), 1)
            self.assertEqual(result["mismatches"][0]["artifact"], "shot.ppm")

    def test_artifact_compare_rejects_capture_provenance_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            left_run = os.path.join(directory, "left")
            right_run = os.path.join(directory, "right")
            self._write_artifact_run(left_run, b"same-")
            self._write_artifact_run(right_run, b"same-")
            left_manifest = os.path.join(directory, "left.json")
            right_manifest = os.path.join(directory, "right.json")
            self._write_artifact_manifest(left_manifest, left_run)
            drift_cases = {
                "rom_sha256": "1" * 64,
                "replay": "/fixtures/other.rec",
                "warp_frame": 5,
                "capture_frames": [11],
                "quit_frames": 21,
                "settings_fixture": {
                    "display_mode": "full",
                    "diorama": False,
                    "diorama_vertical_extend": 0,
                },
            }
            for field, value in drift_cases.items():
                with self.subTest(field=field):
                    self._write_artifact_manifest(
                        right_manifest, right_run, **{field: value})
                    with self.assertRaisesRegex(
                            artifact_compare.ArtifactCompareError, field):
                        artifact_compare.compare_manifests(
                            left_manifest, right_manifest)

            # Provider mode and binary are intentional A/B dimensions rather
            # than capture provenance; differing values remain comparable.
            self._write_artifact_manifest(
                right_manifest, right_run,
                provider_enabled=False,
                provider_binding_expected=False,
                provider_setting="disabled",
                binary="/tmp/other-build/ActRaiserRecomp")
            result = artifact_compare.compare_manifests(
                left_manifest, right_manifest)
            self.assertEqual(result["mismatches"], [])

    def test_artifact_compare_requires_every_component_per_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            left_run = os.path.join(directory, "left")
            right_run = os.path.join(directory, "right")
            self._write_artifact_run(left_run, b"same-")
            self._write_artifact_run(right_run, b"same-")
            left_manifest = os.path.join(directory, "left.json")
            right_manifest = os.path.join(directory, "right.json")
            self._write_artifact_manifest(left_manifest, left_run)
            self._write_artifact_manifest(right_manifest, right_run)

            # Keep the old total component count while moving one component to
            # a different prefix. Count-only validation accepted this shape.
            os.remove(os.path.join(
                right_run, "snapshots", "vd_gf10.ppu.json"))
            with open(os.path.join(
                    right_run, "snapshots", "vd_gf11.ppu.json"), "wb") as output:
                output.write(b"extra")
            with self.assertRaisesRegex(
                    artifact_compare.ArtifactCompareError,
                    "snapshot prefixes differ"):
                artifact_compare.compare_manifests(
                    left_manifest, right_manifest)

    def test_artifact_compare_can_pin_authentic_center_and_report_margins(self):
        with tempfile.TemporaryDirectory() as directory:
            left_run = os.path.join(directory, "left")
            right_run = os.path.join(directory, "right")
            self._write_artifact_run(left_run, b"same-")
            self._write_artifact_run(right_run, b"same-")
            left_manifest = os.path.join(directory, "left.json")
            right_manifest = os.path.join(directory, "right.json")
            self._write_artifact_manifest(left_manifest, left_run)
            self._write_artifact_manifest(right_manifest, right_run)
            width, height = 258, 2
            left = bytearray(width * height * 3)
            right = bytearray(left)
            right[0:3] = b"\x01\x02\x03"
            self._write_ppm(os.path.join(left_run, "shot.ppm"),
                            width, height, left)
            self._write_ppm(os.path.join(right_run, "shot.ppm"),
                            width, height, right)
            result = artifact_compare.compare_manifests(
                left_manifest, right_manifest, "authentic-center")
            self.assertEqual(result["mismatches"], [])
            self.assertEqual(
                result["framebuffer_differences"][0]["left_margin_pixels"], 1)
            self.assertEqual(
                result["framebuffer_differences"][0]["center_pixels"], 0)

            center_offset = 3
            right[center_offset:center_offset + 3] = b"\x04\x05\x06"
            self._write_ppm(os.path.join(right_run, "shot.ppm"),
                            width, height, right)
            result = artifact_compare.compare_manifests(
                left_manifest, right_manifest, "authentic-center")
            self.assertEqual(len(result["mismatches"]), 1)
            self.assertEqual(
                result["mismatches"][0]["framebuffer"]["center_pixels"], 1)

    def test_artifact_compare_accepts_only_censused_provider_vram(self):
        with tempfile.TemporaryDirectory() as directory:
            left_run = os.path.join(directory, "left")
            right_run = os.path.join(directory, "right")
            self._write_artifact_run(left_run, b"same-")
            self._write_artifact_run(right_run, b"same-")
            left_manifest = os.path.join(directory, "left.json")
            right_manifest = os.path.join(directory, "right.json")
            self._write_artifact_manifest(left_manifest, left_run)
            self._write_artifact_manifest(right_manifest, right_run)
            left_vram = [0] * artifact_compare.VRAM_WORDS
            right_vram = list(left_vram)
            right_vram[0x6001] = 1
            for run, words in ((left_run, left_vram),
                               (right_run, right_vram)):
                with open(os.path.join(
                        run, "snapshots", "vd_gf10.vram.bin"), "wb") as output:
                    output.write(struct.pack("<%dH" % len(words), *words))
            for manifest_path, run in ((left_manifest, left_run),
                                       (right_manifest, right_run)):
                with open(manifest_path, "r", encoding="utf-8") as source:
                    manifest = json.load(source)
                manifest["results"][0]["snapshots"] = [{
                    "snapshot": os.path.join(run, "snapshots", "vd_gf10"),
                    "layers": [{
                        "tilemap_base": 0x6000,
                        "ppu": {"eligible": True},
                        "comparison": {
                            "available": True,
                            "mismatches": 0,
                            "outside_world": 0,
                        },
                    }],
                }]
                with open(manifest_path, "w", encoding="utf-8") as output:
                    json.dump(manifest, output)

            result = artifact_compare.compare_manifests(
                left_manifest, right_manifest, snapshot_vram_policy="provider-owned")
            self.assertEqual(result["mismatches"], [])
            self.assertEqual(result["vram_differences"][0]["changed_words"], 1)

            right_vram[0x5000] = 1
            with open(os.path.join(
                    right_run, "snapshots", "vd_gf10.vram.bin"), "wb") as output:
                output.write(struct.pack("<%dH" % len(right_vram), *right_vram))
            result = artifact_compare.compare_manifests(
                left_manifest, right_manifest, snapshot_vram_policy="provider-owned")
            self.assertEqual(len(result["mismatches"]), 1)

    def test_positive_mismatch_and_missing_ppu_are_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = CensusFixture(directory)
            layout = census.decode_layout(fixture.wram, 0)
            first_x, _, first_y, _ = census.authentic_tile_bounds(
                *layout["camera"])
            address = census.ring_address(
                layout["tilemap_base"], first_x, first_y)
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
            first_x, _, first_y, _ = census.authentic_tile_bounds(
                *layout["camera"])
            address = census.ring_address(
                layout["tilemap_base"], first_x, first_y)
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

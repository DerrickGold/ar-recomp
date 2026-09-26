#!/usr/bin/env python3
"""ROM-free regressions for developer-tool discovery and magic table decoding."""

from contextlib import redirect_stderr
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import action_magic_catalog
import check_tooling


class ToolingQualityTest(unittest.TestCase):
    def test_discovery_covers_non_game_code_without_vendor_or_deleted_files(self):
        names = ["installer/web/app.js", "tools/debug.py", "benchmarks/run.sh",
                 "installer/desktop-shell/main.go", "third_party/helper.py",
                 "snesrecomp-go/vendor/helper.go", "src/generated/script.py",
                 "tools/deleted.py"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in names[:-1]:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("")
            listing = ("\0".join(names + [names[0]]) + "\0").encode()
            with patch.object(check_tooling.subprocess, "check_output", return_value=listing):
                self.assertEqual(check_tooling.source_files(root),
                                 sorted(Path(name) for name in names[:4]))

    def test_shell_syntax_uses_declared_interpreter(self):
        self.assertEqual(check_tooling.shell_for("#!/bin/sh\n"), "sh")
        self.assertEqual(check_tooling.shell_for("#!/usr/bin/env sh\n"), "sh")
        self.assertEqual(check_tooling.shell_for("#!/usr/bin/env bash\n"), "bash")

    def test_missing_frontend_fails_instead_of_skipping(self):
        with patch.object(sys, "argv", ["check_tooling.py"]), \
                patch.object(check_tooling, "source_files", return_value=[]), \
                patch.object(check_tooling.shutil, "which", return_value=None), \
                redirect_stderr(io.StringIO()) as errors:
            with self.assertRaises(SystemExit) as failure:
                check_tooling.main()
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("required tool not found: gofmt", errors.getvalue())

    def test_magic_visual_pointer_decodes_geometry_and_signed_motion(self):
        data = bytearray(40)
        data[0:4] = bytes([16, 0, 8, 0])  # Composition table and state sequence.
        data[8:13] = bytes([0, 2, 255, 1, 255])  # Visual, delay, dx, dy, terminator.
        data[16:18] = bytes([24, 0])  # Visual zero's composition pointer.
        data[24:29] = bytes([4, 4, 2, 6, 1])  # Culling extents and part count.
        data[29:36] = bytes([0, 4, 4, 2, 2, 7, 0])  # One 8x8 tile.
        state = action_magic_catalog.decode_state(bytes(data), 0x008000, 0)
        self.assertEqual(state["steps"], [{"visual": 0, "delay": 2, "dx": -1, "dy": 1}])
        self.assertEqual(state["summary"]["nominal_ticks"], 3)
        self.assertEqual(state["summary"]["visual_ids"], [0])
        self.assertEqual(state["summary"]["geometry_union"],
                         {"left": 0, "right": 8, "top": 0, "bottom": 8})
        self.assertEqual(state["visuals"][0]["parts"][0]["tile"], 7)


if __name__ == "__main__":
    unittest.main()

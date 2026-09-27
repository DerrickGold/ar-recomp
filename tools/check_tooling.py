#!/usr/bin/env python3
"""Check authored tools and applications with their language's own frontends.

Run with the Python environment from tools/requirements-quality.txt. Missing
Go, Node, ESLint, ShellCheck, shell interpreters or Ruff is an error, never a
successful skipped gate.
Generated and vendored files are outside this gate; their generators are checked.
"""

import argparse
from collections import Counter
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_PARTS = {"third_party", "vendor", "node_modules", "gen", "generated"}


def language(path):
    """Select source files rather than build output or bundled dependencies."""
    if EXCLUDED_PARTS.intersection(path.parts):
        return None
    return {".py": "python", ".go": "go", ".js": "javascript",
            ".mjs": "javascript", ".sh": "shell", ".command": "shell"}.get(path.suffix)


def source_files(root):
    output = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=root)
    return sorted({Path(name.decode()) for name in output.split(b"\0")
                   if name and (root / name.decode()).is_file() and language(Path(name.decode()))})


def shell_for(text):
    first = text.splitlines()[0] if text else ""
    return "sh" if first in ("#!/bin/sh", "#!/usr/bin/env sh") else "bash"


def run(command, root):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(command, cwd=root, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    files = source_files(root)
    for executable in ("gofmt", "node", "bash", "sh", "shellcheck"):
        if not shutil.which(executable):
            parser.error(f"required tool not found: {executable}")
    eslint = root / "node_modules/eslint/bin/eslint.js"
    if not eslint.is_file():
        parser.error("required tool not found: ESLint; run npm ci at the repository root")
    python_files = [str(path) for path in files if language(path) == "python"]
    run([sys.executable, "-m", "ruff", "check", "--no-cache", *python_files], root)
    go_files = [str(path) for path in files if language(path) == "go"]
    result = subprocess.run(["gofmt", "-l", *go_files], cwd=root,
                            check=True, capture_output=True, text=True)
    if result.stdout:
        sys.exit("Run gofmt on these files:\n" + result.stdout)
    javascript_files = [str(path) for path in files if language(path) == "javascript"]
    if javascript_files:
        run(["node", str(eslint), "--max-warnings=0", *javascript_files], root)
    shell_files = [str(path) for path in files if language(path) == "shell"]
    if shell_files:
        run(["shellcheck", *shell_files], root)
    for path in files:
        if language(path) == "shell":
            run([shell_for((root / path).read_text()), "-n", str(path)], root)
    counts = Counter(language(path) for path in files)
    print("Tooling checks passed: " + ", ".join(f"{n} {kind}" for kind, n in sorted(counts.items())))


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)

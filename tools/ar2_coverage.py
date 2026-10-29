#!/usr/bin/env python3
"""Record observed AR2 dispatches and render their reviewed CFG entries.

record requires the matching local ROM and a probe report with its diagnostic
files. sync is deterministic and ROM-free; check verifies the committed CFG.
Only the marked sections are owned by this tool. Native handlers remain native.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from ar2_metadata import ROOT, ROM_SHA256, ROM_BYTES

BEGIN = "# BEGIN AR2 COVERAGE (tools/ar2_coverage.py sync)"
END = "# END AR2 COVERAGE"
DEFAULT_MANIFEST = ROOT / "recomp/ar2/coverage.json"
# Operand lengths audited against the USA native COP handlers. This is a
# deliberately finite set, not a guess that every COP continues at PC+2.
COP_OPERANDS = {0x00: 3, 0x03: 2, 0x04: 0, 0x11: 0, 0x16: 1, 0x17: 1,
                0x19: 1, 0x1c: 0, 0x1e: 1, 0x20: 0, 0x25: 2, 0x26: 0,
                0x80: 1, 0x81: 1, 0x82: 2, 0x84: 2,
                0x85: 2, 0x86: 3, 0x8c: 1, 0x8e: 0, 0x8f: 0, 0x94: 0,
                0x96: 3, 0x9c: 3, 0xa2: 0, 0xa9: 2, 0xb0: 0, 0xb1: 0,
                0xc8: 0, 0xc9: 0}
# $00 calls a task subroutine: $80:847D stores the return after its 3-byte
# pointer, and $01 resumes it. The target is executable like $96/$9C's tasks.
# $04's fallthrough is reached only when the native loop counter expires.
# Clear/departure operands follow the native handlers at $80:84F5-$80:911B.
COP_TASK_POINTERS = (0x00, 0x96, 0x9c)


def address(text):
    if not isinstance(text, str) or not re.fullmatch(r"[0-9a-f]{6}", text):
        raise ValueError(f"invalid AR2 PC: {text!r}")
    pc = int(text, 16)
    bank = pc >> 16
    if (pc & 0x3fffff) >= ROM_BYTES or not (bank & 0x40 or pc & 0xffff >= 0x8000):
        raise ValueError(f"unmapped AR2 code address: {text}")
    return pc


def validate(manifest, rom=None):
    if manifest.get("version") != 1 or manifest.get("rom_sha256") != ROM_SHA256:
        raise ValueError("unsupported AR2 coverage manifest/ROM")
    if rom is not None and (len(rom) != ROM_BYTES or hashlib.sha256(rom).hexdigest() != ROM_SHA256):
        raise ValueError("coverage requires the unmodified headerless USA AR2 ROM")
    seen = set()
    for entry in manifest["entries"]:
        source, target = address(entry["source"]), address(entry["target"])
        if entry["m"] not in (0, 1) or entry["x"] not in (0, 1):
            raise ValueError("invalid observed CPU widths")
        key = (target, entry["m"], entry["x"])
        if key in seen:
            raise ValueError(f"duplicate coverage entry: {key}")
        seen.add(key)
        kind = entry["kind"]
        if kind == "observed":
            if not re.fullmatch(r"[0-9a-f]{64}", entry.get("evidence_sha256", "")):
                raise ValueError("observed edge requires its diagnostic-log hash")
            if not re.fullmatch(r"[0-9a-f]{2,4}", entry.get("scene", "")):
                raise ValueError("invalid observed scene word")
        elif kind not in ("cop_continuation", "cop_task"):
            raise ValueError(f"unknown coverage evidence kind: {kind}")
        if rom is None or kind == "observed":
            continue
        offset = source & 0x3fffff
        signature = rom[offset + 1]
        if rom[offset] != 2 or signature not in COP_OPERANDS or entry["m"] or entry["x"]:
            raise ValueError(f"unaudited COP signature/state at {source:06x}")
        if kind == "cop_task":
            if signature not in COP_TASK_POINTERS:
                raise ValueError("COP does not have an audited task pointer")
            expected = int.from_bytes(rom[offset + 2:offset + 5], "little")
        else:
            expected = source + 2 + COP_OPERANDS[signature]
        if target != expected:
            raise ValueError(f"COP evidence disagrees with ROM at {source:06x}")


def load(path):
    manifest = json.loads(path.read_text())
    validate(manifest)
    return manifest


def save(path, manifest):
    validate(manifest)
    entries = sorted(manifest["entries"], key=lambda e: (e["target"], e["m"], e["x"]))
    text = ('{\n  "version": 1,\n  "rom_sha256": "' + ROM_SHA256 + '",\n  "entries": [\n' +
            ',\n'.join('    ' + json.dumps(entry, separators=(',', ':')) for entry in entries) +
            '\n  ]\n}\n')
    temporary = path.with_suffix(".tmp")
    temporary.write_text(text)
    temporary.replace(path)


def sync(manifest, directory, check=False):
    validate(manifest)
    configured = {int(p.stem[4:], 16) for p in directory.glob("bank*.cfg")}
    if any(int(e["target"][:2], 16) not in configured for e in manifest["entries"]):
        raise ValueError("coverage target has no bank config")
    changes = []
    for path in sorted(directory.glob("bank*.cfg")):
        bank = int(path.stem[4:], 16)
        text = path.read_text()
        if text.count(BEGIN) != text.count(END) or text.count(BEGIN) > 1:
            raise ValueError(f"incomplete generated coverage section: {path}")
        authored = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n?", "", text, flags=re.S)
        entries = [e for e in manifest["entries"] if int(e["target"][:2], 16) == bank]
        if not entries and BEGIN not in text:
            continue
        lines = []
        for entry in sorted(entries, key=lambda e: (e["target"], e["m"], e["x"])):
            pc = int(entry["target"][2:], 16)
            name = f"bank_{bank:02X}_{pc:04X}"
            for declaration in re.findall(rf"^func \S+ {pc:04X}\b[^\n]*", authored, re.M | re.I):
                mode = re.search(r"\bentry_mx:([01]),([01])\b", declaration)
                # Native COP dispatch can enter the same routine with a new
                # observed width. Only an explicitly different authored mode
                # permits that extra variant; unknown modes stay conservative.
                if mode is None or tuple(map(int, mode.groups())) == (entry['m'], entry['x']):
                    raise ValueError(f"coverage duplicates an authored entry: {entry['target']}")
                name = declaration.split()[1]
            # All widths at a PC share one base symbol. The generator supplies
            # _M0X0 etc.; distinct aliases would misname the other dispatch slots.
            lines.append(f"# {entry['kind']}: ${entry['source'].upper()} -> ${entry['target'].upper()}\n"
                         f"func {name} {pc:04X} entry_mx:{entry['m']},{entry['x']}\n")
        wanted = authored.rstrip() + "\n\n" + BEGIN + "\n" + "".join(lines) + END + "\n"
        if text != wanted:
            changes.append((path, wanted))
    if check and changes:
        raise ValueError("stale AR2 coverage; run python3 tools/ar2_coverage.py sync: " +
                         ", ".join(p.name for p, _ in changes))
    for path, wanted in changes:
        path.write_text(wanted)


def record(manifest, report, rom, expand=False):
    validate(manifest, rom)
    if report.get("rom_sha256") != ROM_SHA256:
        raise ValueError("probe report has no matching ROM identity")
    entries = {(e["target"], e["m"], e["x"]): e for e in manifest["entries"]}
    for case in report["cases"]:
        if not case["missing_dispatch"]:
            continue
        source, target, m, x = case["missing_dispatch"]
        folder = Path(case["artifacts"])
        log = (folder / "stderr.log").read_bytes()
        cpu = json.loads((folder / "exit.cpu.json").read_text())
        marker = f"Uncovered AR2 code {source} -> {target} (M{m} X{x})".encode()
        if marker not in log or not cpu["failed"] or (cpu["m"], cpu["x_flag"]) != (int(m), int(x)):
            raise ValueError("probe diagnostic evidence disagrees with dispatch report")
        entry = {"kind": "observed", "source": source.lower(), "target": target.lower(),
                 "m": int(m), "x": int(x), "scene": f"{cpu['scene']:02x}",
                 "evidence_sha256": hashlib.sha256(log).hexdigest()}
        entries.setdefault((entry["target"], entry["m"], entry["x"]), entry)
        pc = address(entry["target"])
        while expand and m == x == "0" and rom[pc & 0x3fffff] == 2:
            offset = pc & 0x3fffff
            signature = rom[offset + 1]
            if signature not in COP_OPERANDS:
                break
            following = pc + 2 + COP_OPERANDS[signature]
            targets = [("cop_continuation", following)]
            if signature in COP_TASK_POINTERS:
                targets.append(("cop_task", int.from_bytes(rom[offset + 2:offset + 5], "little")))
            for kind, target in targets:
                entry = {"kind": kind, "source": f"{pc:06x}", "target": f"{target:06x}", "m": 0, "x": 0}
                entries.setdefault((entry["target"], 0, 0), entry)
            pc = address(f"{following:06x}")
    result = {**manifest, "entries": list(entries.values())}
    validate(result, rom)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check", "sync", "record"))
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--cfg-dir", type=Path, default=ROOT / "recomp/ar2")
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--expand-audited-cop", action="store_true")
    args = parser.parse_args()
    try:
        manifest = load(args.manifest)
        rom = args.rom.read_bytes() if args.rom else None
        validate(manifest, rom)
        if args.command == "record":
            if rom is None or args.report is None:
                parser.error("record requires --rom and --report")
            manifest = record(manifest, json.loads(args.report.read_text()), rom, args.expand_audited_cop)
            save(args.manifest, manifest)
        else:
            sync(manifest, args.cfg_dir, check=args.command == "check")
        print(f"AR2 coverage: {len(manifest['entries'])} validated entries")
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()

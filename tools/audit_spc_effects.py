#!/usr/bin/env python3
"""Audit every ActRaiser common-bank effect sequence.

The common audio block at LoROM $06:AC00 contains several direct ARAM uploads,
including the sequence table at $2400 and instrument table at $2E00.  This tool
walks the same event grammar as the resident effect interpreter, reports which
$E0-$FA commands each sequence can execute, and checks the control features
that matter to the extended-voice bridge.

Usage:
  python3 tools/audit_spc_effects.py
  python3 tools/audit_spc_effects.py --rom ar.sfc --verbose
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from dis_spc700 import ROM_BLOCK_OFFSET, TEMPLATES, operand_length


COMMON_AUDIO_OFFSET = 0x032C00
FIRST_EFFECT_ID = 0x01
LAST_EFFECT_ID = 0x26
FIRST_COMMAND = 0xE0
LAST_COMMAND = 0xFA
COMMAND_POINTER_TABLE = 0x0A54
COMMAND_LENGTH_TABLE = 0x0A8A


def word(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def resident_payload(rom: bytes) -> tuple[int, bytes]:
    length = word(rom, ROM_BLOCK_OFFSET)
    target = word(rom, ROM_BLOCK_OFFSET + 2)
    start = ROM_BLOCK_OFFSET + 4
    return target, rom[start:start + length]


def common_uploads(rom: bytes) -> dict[int, bytes]:
    """Read the direct [length16,target16,payload] blocks needed here."""
    uploads: dict[int, bytes] = {}
    offset = COMMON_AUDIO_OFFSET
    while offset + 4 <= len(rom):
        length = word(rom, offset)
        target = word(rom, offset + 2)
        if length == 0 or offset + 4 + length > len(rom):
            break
        payload = rom[offset + 4:offset + 4 + length]
        uploads[target] = payload
        offset += 4 + length
        if 0x2400 in uploads and 0x2E00 in uploads:
            break
    return uploads


def aram_byte(uploads: dict[int, bytes], address: int) -> int:
    for target, payload in uploads.items():
        if target <= address < target + len(payload):
            return payload[address - target]
    raise ValueError(f"ARAM ${address:04X} is absent from common uploads")


@dataclass
class SequenceAudit:
    effect_id: int
    entry: int
    commands: Counter[int] = field(default_factory=Counter)
    command_arguments: dict[int, set[tuple[int, ...]]] = field(
        default_factory=lambda: defaultdict(set)
    )
    events: int = 0
    bytes_read: int = 0
    terminated: bool = False
    error: str | None = None


def audit_sequence(
    effect_id: int,
    uploads: dict[int, bytes],
    command_lengths: dict[int, int],
) -> SequenceAudit:
    entry = aram_byte(uploads, 0x2400 + effect_id * 2)
    entry |= aram_byte(uploads, 0x2401 + effect_id * 2) << 8
    result = SequenceAudit(effect_id, entry)
    pc = entry
    # $EF changes the live cursor. Tracking (pc, event count) is insufficient
    # for authored repeats, so use a generous hard guard and report any loop.
    for _ in range(65536):
        try:
            value = aram_byte(uploads, pc)
        except ValueError as exc:
            result.error = str(exc)
            return result
        pc += 1
        result.bytes_read += 1
        if value == 0:
            result.terminated = True
            return result

        # A positive byte starts a new duration. A second positive byte is the
        # packed duration/velocity value; the following byte is note/command.
        if value < 0x80:
            value = aram_byte(uploads, pc)
            pc += 1
            result.bytes_read += 1
            if value < 0x80:
                value = aram_byte(uploads, pc)
                pc += 1
                result.bytes_read += 1

        if value < FIRST_COMMAND:
            result.events += 1
            continue
        if value > LAST_COMMAND:
            result.error = f"unsupported command ${value:02X} at ${pc - 1:04X}"
            return result

        count = command_lengths[value]
        arguments = tuple(aram_byte(uploads, pc + i) for i in range(count))
        pc += count
        result.bytes_read += count
        result.commands[value] += 1
        result.command_arguments[value].add(arguments)

        # $EF installs an authored sub-sequence cursor. The native effect
        # interpreter follows it immediately; bytes after the command are the
        # saved return position used by shared look-ahead/repeat machinery.
        if value == 0xEF:
            pc = arguments[0] | (arguments[1] << 8)

    result.error = "instruction guard exhausted (probable authored loop)"
    return result


def direct_page_references(
    resident_target: int, resident: bytes, address: int,
) -> list[int]:
    """Return decoded instruction PCs with a literal reference to one DP byte."""
    refs: list[int] = []
    pc = resident_target
    end = resident_target + len(resident)
    while pc < end:
        opcode = resident[pc - resident_target]
        template = TEMPLATES[opcode]
        size = 1 + operand_length(template)
        raw = resident[pc - resident_target:pc - resident_target + size]
        if len(raw) != size:
            break
        operands: set[int] = set()
        if "{b2}" in template:
            operands.update((raw[1], raw[2]))
        elif "{b}" in template:
            operands.add(raw[1])
        if address in operands:
            refs.append(pc)
        pc += size
    return refs


def format_command(command: int) -> str:
    return f"${command:02X}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", type=Path, default=Path("ar.sfc"))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    rom = args.rom.read_bytes()
    resident_target, resident = resident_payload(rom)
    uploads = common_uploads(rom)
    if 0x2400 not in uploads or 0x2E00 not in uploads:
        parser.error("common audio block did not contain $2400/$2E00 uploads")

    def resident_byte(address: int) -> int:
        if not resident_target <= address < resident_target + len(resident):
            raise ValueError(f"resident address ${address:04X} is out of range")
        return resident[address - resident_target]

    command_lengths = {
        command: resident_byte(COMMAND_LENGTH_TABLE + command - FIRST_COMMAND)
        for command in range(FIRST_COMMAND, LAST_COMMAND + 1)
    }
    command_handlers = {
        command: (
            resident_byte(COMMAND_POINTER_TABLE + (command - FIRST_COMMAND) * 2)
            | resident_byte(
                COMMAND_POINTER_TABLE + (command - FIRST_COMMAND) * 2 + 1
            ) << 8
        )
        for command in range(FIRST_COMMAND, LAST_COMMAND + 1)
    }

    audits = [
        audit_sequence(effect_id, uploads, command_lengths)
        for effect_id in range(FIRST_EFFECT_ID, LAST_EFFECT_ID + 1)
    ]
    totals: Counter[int] = Counter()
    ids_by_command: dict[int, list[int]] = defaultdict(list)
    noise_ids: set[int] = set()
    for audit in audits:
        totals.update(audit.commands)
        for command, argument_sets in audit.command_arguments.items():
            ids_by_command[command].append(audit.effect_id)
            if command == 0xE0:
                for arguments in argument_sets:
                    instrument = arguments[0]
                    descriptor = aram_byte(uploads, 0x2E00 + instrument * 6)
                    if descriptor & 0x80:
                        noise_ids.add(audit.effect_id)

    failed = [audit for audit in audits if not audit.terminated]
    print(
        f"effects={len(audits)} terminated={len(audits) - len(failed)} "
        f"events={sum(a.events for a in audits)}"
    )
    for command in range(FIRST_COMMAND, LAST_COMMAND + 1):
        if totals[command] or args.verbose:
            ids = " ".join(f"{effect_id:02X}" for effect_id in ids_by_command[command])
            print(
                f"{format_command(command)} handler=${command_handlers[command]:04X} "
                f"operands={command_lengths[command]} uses={totals[command]} "
                f"ids=[{ids}]"
            )
    print(
        "noise instruments: "
        + (" ".join(f"{effect_id:02X}" for effect_id in sorted(noise_ids))
           if noise_ids else "none")
    )
    echo_commands = sorted(
        command for command in range(0xF5, 0xF9) if totals[command]
    )
    print(
        "effect echo-control commands $F5-$F8: "
        + (" ".join(format_command(command) for command in echo_commands)
           if echo_commands else "none")
    )
    pmon_refs = direct_page_references(resident_target, resident, 0x4B)
    print(
        "PMON source $4B literal instruction references: "
        + (" ".join(f"${pc:04X}" for pc in pmon_refs) if pmon_refs else "none")
    )
    print("EON formula: $38 = $4A & ~$36 (effect-owned bits are excluded)")

    if failed:
        for audit in failed:
            print(
                f"ERROR id=${audit.effect_id:02X} entry=${audit.entry:04X}: "
                f"{audit.error}"
            )
        raise SystemExit(1)


if __name__ == "__main__":
    main()

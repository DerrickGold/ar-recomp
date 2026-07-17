#!/usr/bin/env python3
"""Inspect and safely edit ActRaiser's exact 8 KiB battery-save image."""

from __future__ import annotations

import argparse
import configparser
import pathlib
import sys

SRAM_SIZE = 0x2000
CHECKSUM_OFFSET = 0x1FEC
CHUNK_SIZE = 64
REGIONS = {
    "fillmore": 0x1200,
    "bloodpool": 0x1202,
    "kasandora": 0x1204,
    "aitos": 0x1206,
    "marahna": 0x1208,
    "northwall": 0x120A,
}
REGION_ACT2_FLAGS = 0x13B6
REGION_STATES = {
    0: "act1",
    2: "act1-cleared",
    3: "act2",
    4: "act2-cleared",
}
REGION_STATE_VALUES = {name: value for value, name in REGION_STATES.items()}


def progress_name(value: int) -> str:
    return REGION_STATES.get(value, f"unknown-${value:02X}")


def region_state(image: bytes | bytearray, region: str) -> int:
    index = tuple(REGIONS).index(region)
    return image[REGIONS[region]] * 2 + (image[REGION_ACT2_FLAGS + index * 2] & 1)


def set_region_state(image: bytearray, region: str, value: int) -> None:
    if value not in REGION_STATES:
        raise ValueError(f"invalid {region} state ${value:02X}")
    index = tuple(REGIONS).index(region)
    image[REGIONS[region]] = value // 2
    flag_offset = REGION_ACT2_FLAGS + index * 2
    image[flag_offset] = (image[flag_offset] & ~1) | (value & 1)


def checksum(image: bytes | bytearray) -> int:
    if len(image) != SRAM_SIZE:
        raise ValueError(f"expected exactly {SRAM_SIZE} bytes, got {len(image)}")
    xored = 0
    summed = 0
    for offset in range(0, CHECKSUM_OFFSET, 2):
        word = image[offset] | image[offset + 1] << 8
        xored ^= word
        summed = (summed + word) & 0xFFFF
    return xored << 16 | summed


def stored_checksum(image: bytes | bytearray) -> int:
    return int.from_bytes(image[CHECKSUM_OFFSET : CHECKSUM_OFFSET + 4], "little")


def update_checksum(image: bytearray) -> int:
    value = checksum(image)
    image[CHECKSUM_OFFSET : CHECKSUM_OFFSET + 4] = value.to_bytes(4, "little")
    return value


def validate(image: bytes | bytearray, source: pathlib.Path) -> None:
    if len(image) != SRAM_SIZE:
        raise ValueError(f"{source}: expected exactly {SRAM_SIZE} bytes, got {len(image)}")
    expected = checksum(image)
    stored = stored_checksum(image)
    if stored != expected:
        raise ValueError(
            f"{source}: checksum mismatch: stored ${stored:08X}, expected ${expected:08X}"
        )


def parse_ini(path: pathlib.Path) -> bytearray:
    config = configparser.ConfigParser(interpolation=None, strict=True)
    config.optionxform = str.lower
    with path.open("r", encoding="utf-8") as stream:
        config.read_file(stream)
    meta = config["Meta"]
    if meta.get("format") != "actraiser-sram" or meta.getint("version") != 1:
        raise ValueError(f"{path}: unsupported save INI format/version")
    if int(meta.get("size", "0"), 0) != SRAM_SIZE:
        raise ValueError(f"{path}: invalid declared SRAM size")
    raw = config["Raw"]
    image = bytearray(SRAM_SIZE)
    expected_keys = {f"{offset:04x}" for offset in range(0, SRAM_SIZE, CHUNK_SIZE)}
    if set(raw) != expected_keys:
        missing = sorted(expected_keys - set(raw))
        extra = sorted(set(raw) - expected_keys)
        raise ValueError(f"{path}: raw chunks mismatch; missing={missing}, extra={extra}")
    for offset in range(0, SRAM_SIZE, CHUNK_SIZE):
        data = bytes.fromhex(raw[f"{offset:04x}"])
        if len(data) != CHUNK_SIZE:
            raise ValueError(f"{path}: raw chunk {offset:04x} is not 64 bytes")
        image[offset : offset + CHUNK_SIZE] = data
    validate(image, path)
    if config.has_section("Regions"):
        for region, offset in REGIONS.items():
            if region not in config["Regions"]:
                continue
            name = config["Regions"][region]
            if name not in REGION_STATE_VALUES:
                raise ValueError(f"{path}: invalid {region} progress {name!r}")
            set_region_state(image, region, REGION_STATE_VALUES[name])
    update_checksum(image)
    return image


def load(path: pathlib.Path) -> bytearray:
    image = parse_ini(path) if path.suffix.lower() == ".ini" else bytearray(path.read_bytes())
    validate(image, path)
    return image


def atomic_write(path: pathlib.Path, data: bytes | str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    try:
        if isinstance(data, str):
            temporary.write_text(data, encoding="utf-8")
        else:
            temporary.write_bytes(data)
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def write_ini(path: pathlib.Path, image: bytearray) -> None:
    update_checksum(image)
    lines = [
        "; ActRaiser Recompiled lossless battery save",
        "[Meta]",
        "format = actraiser-sram",
        "version = 1",
        "size = 0x2000",
        "rom = usa",
        "",
        "[Regions]",
    ]
    for region, offset in REGIONS.items():
        value = region_state(image, region)
        if value not in REGION_STATES:
            raise ValueError(f"cannot encode {region}: invalid state ${value:02X}")
        lines.append(f"{region} = {REGION_STATES[value]}")
    lines.extend(("", "[Raw]"))
    for offset in range(0, SRAM_SIZE, CHUNK_SIZE):
        lines.append(f"{offset:04x} = {image[offset:offset + CHUNK_SIZE].hex()}")
    atomic_write(path, "\n".join(lines) + "\n")


def write(path: pathlib.Path, image: bytearray) -> None:
    update_checksum(image)
    if path.suffix.lower() == ".ini":
        write_ini(path, image)
    else:
        atomic_write(path, bytes(image))


def describe(path: pathlib.Path, image: bytearray) -> None:
    print(f"{path}: {len(image)} bytes, checksum ${stored_checksum(image):08X} OK")
    for region, offset in REGIONS.items():
        index = tuple(REGIONS).index(region)
        flag_offset = REGION_ACT2_FLAGS + index * 2
        value = region_state(image, region)
        print(
            f"  {region:10s} ${offset:04X}=${image[offset]:02X} "
            f"${flag_offset:04X}.0={image[flag_offset] & 1} -> "
            f"${value:02X} {progress_name(value)}"
        )


def changed_ranges(left: bytes, right: bytes) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    start = -1
    for offset, (a, b) in enumerate(zip(left, right)):
        if a != b and start < 0:
            start = offset
        elif a == b and start >= 0:
            result.append((start, offset))
            start = -1
    if start >= 0:
        result.append((start, len(left)))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="validate checksum and verified fields")
    check.add_argument("paths", nargs="+", type=pathlib.Path)
    decode = commands.add_parser("decode", help="print verified fields")
    decode.add_argument("path", type=pathlib.Path)
    diff = commands.add_parser("diff", help="show changed byte ranges and verified fields")
    diff.add_argument("left", type=pathlib.Path)
    diff.add_argument("right", type=pathlib.Path)
    edit = commands.add_parser("edit", help="write a checksum-correct edited copy")
    edit.add_argument("input", type=pathlib.Path)
    edit.add_argument("output", type=pathlib.Path)
    edit.add_argument("--region", action="append", default=[], metavar="NAME=VALUE")
    convert = commands.add_parser("convert", help="convert .srm <-> lossless .ini")
    convert.add_argument("input", type=pathlib.Path)
    convert.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    try:
        if args.command == "check":
            for path in args.paths:
                describe(path, load(path))
        elif args.command == "decode":
            describe(args.path, load(args.path))
        elif args.command == "diff":
            left, right = load(args.left), load(args.right)
            ranges = changed_ranges(left, right)
            print(f"{len(ranges)} changed range(s), {sum(b-a for a, b in ranges)} byte(s)")
            for start, end in ranges:
                print(f"  ${start:04X}-${end - 1:04X} ({end - start} bytes)")
            for region, offset in REGIONS.items():
                left_state = region_state(left, region)
                right_state = region_state(right, region)
                if left_state != right_state:
                    print(
                        f"  {region}: {progress_name(left_state)} -> "
                        f"{progress_name(right_state)}"
                    )
        elif args.command == "edit":
            image = load(args.input)
            for assignment in args.region:
                if "=" not in assignment:
                    raise ValueError(f"--region wants NAME=VALUE, got {assignment!r}")
                region, value = assignment.split("=", 1)
                region, value = region.lower(), value.lower()
                if region not in REGIONS or value not in REGION_STATE_VALUES:
                    raise ValueError(f"invalid region edit {assignment!r}")
                set_region_state(image, region, REGION_STATE_VALUES[value])
            write(args.output, image)
            describe(args.output, load(args.output))
        else:
            write(args.output, load(args.input))
            print(f"{args.input} -> {args.output}")
        return 0
    except (OSError, ValueError, configparser.Error) as exc:
        print(f"srm.py: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Small SPC700 disassembler for ROM-static ActRaiser audio research.

The ActRaiser US ROM stores its resident SPC700 upload as one block at file
offset $011ACD: [length16][ARAM target16][payload].  This tool defaults to that
payload and accepts ARAM addresses, which keeps driver notes comparable with
live SPC traces.

Usage:
  python3 tools/dis_spc700.py 0D90 0E70
  python3 tools/dis_spc700.py 0800 08F0 --rom ar.sfc
  python3 tools/dis_spc700.py 0400 0F4D --find-ref 004B
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROM_BLOCK_OFFSET = 0x011ACD


# Operand templates use b=direct-page byte, w=absolute word, r=relative byte,
# and m=the SPC700's 13-bit-address + 3-bit bit-number encoding.  The opcode
# matrix mirrors the hardware's regular 16x16 layout, making the table easy to
# audit against runtime-next/src/snes/spc.c.
ROWS = [
    "nop|tcall 0|set1 {b}.0|bbs {b}.0,{r}|or a,{b}|or a,!{w}|or a,(x)|or a,[{b}+x]|or a,#{b}|or {b},{b2}|or1 c,{m}|asl {b}|asl !{w}|push psw|tset1 !{w}|brk",
    "bpl {r}|tcall 1|clr1 {b}.0|bbc {b}.0,{r}|or a,{b}+x|or a,!{w}+x|or a,!{w}+y|or a,[{b}]+y|or {b},#{b2}|or (x),(y)|decw {b}|asl {b}+x|asl a|dec x|cmp x,!{w}|jmp [!{w}+x]",
    "clrp|tcall 2|set1 {b}.1|bbs {b}.1,{r}|and a,{b}|and a,!{w}|and a,(x)|and a,[{b}+x]|and a,#{b}|and {b},{b2}|or1 c,/{m}|rol {b}|rol !{w}|push a|cbne {b},{r}|bra {r}",
    "bmi {r}|tcall 3|clr1 {b}.1|bbc {b}.1,{r}|and a,{b}+x|and a,!{w}+x|and a,!{w}+y|and a,[{b}]+y|and {b},#{b2}|and (x),(y)|incw {b}|rol {b}+x|rol a|inc x|cmp x,{b}|call !{w}",
    "setp|tcall 4|set1 {b}.2|bbs {b}.2,{r}|eor a,{b}|eor a,!{w}|eor a,(x)|eor a,[{b}+x]|eor a,#{b}|eor {b},{b2}|and1 c,{m}|lsr {b}|lsr !{w}|push x|tclr1 !{w}|pcall ${b}",
    "bvc {r}|tcall 5|clr1 {b}.2|bbc {b}.2,{r}|eor a,{b}+x|eor a,!{w}+x|eor a,!{w}+y|eor a,[{b}]+y|eor {b},#{b2}|eor (x),(y)|cmpw ya,{b}|lsr {b}+x|lsr a|mov x,a|cmp y,!{w}|jmp !{w}",
    "clrc|tcall 6|set1 {b}.3|bbs {b}.3,{r}|cmp a,{b}|cmp a,!{w}|cmp a,(x)|cmp a,[{b}+x]|cmp a,#{b}|cmp {b},{b2}|and1 c,/{m}|ror {b}|ror !{w}|push y|dbnz {b},{r}|ret",
    "bvs {r}|tcall 7|clr1 {b}.3|bbc {b}.3,{r}|cmp a,{b}+x|cmp a,!{w}+x|cmp a,!{w}+y|cmp a,[{b}]+y|cmp {b},#{b2}|cmp (x),(y)|addw ya,{b}|ror {b}+x|ror a|mov a,x|cmp y,{b}|reti",
    "setc|tcall 8|set1 {b}.4|bbs {b}.4,{r}|adc a,{b}|adc a,!{w}|adc a,(x)|adc a,[{b}+x]|adc a,#{b}|adc {b},{b2}|eor1 c,{m}|dec {b}|dec !{w}|mov y,#{b}|pop psw|mov {b},#{b2}",
    "bcc {r}|tcall 9|clr1 {b}.4|bbc {b}.4,{r}|adc a,{b}+x|adc a,!{w}+x|adc a,!{w}+y|adc a,[{b}]+y|adc {b},#{b2}|adc (x),(y)|subw ya,{b}|dec {b}+x|dec a|mov x,sp|div ya,x|xcn a",
    "ei|tcall 10|set1 {b}.5|bbs {b}.5,{r}|sbc a,{b}|sbc a,!{w}|sbc a,(x)|sbc a,[{b}+x]|sbc a,#{b}|sbc {b},{b2}|mov1 c,{m}|inc {b}|inc !{w}|cmp y,#{b}|pop a|mov (x)+,a",
    "bcs {r}|tcall 11|clr1 {b}.5|bbc {b}.5,{r}|sbc a,{b}+x|sbc a,!{w}+x|sbc a,!{w}+y|sbc a,[{b}]+y|sbc {b},#{b2}|sbc (x),(y)|movw ya,{b}|inc {b}+x|inc a|mov sp,x|das a|mov a,(x)+",
    "di|tcall 12|set1 {b}.6|bbs {b}.6,{r}|mov {b},a|mov !{w},a|mov (x),a|mov [{b}+x],a|cmp x,#{b}|mov !{w},x|mov1 {m},c|mov {b},y|mov !{w},y|mov x,#{b}|pop x|mul ya",
    "bne {r}|tcall 13|clr1 {b}.6|bbc {b}.6,{r}|mov {b}+x,a|mov !{w}+x,a|mov !{w}+y,a|mov [{b}]+y,a|mov {b},x|mov {b}+y,x|movw {b},ya|mov {b}+x,y|dec y|mov a,y|cbne {b}+x,{r}|daa a",
    "clrv|tcall 14|set1 {b}.7|bbs {b}.7,{r}|mov a,{b}|mov a,!{w}|mov a,(x)|mov a,[{b}+x]|mov a,#{b}|mov x,!{w}|not1 {m}|mov y,{b}|mov y,!{w}|notc|pop y|sleep",
    "beq {r}|tcall 15|clr1 {b}.7|bbc {b}.7,{r}|mov a,{b}+x|mov a,!{w}+x|mov a,!{w}+y|mov a,[{b}]+y|mov x,{b}|mov x,{b}+y|mov {b},{b2}|mov y,{b}+x|inc y|mov y,a|dbnz y,{r}|stop",
]

TEMPLATES = [entry for row in ROWS for entry in row.split("|")]
assert len(TEMPLATES) == 256


def operand_length(template: str) -> int:
    if "{w}" in template or "{m}" in template:
        return 2
    if "{b2}" in template or ("{b}" in template and "{r}" in template):
        return 2
    if "{b}" in template or "{r}" in template:
        return 1
    return 0


def decode_operand(template: str, raw: bytes, pc: int) -> str:
    values: dict[str, str] = {}
    if "{w}" in template or "{m}" in template:
        word = raw[1] | (raw[2] << 8)
        values["w"] = f"${word:04X}"
        values["m"] = f"${word & 0x1FFF:04X}.{word >> 13}"
    elif "{b2}" in template:
        # Memory-to-memory opcodes encode source first and destination second;
        # render in execution/assembly order rather than byte order.
        values["b"] = f"${raw[2]:02X}"
        values["b2"] = f"${raw[1]:02X}"
    else:
        if "{b}" in template:
            values["b"] = f"${raw[1]:02X}"
        if "{r}" in template:
            displacement = raw[-1] - 0x100 if raw[-1] & 0x80 else raw[-1]
            values["r"] = f"${(pc + len(raw) + displacement) & 0xFFFF:04X}"
    if "{r}" in template and "r" not in values:
        displacement = raw[-1] - 0x100 if raw[-1] & 0x80 else raw[-1]
        values["r"] = f"${(pc + len(raw) + displacement) & 0xFFFF:04X}"
    return template.format(**values)


def parse_address(value: str) -> int:
    return int(value.removeprefix("$").removeprefix("0x"), 16)


def operand_references(template: str, raw: bytes) -> set[int]:
    """Return literal DP/absolute addresses encoded by one instruction."""
    if "{w}" in template:
        return {raw[1] | (raw[2] << 8)}
    if "{m}" in template:
        return {(raw[1] | (raw[2] << 8)) & 0x1FFF}
    if "{b2}" in template:
        return {raw[1], raw[2]}
    if "{b}" in template:
        return {raw[1]}
    return set()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("start", type=parse_address)
    parser.add_argument("end", type=parse_address)
    parser.add_argument("--rom", type=Path, default=Path("ar.sfc"))
    parser.add_argument(
        "--find-ref", type=parse_address,
        help="print only instructions with this literal DP/absolute operand",
    )
    args = parser.parse_args()

    rom = args.rom.read_bytes()
    length = rom[ROM_BLOCK_OFFSET] | (rom[ROM_BLOCK_OFFSET + 1] << 8)
    target = rom[ROM_BLOCK_OFFSET + 2] | (rom[ROM_BLOCK_OFFSET + 3] << 8)
    payload = rom[ROM_BLOCK_OFFSET + 4:ROM_BLOCK_OFFSET + 4 + length]
    if not target <= args.start < args.end <= target + len(payload):
        parser.error(
            f"range must be inside uploaded ARAM ${target:04X}-"
            f"${target + len(payload):04X}"
        )

    pc = args.start
    while pc < args.end:
        opcode = payload[pc - target]
        template = TEMPLATES[opcode]
        size = 1 + operand_length(template)
        raw = payload[pc - target:pc - target + size]
        if len(raw) != size:
            break
        if (args.find_ref is None or
                args.find_ref in operand_references(template, raw)):
            bytes_text = " ".join(f"{byte:02X}" for byte in raw)
            decoded = decode_operand(template, raw, pc)
            print(f"{pc:04X}: {bytes_text:<8} {decoded}")
        pc += size


if __name__ == "__main__":
    main()

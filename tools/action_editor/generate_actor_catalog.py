#!/usr/bin/env python3
"""Generate the editor's actor-family picker from the canonical US ROM descriptors."""
import argparse
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def generate(rom):
    if len(rom) != 0x100000 or rom[0x7fc0:0x7fc9] != b'ACTRAISER':
        raise ValueError('Expected the unheadered canonical ActRaiser US ROM')
    catalog = []
    for group, base in enumerate([0x96AF, 0xA8F6, 0xB449, 0xC11E, 0xCD9B, 0xD928, 0xE722, 0xF39A]):
        if not group:
            continue
        at, end = base, 0x10000
        while at < min(end, base + 0x100):
            source = int.from_bytes(rom[at-0x8000:at-0x8000+2], 'little')
            actor_type = (at-base)//2
            at += 2
            if not source:
                continue
            if not base < source <= 65535:
                break
            end = min(end, source)
            data = rom[source-0x8000:source-0x8000+12]
            if data[0] != 0 or data[2] not in (0x7e, 0x7f):
                continue
            catalog.append(dict(group=group, type=actor_type, source=f'{source:04X}',
                animation=f'{int.from_bytes(data[:2], "little"):04X}',
                label='Bloodpool Act 1 boss' if source == 0xb786 else f'Actor type {actor_type:02X}'))
    return json.dumps(catalog, indent=2) + '\n'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, default=ROOT/'ar.sfc')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    expected = generate(args.rom.read_bytes())
    target = ROOT/'assets/effects/actor-families.json'
    if args.check:
        if not target.exists() or target.read_text() != expected:
            parser.error('Actor catalogue is stale; run tools/action_editor/generate_actor_catalog.py')
    else:
        target.write_text(expected)

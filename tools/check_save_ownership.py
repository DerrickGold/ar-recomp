#!/usr/bin/env python3
"""Keep campaign publication inside the save owner, not feature/UI callers."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {
    'SaveCheckpoint_CommitSnapshot': {'src/save/save_checkpoint.c', 'src/save/save_system.c'},
    # Payload-only compatibility entry point is for offline test fixtures.
    'SaveCheckpoint_Commit': {'src/save/save_checkpoint.c'},
    'Save_WriteFile': {'src/save/save_system.c', 'src/save/save_checkpoint.c',
                       'src/save/save_slots_migration.inc'},
    'Save_WriteCompanionFile': {'src/save/save_system.c', 'src/save/save_checkpoint.c',
                                'src/save/save_slots.c', 'src/save/save_slots_migration.inc',
                                'src/save/save_campaign_archive.inc'},
}


def violations(path, text):
    code = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', '', text, flags=re.S)
    return [name for name, owners in ALLOWED.items()
            if path not in owners and re.search(r'\b' + name + r'\s*\(', code)]


def main():
    # A future feature cannot add a direct writer or hide it in a comment.
    assert violations('src/regional/session/new_feature.c', 'Save_WriteFile(a, b, c, d);')
    assert violations('src/save/new_writer.c', 'SaveCheckpoint_CommitSnapshot(a);')
    assert not violations('src/save/save_system.c', 'SaveCheckpoint_CommitSnapshot(a);')
    assert not violations('src/ui.c', '/* Save_WriteFile(a); */')
    errors = []
    for file in (ROOT / 'src').rglob('*'):
        if file.suffix not in {'.c', '.cpp', '.inc'} or 'gen' in file.parts:
            continue
        path = file.relative_to(ROOT).as_posix()
        errors.extend(f'{path}: {name} bypasses the campaign persistence owner'
                      for name in violations(path, file.read_text()))
    if errors:
        raise SystemExit('\n'.join(errors))
    print('Save ownership: campaign writes stay inside the persistence owner')


if __name__ == '__main__':
    main()

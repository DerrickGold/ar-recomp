#!/usr/bin/env python3
"""Bundle the read-only, touch-first level audit browser into one offline HTML."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
from datetime import datetime, timezone

from build import SOURCES, preview_settings, script_json
from build_compositor import shader_variants


def build_document(rooms, layers, wasm, settings=None):
    scenery = layers.read_text() if layers.exists() else ''
    effects_path = layers.parent / 'action-effects.ini'
    effects = effects_path.read_text() if effects_path.exists() else '[effects]\nversion=1\n'
    try:
        revision = subprocess.check_output(
            ['git', 'rev-parse', '--short=12', 'HEAD'], cwd=SOURCES, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        revision = 'unknown'
    # A source commit alone does not identify an uncommitted/rebuilt bundle.
    assets = rooms.read_bytes()
    renderer = wasm.read_bytes()
    scripts = ['shared_preview.js', 'webgl2_backend.js', 'mobile/camera.js', 'mobile/browser.js']
    template = (SOURCES / 'mobile/browser.html').read_text()
    code = '\n'.join('<script>\n' + (SOURCES / name).read_text() + '\n</script>' for name in scripts)
    bundle = {'data': json.loads(assets), 'scenery': scenery, 'effects': effects,
              'wasm': base64.b64encode(renderer).decode(), 'shaders': shader_variants(),
              'view': preview_settings(settings or layers.parent / 'settings.ini')}
    fingerprint = hashlib.sha256(script_json(bundle).encode() + template.encode() + code.encode()).hexdigest()[:16]
    bundle['build'] = {'revision': revision, 'fingerprint': fingerprint,
                       'builtAt': datetime.now(timezone.utc).isoformat(timespec='seconds')}
    return (template
            .replace('<!-- BUNDLE -->', '<script>window.__ACTION_AUDIT__=' + script_json(bundle) + ';</script>')
            .replace('<!-- SCRIPTS -->', code))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rooms', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('layers', type=Path)
    parser.add_argument('--wasm', type=Path, required=True)
    parser.add_argument('--settings', type=Path)
    args = parser.parse_args()
    result = build_document(args.rooms, args.layers, args.wasm, args.settings)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result)
    print(f'[action-browser] {args.output} ({len(result.encode()) / 1048576:.2f} MiB)')

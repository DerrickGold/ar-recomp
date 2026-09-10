#!/usr/bin/env python3
import importlib.util
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'language_pack_v1', ROOT / 'tools' / 'language_pack_v1.py')
PACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACK)


def walk(value):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def main():
    catalog_path = (
        ROOT / 'tools/data/localization/semantic-catalog-v1.json')
    catalog = PACK.load_catalog(catalog_path)
    assert catalog['route_count'] == 531
    assert catalog['all_release_route_count'] == 472
    assert catalog['regional_or_release_variant_route_count'] == 59
    assert catalog['placeholder_count'] == 58
    assert catalog['release_order'] == ['us', 'eu-en', 'de', 'fr', 'jp']
    assert catalog['limits'] == {
        'authored_pages_per_message': PACK.MAX_AUTHORED_PAGES,
        'message_utf8_bytes': PACK.MAX_MESSAGE_UTF8_BYTES,
        'operations_per_message': PACK.MAX_OPERATIONS_PER_MESSAGE,
        'wait_frames_per_message': PACK.MAX_WAIT_FRAMES_PER_MESSAGE,
        'wait_frames_per_operation': PACK.MAX_WAIT_FRAMES_PER_OPERATION,
    }

    route_ids = [route['id'] for route in catalog['routes']]
    assert route_ids == sorted(route_ids)
    assert len(route_ids) == len(set(route_ids))
    assert all(PACK.IDENTIFIER_RE.fullmatch(route_id) for route_id in route_ids)
    assert not any(re.search(r'bank[0-9a-f]{2}\.[0-9a-f]{4}', route_id)
                   for route_id in route_ids)
    assert not any(key in {
        'source_operations', 'source_record_id', 'source_offset_within_record',
        'file_offset', 'raw_sha256', 'rom_sha256', 'native_address',
    } for key, _ in walk(catalog))

    for flow_route in (
            'system.choice.yes_no', 'system.message_speed.scale_labels'):
        route = next(row for row in catalog['routes']
                     if row['id'] == flow_route)
        assert route['availability'] == 'all_supported_releases'
        assert route['available_releases'] == catalog['release_order']

    for route in catalog['routes']:
        assert route['canonical_contract_profile'] in route['contracts']
        assert route['available_releases'] == [
            release for release in catalog['release_order']
            if release in route['contracts']]
        for placeholder in route['allowed_placeholders']:
            assert placeholder in catalog['placeholders']
        for contract in route['contracts'].values():
            anchor_ids = [anchor['id']
                          for anchor in contract['required_anchors']]
            assert len(anchor_ids) == len(set(anchor_ids))

    example = PACK.validate_pack(ROOT / 'examples/language-pack', catalog)
    assert example['valid']
    assert example['message_count'] == 2
    assert example['progress_rows'] == 2
    print('language pack canonical catalog checks passed')


if __name__ == '__main__':
    main()

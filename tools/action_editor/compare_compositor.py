#!/usr/bin/env python3
"""Compare an exported WebGL2 frame with its matching native replay (requires Pillow)."""
import argparse
import json
from PIL import Image, ImageChops, ImageStat


def compare(native, browser, threshold=2, maximum_fraction=0.001):
    a, b = Image.open(native).convert('RGB'), Image.open(browser).convert('RGB')
    if a.size != b.size:
        raise ValueError(f'Image extents differ: native={a.size}, browser={b.size}')
    diff = ImageChops.difference(a, b)
    pixels = diff.get_flattened_data() if hasattr(diff, 'get_flattened_data') else diff.getdata()
    over = sum(max(pixel) > threshold for pixel in pixels)
    result = dict(width=a.width, height=a.height, mean_channel_error=ImageStat.Stat(diff).mean,
                  maximum_channel_error=max(hi for _, hi in diff.getextrema()),
                  threshold=threshold, pixels_over_threshold=over,
                  fraction_over_threshold=over/(a.width*a.height), maximum_fraction=maximum_fraction)
    print(json.dumps(result, indent=2))
    return result['fraction_over_threshold'] <= maximum_fraction


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('native')
    p.add_argument('browser')
    p.add_argument('--threshold', type=int, default=2)
    p.add_argument('--maximum-fraction', type=float, default=0.001)
    args = p.parse_args()
    raise SystemExit(0 if compare(args.native, args.browser, args.threshold, args.maximum_fraction) else 1)

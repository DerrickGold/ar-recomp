#!/usr/bin/env python3
"""Compare production SIM models with both character banks of supplied ROMs.

Reads the manifest produced by sim_voxel_model_sheet. Unlike the archived
snapshot/map-crop index, originals come directly from each ROM's town asset
script. Output contains private ROM-derived images and belongs under runs/.
Usage: python3 tools/make_sim_voxel_regional_index.py --renders RENDERS --out OUT
Requires Pillow; ROM paths are relative to --rom-dir (default: repository root).
"""

import argparse
import csv
import hashlib
import html
import json
import re
from functools import lru_cache
from pathlib import Path

from PIL import Image

from act_content import high_bit, iter_script
from sim_bg_tile_catalog import Snapshot, parse_draw_lists


RELEASES = {
    "US": "ar.sfc", "Japan": "ar-jp.sfc", "Europe": "ar-eu.sfc",
    "Germany": "ar-ger.sfc", "France": "ar-fra.sfc",
}

# Reviewed headerless retail ROMs. Japan's native selector loads $03:D7CB;
# the other releases load $03:DCC6. Store the actual table, not a US fallback.
HOUSE_TABLE_OFFSETS = {r: 0x1D7CB if r == "Japan" else 0x1DCC6
                       for r in RELEASES}


def house_families(data, release):
    offset = HOUSE_TABLE_OFFSETS[release]
    values = [int.from_bytes(data[i:i + 2], "little")
              for i in range(offset, offset + 36, 2)]
    if len(data) < offset + 36 or any(v not in range(0, 16, 2) for v in values):
        raise ValueError(f"Unrecognized {release} house family table")
    return {town: values[(town - 1) * 3:town * 3] for town in range(1, 7)}


class RomArt(Snapshot):
    """Reuse the catalogue's 4bpp decoder with immutable ROM asset inputs."""

    def __init__(self, data, scripts, town, bank):
        ops = [(high_bit(command), operands)
               for command, operands in scripts[(0, town)]]
        self.wram = bytearray(0x20000)
        self.sources = {}
        for kind, operands in ops:
            if kind != 5 or operands[3] not in (1, 4):
                continue
            if not operands[0] & 0x80:
                raise ValueError("Expected raw town metatile definitions")
            atlas, destination = ("terrain", 0x2100) if operands[3] == 1 else (
                "structure", 0x3100)
            address = int.from_bytes(operands[-3:], "little")
            raw = data[address:address + 2048]
            if len(raw) != 2048:
                raise ValueError("Truncated metatile definitions")
            self.wram[destination:destination + 2048] = bytes(
                value for i in range(0, 2048, 2) for value in (raw[i + 1], raw[i]))
            self.sources[atlas] = address
        palettes = [o for k, o in ops if k == 6 and o[2] == 0]
        address = int.from_bytes(palettes[0][-3:], "little")
        self.cgram = data[address:address + 256] + bytes(256)
        self.palette = tuple(self._decode_color(i) for i in range(256))
        self.sources["palette"] = address
        characters = [o for k, o in ops if k == 7 and o[2] == 0]
        address = int.from_bytes(characters[bank][-3:], "little")
        self.vram = data[address:address + 0x4000] + bytes(0xC000)
        self.sources["characters"] = address


def source_spec(filename, draw_lists, families):
    """Return town, atlas, composition and source identity, without redrawing."""
    match = re.fullmatch(r"house-(\d)-(\d)-(front|alternate)\.bmp", filename)
    if match:
        town, tier = int(match[1]), int(match[2])
        family = families[town][tier]
        tile = family // 2 * 8 + (3 if match[3] == "alternate" else 2)
        return town, "structure", [(0, 0, tile)], f"house family ${family:02X}"

    plots = {
        "cathedral-temperate.bmp": (1, "terrain", 0xC2, "sanctuary $C2"),
        "cathedral-snow.bmp": (6, "terrain", 0xC2, "sanctuary $C2"),
        "bloodpool-castle.bmp": (2, "structure", 0xD4, "landmark 1 / mark $EC"),
        "kasandora-pyramid.bmp": (3, "structure", 0xD6, "landmark 15 / mark $EE"),
        # This is the separate landmark, NOT the town's ordinary cathedral.
        "marahna-temple.bmp": (5, "structure", 0xE4, "landmark 16 / mark $EF"),
        "northwall-story-tree.bmp": (6, "structure", 0xE6, "landmark 2 / mark $EB"),
    }
    if filename in plots:
        town, atlas, tile, identity = plots[filename]
        return town, atlas, [(0, 0, tile), (1, 0, tile + 1),
                             (0, 1, tile + 8), (1, 1, tile + 9)], identity
    match = re.fullmatch(r"tree-town-(\d)\.bmp", filename)
    if match:
        return int(match[1]), "terrain", [(0, 0, 0x0B)], "representative evergreen $0B"
    vegetation = {
        "broad-tree-kasandora.bmp": (3, 0x0D),
        "broad-tree-marahna.bmp": (5, 0x0D),
        "marahna-palm.bmp": (5, 0x09), "clearable-shrub.bmp": (1, 0x01),
    }
    if filename in vegetation:
        town, tile = vegetation[filename]
        return town, "terrain", [(0, 0, tile)], "representative vegetation metatile"
    if filename == "fillmore-boulder.bmp":
        return 1, "terrain", [(0, 0, 0x61)], "lightning boulder $61"
    match = re.fullmatch(r"rocky-ground-([0-9a-f]{2})\.bmp", filename)
    if match:
        tile = int(match[1], 16)
        return 4, "terrain", [(0, 0, tile)], f"scattered stones ${tile:02X}"
    if filename.startswith("bridge-"):
        snow, ew = "snow" in filename, "-ew" in filename
        tile = (0xE2 if snow else 0x44) + int(not ew)
        return 6 if snow else 1, "structure", [(0, 0, tile)], "completed bridge cell"
    if filename == "construction-house.bmp":
        return 1, "structure", [(0, 0, 0)], "first shared house scaffold"
    drawings = {
        "windmill-phase-0.bmp": 77, "windmill-phase-1.bmp": 78,
        "windmill-phase-2.bmp": 76, "windmill-snow.bmp": 77,
        "factory-temperate.bmp": 89, "factory-snow.bmp": 89,
        "construction-windmill-0.bmp": 73, "construction-windmill-1.bmp": 74,
        "construction-windmill-2.bmp": 75, "construction-factory.bmp": 88,
    }
    item = draw_lists[drawings[filename]]
    return (6 if "snow" in filename else 1), "structure", [
        (c.dx, c.dy, c.metatile) for c in item.commands], f"shared composition, US draw list {item.index}"


def compose(art, atlas, cells):
    image = Image.new("RGBA", ((max(c[0] for c in cells) + 1) * 16,
                               (max(c[1] for c in cells) + 1) * 16))
    for x, y, tile in cells:
        image.alpha_composite(art.render_metatile(atlas, tile), (x * 16, y * 16))
    return image


def write_gallery(out, rows, releases, states):
    cards = []
    for row in rows:
        value = lambda key: html.escape(str(row[key]), quote=True)
        cards.append(f'''<article data-release="{value('release')}" data-bank="{value('bank')}">
<h2>#{value('number')} {value('label')}</h2><div class="pair">
<figure><img class="original" loading="lazy" alt="Original {value('label')}" src="{value('original_image')}"><figcaption>Original · {value('release')} · {value('bank')}</figcaption></figure>
<figure><img class="model" loading="lazy" alt="Current 3D {value('label')}" src="{value('model_image')}"><figcaption>Current 3D · Ultra / Varied</figcaption></figure></div>
<p>{value('identity')} · {value('atlas')} {value('metatiles')}</p>
<p class="status">{value('model_status')}</p></article>''')
    state_cards = "".join(
        f'<figure><img class="model" src="images/{html.escape(Path(row["file"]).stem)}.png" '
        f'alt="{html.escape(row["label"])}"><figcaption>{html.escape(row["label"])}</figcaption></figure>'
        for row in states)
    options = "".join(f'<option>{html.escape(r)}</option>' for r in releases)
    page = '''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>SIM model art index</title>
<style>body{margin:24px;background:#10181e;color:#e9eef2;font:16px system-ui}h1{font-size:28px}header{max-width:1100px}select,input{font:inherit;padding:8px;margin:8px;background:#20303c;color:white;border:1px solid #657887}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(410px,1fr));gap:18px}article{background:#1d2a33;border:1px solid #425563;padding:14px}article[hidden]{display:none}h2{font-size:17px}.pair{display:flex;background:#59646b;align-items:center;min-height:210px}figure{width:50%;margin:0;text-align:center}.original{image-rendering:pixelated;width:160px;height:160px;object-fit:contain}.model{width:100%;max-height:210px;object-fit:contain}figcaption{font-size:12px;padding:8px}p{font-size:13px;line-height:1.5}.status{color:#ffd18a}a{color:#9dd7ff}</style>
<header><h1>SIM model art index · 2026-09-27</h1>
<p>Original pixels decoded from each supplied ROM; production models rendered with the existing D32 audit tool. Original previews are enlarged independently for inspection; they are not a scale match. Model panels share one world scale. Early / late means the act-completion character bank, not house tier. These are resource reconstructions, not proof that every combination occurs in play.</p>
<p>The selected ROM shows its native artwork. The gallery pairs each release with its corresponding 3D variant, including the Japanese eye and Marahna stilt houses. In game, pyramid decoration is selectable independently of gameplay region. See <a href="art-index.tsv">the full provenance index</a> and <a href="manifest.json">ROM hashes</a>.</p>
<label>Release <select id="release">OPTIONS</select></label><label>Art bank <select id="bank"><option>early</option><option selected>late</option></select></label><label>Find <input id="search" type="search" placeholder="pyramid, Northwall, bridge…"></label><p id="count"></p></header>
<main>CARDS</main><section><h2>Supplementary construction states</h2><p>Additional production model checks, without changing the historical model numbers. These panels do not supply native-art provenance.</p><div class="pair">STATES</div></section><script>
const release=document.querySelector('#release'),bank=document.querySelector('#bank'),search=document.querySelector('#search');
function filter(){let n=0;document.querySelectorAll('article').forEach(a=>{a.hidden=a.dataset.release!==release.value||a.dataset.bank!==bank.value||!a.textContent.toLowerCase().includes(search.value.toLowerCase());if(!a.hidden)n++});document.querySelector('#count').textContent=n+' models shown';}
[release,bank,search].forEach(e=>e.addEventListener('input',filter));filter();
</script></html>'''
    (out / "index.html").write_text(page.replace("OPTIONS", options).replace("CARDS", "\n".join(cards)).replace("STATES", state_cards))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renders", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--rom-dir", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "images").mkdir(exist_ok=True)
    with (args.renders / "manifest.tsv").open() as stream:
        models = list(csv.DictReader(stream, delimiter="\t"))
    with (args.renders / "regional-manifest.tsv").open() as stream:
        variants = list(csv.DictReader(stream, delimiter="\t"))
    for variant in variants:
        Image.open(args.renders / variant["file"]).save(
            args.out / "images" / (Path(variant["file"]).stem + ".png"))
    roms = {r: (args.rom_dir / f).read_bytes() for r, f in RELEASES.items()
            if (args.rom_dir / f).exists()}
    if "US" not in roms:
        parser.error("ar.sfc is required for shared composition identities")
    scripts = {r: {(m, t): commands for m, t, commands in iter_script(data)}
               for r, data in roms.items()}
    draws = parse_draw_lists(roms["US"])
    families = {r: house_families(data, r) for r, data in roms.items()}
    # Prove that the shared composition bytes exist in every supplied donor.
    # Individual draw-list indices are US labels, not Japanese ROM addresses.
    for index in (73, 74, 75, 76, 77, 78, 88, 89):
        item = draws[index]
        raw = bytes([len(item.commands)]) + bytes(
            value for cell in item.commands
            for value in (cell.dx, cell.dy, cell.metatile))
        for release, data in roms.items():
            if raw not in data:
                raise ValueError(f"{release} lacks shared composition {index}")

    @lru_cache(None)
    def library(release, town, bank):
        return RomArt(roms[release], scripts[release], town, bank)

    rows = []
    for number, model in enumerate(models, 1):
        filename = model["file"]
        model_image = "images/" + Path(filename).stem + ".png"
        Image.open(args.renders / filename).save(args.out / model_image)
        for release in roms:
            town, atlas, cells, identity = source_spec(filename, draws, families[release])
            for bank, bank_name in enumerate(("early", "late")):
                art = library(release, town, bank)
                original = compose(art, atlas, cells)
                digest = hashlib.sha256(original.tobytes()).hexdigest()
                original_image = f"images/source-{digest}.png"
                original.save(args.out / original_image)
                selected_model = model_image
                status = "Shared 3D silhouette; original art-bank colors indexed for comparison."
                if filename == "clearable-shrub.bmp":
                    status = "Rounded burnable crown and grouped leaf colours; closed underside now retains outward lighting normals."
                if filename.startswith("tree-town-"):
                    status = "Taller evergreen with scalloped branch tiers, pointed leader, brighter needles and profile-matched shadows."
                if filename.startswith("broad-tree-"):
                    status = "Three leaf clusters on visible branches; complete coarse silhouette at Low."
                if filename == "northwall-story-tree.bmp":
                    status = "Landmark budget: four joined snow-laden crowns, including the central bulb, with a forked trunk and spreading roots."
                if filename.startswith("house-"):
                    status = "Straw huts have reed walls, overhanging thatch and bundled peaks or broad alternate crests; canvas pavilions, chimneys and stilt stairs retain their separate regional forms."
                if filename.startswith("windmill-"):
                    status = "Round tower and entrance; outward-flaring curved sails carry the native purple pinstripe."
                if filename.startswith("factory-"):
                    status = "Joined pitched U-roof, two chimneys on the right connector and three peaked dormer windows."
                if filename.startswith("construction-"):
                    status = "Building-specific construction: peaked house framing, open factory wings and progressive round windmill tower."
                if filename == "fillmore-boulder.bmp" or filename.startswith("rocky-ground-"):
                    status = "Native stone layout retained at every detail level; follows displayed terrain until clearing redraws it."
                if filename == "kasandora-pyramid.bmp":
                    if release == "Japan":
                        selected_model = "images/kasandora-pyramid-jp.png"
                        status = "Japanese eye inlay; follows resolved artwork donor, independently of gameplay region."
                    else:
                        status = "Plain Egyptian pyramid with continuous masonry faces."
                if filename == "marahna-temple.bmp":
                    status = "Separate $EF landmark: central prang, rounded side mounds, front pillars, gateposts and wrapping walls; ordinary $C2 cathedral remains independent."
                if release == "Japan" and filename.startswith("house-5-2-"):
                    selected_model = "images/" + filename.replace("house-5-2-", "house-5-2-jp-").replace(".bmp", ".png")
                    status = "Japanese developed stilt family $0E, selected from the native finished metatile."
                rows.append(dict(number=number, label=model["label"], section=model["section"],
                                 release=release, bank=bank_name, town=town, atlas=atlas,
                                 identity=identity, metatiles=" ".join(f"${t:02X}" for _, _, t in cells),
                                 definitions_file_offset=f"0x{art.sources[atlas]:06X}",
                                 characters_file_offset=f"0x{art.sources['characters']:06X}",
                                 palette_file_offset=f"0x{art.sources['palette']:06X}",
                                 house_selector_file_offset=(f"0x{HOUSE_TABLE_OFFSETS[release]:06X}"
                                                             if filename.startswith("house-") else ""),
                                 original_pixel_sha256=digest, original_image=original_image,
                                 model_image=selected_model, model_status=status))
    with (args.out / "art-index.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    (args.out / "manifest.json").write_text(json.dumps({
        "rom_sha256": {r: hashlib.sha256(b).hexdigest() for r, b in roms.items()},
        "models": len(models), "regional_models": len(variants), "source_rows": len(rows),
        "house_family_tables": families,
        "house_table_file_offsets": HOUSE_TABLE_OFFSETS,
        "source_method": "ROM asset scripts, shared metatile compositions, original 4bpp pixels",
    }, indent=2) + "\n")
    states_path = args.renders / "state-manifest.tsv"
    states = list(csv.DictReader(states_path.open(), delimiter="\t")) if states_path.exists() else []
    for row in states:
        Image.open(args.renders / row["file"]).save(args.out / "images" / (Path(row["file"]).stem + ".png"))
    write_gallery(args.out, rows, roms, states)
    print(args.out / "index.html")


if __name__ == "__main__":
    main()

package localizationkit

import (
	"bytes"
	"fmt"
	"slices"
)

var endingPageSignature = instructionBytes("a3018bc22029ff000a0a0aeb18690040aaa000b0a9ff07547f7ee220abe6")

func (d *Decoder) titleGraphicalSurface(entries []assetEntry) (IRObject, error) {
	var title *assetEntry
	for i := range entries {
		if entries[i].mode == 0 && entries[i].submode == 0 {
			if title != nil {
				return nil, fmt.Errorf("duplicate title asset entry")
			}
			title = &entries[i]
		}
	}
	if title == nil || title.start != assetScriptBase+3 {
		return nil, fmt.Errorf("title asset entry is missing or no longer first")
	}
	raw, err := d.span(title.start, title.end-title.start)
	if err != nil {
		return nil, err
	}
	return IRObject{"id": "title.logo_and_publisher", "classification": "graphical_text_full_surface", "replacement_path": "future_graphics_surface_replacement",
		"runtime_selector": IRObject{"mode_18": "$00", "submode_19": "$00"}, "surface": "mode7_bg1", "settled_capture_rect_xyxy": []int{11, 27, 248, 122},
		"structured_menu_text_exclusion": "screen_y_greater_than_or_equal_140",
		"asset_scene":                    IRObject{"source_file_offset": fileOffset(title.start), "byte_count": len(raw), "command_count": len(title.commands), "raw_sha256": rawSHA256(raw)},
		"source_asset_status":            "scene_fingerprinted_individual_art_blob_not_claimed", "redistribution": "fingerprint_only_rom_art_not_redistributable"}, nil
}

func (d *Decoder) endingGraphicalSurface() (IRObject, error) {
	entries, err := d.assetScript()
	if err != nil {
		return nil, err
	}
	assets, err := d.endingAssets(entries)
	if err != nil {
		return nil, err
	}
	matches := scanPattern(d.rom, endingPageSignature)
	if len(matches) != 1 {
		return nil, fmt.Errorf("expected one ending page copy, found %d", len(matches))
	}
	signatureOffset, err := pc24Offset(matches[0])
	if err != nil {
		return nil, err
	}
	routineOffset := signatureOffset - 20
	raw, err := d.span(routineOffset, 5)
	if err != nil || !bytes.Equal(raw, instructionBytes("48b00ea90f")) {
		return nil, fmt.Errorf("ending page-copy entry changed")
	}
	fade, err := d.span(routineOffset+5, 12)
	if err != nil || fade[0] != 0x20 || !bytes.Equal(fade[:3], fade[3:6]) ||
		!bytes.Equal(fade[6:], instructionBytes("8d00213a10f4")) {
		return nil, fmt.Errorf("ending fade-out changed")
	}
	fadeIn, err := d.span(routineOffset+0x33, 16)
	if err != nil || !bytes.Equal(fadeIn[:2], instructionBytes("a900")) ||
		!bytes.Equal(fadeIn[2:8], fade[:6]) || !bytes.Equal(fadeIn[8:], instructionBytes("8d00211ac91090f2")) {
		return nil, fmt.Errorf("ending fade-in changed")
	}
	hold, err := d.span(routineOffset+0x43, 18)
	if err != nil || !bytes.Equal(hold[:9], instructionBytes("a301c913f00ac220a9")) ||
		hold[11] != 0x20 || !bytes.Equal(hold[14:], instructionBytes("e2206860")) {
		return nil, fmt.Errorf("ending page hold changed")
	}
	waitOffset, err := pc24Offset((offsetPC(routineOffset) & 0xff0000) | word(hold[12:]))
	if err != nil {
		return nil, err
	}
	wait, err := d.span(waitOffset, 7)
	if err != nil || !bytes.Equal(wait[:3], fade[:3]) || !bytes.Equal(wait[3:], instructionBytes("3ad0fa60")) {
		return nil, fmt.Errorf("ending frame-counted hold changed")
	}
	routine := offsetPC(routineOffset)
	calls, err := d.callsTo(routine, "jsr", true)
	if err != nil {
		return nil, err
	}
	if len(calls) != 5 {
		return nil, fmt.Errorf("expected five ending page callers, found %d", len(calls))
	}
	immediate := []IRObject{}
	indices := []int{}
	for _, call := range calls {
		offset, err := pc24Offset(call)
		if err != nil {
			return nil, err
		}
		prior, err := d.span(offset-3, 3)
		if err == nil && prior[0] == 0xa9 && (prior[2] == 0x18 || prior[2] == 0x38) {
			carry := "clear"
			if prior[2] == 0x38 {
				carry = "set"
			}
			indices = append(indices, int(prior[1]))
			immediate = append(immediate, IRObject{"call_site": pcString(call), "page_index": int(prior[1]), "carry": carry})
		}
	}
	if len(indices) != 4 || indices[0] != 0 || indices[2] != 18 || indices[3] != 19 {
		return nil, fmt.Errorf("ending immediate page selectors changed: %v", indices)
	}
	offset, err := pc24Offset(calls[1])
	if err != nil {
		return nil, err
	}
	tail, err := d.span(offset+3, 4)
	if err != nil || tail[0] != 0x1a || tail[1] != 0xc9 || tail[3] != 0x90 || (tail[2] != 16 && tail[2] != 17) {
		return nil, fmt.Errorf("ending sequential page bound changed")
	}
	stop := int(tail[2])
	active := slices.Clone(indices)
	for index := 0; index < stop; index++ {
		active = append(active, index)
	}
	slices.Sort(active)
	active = slices.Compact(active)
	pageCount := active[len(active)-1] + 1
	dormant := []int{}
	for index := 0; index < pageCount; index++ {
		if !slices.Contains(active, index) {
			dormant = append(dormant, index)
		}
	}
	mvn := signatureOffset + bytes.Index(endingPageSignature, instructionBytes("547f7e"))
	return IRObject{"id": "ending.credits.graphical_pages", "classification": "graphical_text_paged_surface", "replacement_path": "editable_credits_pages_with_native_artwork_exclusions",
		"runtime_selector": IRObject{"mode_18": "$08"},
		"copy_routine":     IRObject{"entry_pc24": pcString(routine), "mvn_site_pc24": cursorAddress(mvn), "signature_sha256": rawSHA256(endingPageSignature), "caller_count": len(calls), "call_sites": pcStrings(calls)},
		"page_abi": IRObject{"page_byte_count": 0x800, "sequential_page_stop_exclusive": stop, "immediate_calls": immediate, "active_page_indices": active, "dormant_page_indices": dormant,
			"addressable_page_count": pageCount, "source_wram_range": fmt.Sprintf("$7E:4000-$7E:%04X", 0x4000+pageCount*0x800-1), "destination_wram_range": "$7F:B000-$7F:B7FF"},
		"source_asset_status": "asset_script_source_verified", "producer": assets.evidence(),
		"timing":         IRObject{"fade_out_steps": 16, "fade_in_steps": 16, "frames_per_step": 2, "hold_frames": word(hold[9:]), "hold_exempt_page_index": 19},
		"redistribution": "structural_provenance_only_no_retail_pages"}, nil
}

func (d *Decoder) graphicalCensus(census *NativeDestinationCensus, entries []assetEntry) (IRObject, error) {
	resources := []IRObject{{"id": "font.dialog.native", "classification": "regional_graphical_text_font", "replacement_path": "native_or_enhanced_runtime_font", "source": census.Font}}
	enemy := []IRObject{}
	for _, row := range irRows(census.BG3Buffer, "outside_classifications") {
		if row["coverage_class"] == "graphical_text" && row["semantic_id"] == "action.hud.enemy_label" {
			enemy = append(enemy, row)
		}
	}
	longEnemy := 0
	templates := []IRObject{}
	for _, row := range irRows(census.BG3Long, "sites") {
		if row["coverage_class"] != "graphical_text" {
			continue
		}
		if row["semantic_id"] == "action.hud.enemy_label" {
			longEnemy++
		}
		if row["classification"] == "classified_graphical_text_template" {
			templates = append(templates, row)
		}
	}
	if len(enemy) != 1 || longEnemy != 1 {
		return nil, fmt.Errorf("graphical ENEMY source changed")
	}
	row := IRObject{"id": enemy[0]["semantic_id"], "classification": "graphical_text_tile_strip", "replacement_path": "enhanced_text_or_native_tile_strip"}
	for _, key := range []string{"write_site", "destination_range", "tile_word_start", "tile_count"} {
		row[key] = enemy[0][key]
	}
	resources = append(resources, row)
	expected := map[string]bool{}
	for _, regions := range destinationFacts.HUDRegions {
		for _, region := range regions {
			expected[region.ID] = true
		}
	}
	if len(templates) != len(destinationFacts.HUDRegions) {
		return nil, fmt.Errorf("graphical HUD template count changed")
	}
	found := map[string]bool{}
	for _, template := range templates {
		for _, region := range irRows(template, "graphical_text_regions") {
			id := irString(region, "semantic_id")
			if found[id] || !expected[id] {
				return nil, fmt.Errorf("duplicate/unknown graphical HUD region %q", id)
			}
			found[id] = true
			resources = append(resources, IRObject{"id": id, "classification": "graphical_text_tilemap_region", "replacement_path": "enhanced_text_or_native_tilemap_region",
				"source_tilemap": template["source_tilemap"], "destination_range": template["destination_range"], "word_indices": region["word_indices"], "font_tile_ids": region["font_tile_ids"]})
		}
	}
	if len(found) != len(expected) {
		return nil, fmt.Errorf("missing graphical HUD identities")
	}
	title, err := d.titleGraphicalSurface(entries)
	if err != nil {
		return nil, err
	}
	ending, err := d.endingGraphicalSurface()
	if err != nil {
		return nil, err
	}
	resources = append(resources, title, ending)
	audits := IRObject{"unclassified_base_bg3_writes": census.BG3Buffer["outside_unclassified_count"], "unclassified_direct_long_bg3_writes": census.BG3Long["unclassified_count"],
		"unclassified_direct_vram_paths": census.VRAM["unclassified_path_count"], "unclassified_dma_launches": census.DMA["unclassified_count"],
		"unclassified_vram_descriptor_families": census.Descriptor["unclassified_family_count"], "unclassified_indirect_write_sites": census.Indirect["unclassified_count"]}
	complete := census.ConsumerComplete && census.Font["status"] == "asset_script_source_verified" && len(resources) == 11
	for _, value := range audits {
		if value != 0 {
			complete = false
		}
	}
	status := "incomplete_language_bearing_graphical_surface_classification"
	if complete {
		status = "complete_language_bearing_graphical_surface_classification"
	}
	return IRObject{"status": status, "complete": complete, "scope": "whole_game_language_bearing_graphical_surfaces",
		"claim":          "All language-bearing graphical surfaces are owned by a stable replacement class; only structured text is author-editable.",
		"resource_count": len(resources), "font_resource_count": 1, "tile_strip_or_tilemap_region_count": 8, "full_surface_count": 2, "path_audits": audits, "resources": resources,
		"boundaries": []string{"Non-language regional art and gameplay graphics belong to the future regional-graphics registry, not language extraction.",
			"Title logo and copyright pages remain artwork; credits lettering has a separate editable Unicode composition decoder.",
			"Credits use the final 08/01 asset entry's distinct alphabet, palette and twenty page maps; not the dialogue alphabet."}}, nil
}

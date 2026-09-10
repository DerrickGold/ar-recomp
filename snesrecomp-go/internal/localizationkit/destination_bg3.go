package localizationkit

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"slices"
)

func rawSHA256(data []byte) string { return fmt.Sprintf("%x", sha256.Sum256(data)) }

func (d *Decoder) outsideBG3(site int, role string) (IRObject, error) {
	offset, err := pc24Offset(site)
	if err != nil {
		return nil, err
	}
	expect := func(delta int, hex string) bool {
		pattern := instructionBytes(hex)
		actual, err := d.span(offset+delta, len(pattern))
		return err == nil && bytes.Equal(actual, pattern)
	}
	if !expect(0, "9f00b07f") {
		return nil, fmt.Errorf("outside BG3 store changed at %s", pcString(site))
	}
	row := IRObject{"write_site": pcString(site), "role": role, "classification": "verified_non_language_maintenance", "language_bearing_candidate": false}
	valid := false
	switch role {
	case "status_strip_clear":
		valid = expect(-12, "a90000") && expect(-3, "a2c000") && expect(4, "e8e8e00001d0f5")
		row["destination_range"], row["value"] = "$7F:B0C0-$7F:B0FF", "$0000"
	case "status_strip_sequential_tiles":
		valid = expect(-6, "a2c000") && expect(-3, "a9") && expect(4, "1ae8e8e0cc00d0f4")
		if valid {
			raw, err := d.span(offset-2, 2)
			if err != nil {
				return nil, err
			}
			row["tile_word_start"] = localString(word(raw))
		}
		row["classification"], row["coverage_class"] = "classified_graphical_text", "graphical_text"
		row["semantic_id"], row["destination_range"], row["tile_count"] = "action.hud.ready", "$7F:B0C0-$7F:B0CB", 6
	case "dialogue_surface_clear":
		valid = expect(-6, "a90020a20001") && expect(4, "e8e8e00008d0f5")
		row["destination_range"], row["value"] = "$7F:B100-$7F:B7FF", "$2000"
	case "general_surface_clear", "city_pause_surface_clear":
		valid = expect(-6, "a90020a20000") && expect(4, "e8e8e00007d0f5")
		row["destination_range"], row["value"] = "$7F:B000-$7F:B6FF", "$2000"
	default:
		return nil, fmt.Errorf("unknown outside BG3 role %q", role)
	}
	if !valid {
		return nil, fmt.Errorf("%s loop changed at %s", role, pcString(site))
	}
	return row, nil
}

func (d *Decoder) hudTemplate(site int, role string, p destinationProfile) (IRObject, error) {
	regions, ok := destinationFacts.HUDRegions[role]
	if !ok {
		return nil, fmt.Errorf("unknown HUD template %q", role)
	}
	offset, err := pc24Offset(site)
	if err != nil {
		return nil, err
	}
	raw, err := d.span(offset-7, 18)
	if err != nil || raw[0] != 0xbf || !bytes.Equal(raw[4:], instructionBytes("0900209f40b07fe8e8e08000d0ee")) {
		return nil, fmt.Errorf("%s copy changed at %s", role, pcString(site))
	}
	source := int(raw[1]) | int(raw[2])<<8 | int(raw[3])<<16
	if expected, exists := p.HUDSources[role]; exists && source != expected {
		return nil, fmt.Errorf("%s template source changed", role)
	}
	data, err := d.pcSpan(source, 0x80)
	if err != nil {
		return nil, err
	}
	rows := []IRObject{}
	classified := map[int]bool{}
	for _, region := range regions {
		indices := slices.Clone(region.Indices)
		if region.ID == "sim_sky.hud.context_label" {
			if p.ContextWords <= 0 || p.ContextWords > 64 {
				return nil, fmt.Errorf("unprofiled HUD context width")
			}
			indices = make([]int, p.ContextWords)
			for i := range indices {
				indices[i] = i
			}
		}
		tiles := []string{}
		inked := false
		for _, index := range indices {
			if index < 0 || index >= 64 {
				return nil, fmt.Errorf("HUD region outside template")
			}
			value := word(data[index*2:]) & 0x3ff
			inked = inked || value != 0
			tiles = append(tiles, fmt.Sprintf("$%03X", value))
			classified[index] = true
		}
		if !inked {
			return nil, fmt.Errorf("%s graphical region is empty", region.ID)
		}
		rows = append(rows, IRObject{"semantic_id": region.ID, "classification": "graphical_text", "word_indices": indices, "font_tile_ids": tiles})
	}
	return IRObject{"classification": "classified_graphical_text_template", "coverage_class": "graphical_text", "language_bearing_candidate": false,
		"source_tilemap":    IRObject{"pc24": pcString(source), "byte_count": len(data), "raw_sha256": rawSHA256(data), "dimensions_cells": []int{32, 2}},
		"destination_range": "$7F:B040-$7F:B0BF", "graphical_text_regions": rows, "graphical_text_region_count": len(rows),
		"graphical_text_word_count": len(classified), "other_template_word_count": 64 - len(classified), "other_words_classification": "dynamic_placeholder_or_non_language_decoration"}, nil
}

func scanLongBG3(rom []byte) []IRObject {
	rows := []IRObject{}
	for offset := 0; offset+4 <= len(rom); offset++ {
		opcode := rom[offset]
		if opcode != 0x8f && opcode != 0x9f {
			continue
		}
		destination := int(rom[offset+1]) | int(rom[offset+2])<<8 | int(rom[offset+3])<<16
		if destination < 0x7fb000 || destination > 0x7fbfff {
			continue
		}
		name := "sta_long"
		if opcode == 0x9f {
			name = "sta_long_x"
		}
		rows = append(rows, IRObject{"write_site": pcString(offsetPC(offset)), "opcode": name, "destination": pcString(destination)})
	}
	return rows
}

func (d *Decoder) bg3Destinations(p destinationProfile, s sourceProfile) (IRObject, IRObject, error) {
	owner := func(site int) string {
		if site >= s.InteractiveEntry && site < s.InteractiveEnd {
			return "interactive_dialogue_and_helpers"
		}
		if site >= s.ComposerEntry && site < s.ComposerEnd {
			return "fixed_text_composer_and_helpers"
		}
		return "outside_known_text_consumers"
	}
	sites := scanPattern(d.rom, instructionBytes("9f00b07f"))
	if len(sites) != p.BaseWriteCount {
		return nil, nil, fmt.Errorf("BG3 base store count changed")
	}
	writeRows, outside := []IRObject{}, []IRObject{}
	baseClasses := map[string]IRObject{}
	for _, site := range sites {
		name := owner(site)
		row := IRObject{"write_site": pcString(site), "owner": name}
		if name == "outside_known_text_consumers" {
			if len(outside) >= len(p.OutsideRoles) {
				return nil, nil, fmt.Errorf("unexpected outside BG3 store")
			}
			classification, err := d.outsideBG3(site, p.OutsideRoles[len(outside)])
			if err != nil {
				return nil, nil, err
			}
			for _, key := range []string{"role", "classification", "language_bearing_candidate"} {
				row[key] = classification[key]
			}
			outside = append(outside, classification)
			baseClasses[pcString(site)] = classification
		}
		writeRows = append(writeRows, row)
	}
	if len(outside) != 5 || len(outside) != len(p.OutsideRoles) {
		return nil, nil, fmt.Errorf("outside BG3 role count changed")
	}
	baseGraphical := 0
	for _, row := range outside {
		if row["coverage_class"] == "graphical_text" {
			baseGraphical++
		}
	}
	base := IRObject{"destination_base": "$7F:B000", "direct_write_site_count": len(sites), "outside_known_text_consumer_count": len(outside),
		"outside_write_classification_status": "all_profiled_sites_classified", "outside_unclassified_count": 0, "outside_graphical_candidate_count": 0,
		"outside_graphical_text_source_count": baseGraphical, "outside_classifications": outside, "sites": writeRows}
	auxiliary := map[int]string{}
	for _, item := range p.Auxiliary {
		if _, exists := auxiliary[item.Address]; exists {
			return nil, nil, fmt.Errorf("duplicate auxiliary BG3 address")
		}
		auxiliary[item.Address] = item.ID
	}
	longRows := scanLongBG3(d.rom)
	if len(longRows) != p.LongWriteCount {
		return nil, nil, fmt.Errorf("long BG3 store count changed: %d != %d", len(longRows), p.LongWriteCount)
	}
	noncode, graphical, regionCount := 0, 0, 0
	for _, row := range longRows {
		site, err := parsePC(irString(row, "write_site"))
		if err != nil {
			return nil, nil, err
		}
		name := owner(site)
		row["language_bearing_candidate"] = false
		switch {
		case slices.Contains(p.Noncode, site):
			row["owner"], row["classification"] = "non_executable_rom_data", "decoded_cfg_rejected_instruction"
			noncode++
		case name != "outside_known_text_consumers":
			row["owner"], row["classification"] = name, "known_text_consumer"
		case baseClasses[pcString(site)] != nil:
			row["owner"] = "profiled_auxiliary_surface_write"
			for _, key := range []string{"role", "classification", "language_bearing_candidate", "coverage_class", "semantic_id", "destination_range", "tile_word_start", "tile_count"} {
				if value, ok := baseClasses[pcString(site)][key]; ok {
					row[key] = value
				}
			}
		case auxiliary[site] != "":
			role := auxiliary[site]
			row["owner"], row["role"] = "profiled_auxiliary_surface_write", role
			if _, ok := destinationFacts.HUDRegions[role]; ok {
				classified, err := d.hudTemplate(site, role, p)
				if err != nil {
					return nil, nil, err
				}
				for key, value := range classified {
					row[key] = value
				}
			} else if role == "manual_dialogue_attribute" {
				row["classification"] = "presentation_attribute_write"
			} else {
				return nil, nil, fmt.Errorf("unknown auxiliary BG3 role %q", role)
			}
		default:
			return nil, nil, fmt.Errorf("unclassified long BG3 write at %s", pcString(site))
		}
		if row["coverage_class"] == "graphical_text" {
			graphical++
			if count, ok := row["graphical_text_region_count"].(int); ok {
				regionCount += count
			} else {
				regionCount++
			}
		}
	}
	long := IRObject{"destination_range": "$7F:B000-$7F:BFFF", "opcodes": []string{"STA long", "STA long,X"}, "status": "all_raw_candidates_classified",
		"raw_candidate_count": len(longRows), "decoded_instruction_count": len(longRows) - noncode, "noncode_pattern_count": noncode, "unclassified_count": 0,
		"graphical_candidate_count": 0, "graphical_text_source_count": graphical, "graphical_text_region_count": regionCount, "sites": longRows}
	return base, long, nil
}

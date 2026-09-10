package localization

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"slices"
)

func (d *Decoder) rejectedDestinations(signatures []destinationSignature, classification string) ([]IRObject, error) {
	rows := []IRObject{}
	for _, item := range signatures {
		expected, err := hex.DecodeString(item.Hex)
		if err != nil || len(expected) == 0 {
			return nil, fmt.Errorf("invalid rejected-instruction signature")
		}
		if err := d.expectPC(item.Address, expected, "rejected destination decode"); err != nil {
			return nil, err
		}
		// Python's bytes.hex(' ') uses lowercase, unlike text IR's native hex.
		rows = append(rows, IRObject{"site": pcString(item.Address), "bytes_hex": fmt.Sprintf("% x", expected), "classification": classification})
	}
	return rows, nil
}

func (d *Decoder) decodedBG3Evidence(p destinationProfile) (IRObject, error) {
	rows, err := d.rejectedDestinations(p.RangeRejected, "non_executable_or_overlapping_decode")
	if err != nil {
		return nil, err
	}
	if p.RangeLongCount+len(rows) != p.RangeCount {
		return nil, fmt.Errorf("inconsistent decoded BG3 evidence count")
	}
	return IRObject{"status": "all_address_bearing_candidates_classified", "method": "snesbuild xref v2 decoded $B000-$BFFF WRAM-mirror address-range audit, pinned to the exact ROM hash",
		"decoded_reference_count": p.RangeCount, "decoded_direct_long_reference_count": p.RangeLongCount, "rejected_nonlong_candidate_count": len(rows), "decoded_nonlong_bg3_write_count": 0,
		"rejected_nonlong_candidates": rows, "limitation": "Computed pointer destinations do not encode $B000 in the instruction operand and require separate consumer/data-flow evidence."}, nil
}

func (d *Decoder) directVRAMPaths(p destinationProfile) (IRObject, error) {
	paths := []IRObject{}
	seen := map[int]bool{}
	for _, path := range p.VRAMPaths {
		detail, ok := destinationFacts.VRAMDetails[path.ID]
		if !ok {
			return nil, fmt.Errorf("unknown direct VRAM path %q", path.ID)
		}
		rows := []IRObject{}
		for _, site := range path.Sites {
			if seen[site] {
				return nil, fmt.Errorf("duplicate VRAM write at %s", pcString(site))
			}
			seen[site] = true
			raw, err := d.pcSpan(site, 3)
			if err != nil {
				return nil, err
			}
			instruction, width := "", 16
			switch {
			case bytes.Equal(raw, instructionBytes("8d1821")):
				instruction = "sta_abs_vmdatal"
			case bytes.Equal(raw, instructionBytes("8d1921")):
				instruction = "sta_abs_vmdatah"
			case bytes.Equal(raw, instructionBytes("9c1821")):
				instruction = "stz_abs_vmdatal"
			default:
				raw, err = d.pcSpan(site, 4)
				if err == nil && bytes.Equal(raw, instructionBytes("8f182100")) {
					instruction, width = "sta_long", 24
				}
			}
			if instruction == "" {
				return nil, fmt.Errorf("direct VRAM store changed at %s", pcString(site))
			}
			rows = append(rows, IRObject{"write_site": pcString(site), "instruction": instruction, "operand_width": width, "bytes_hex": fmt.Sprintf("% x", raw)})
		}
		row := cloneIR(detail).(IRObject)
		row["id"], row["write_site_count"], row["write_sites"] = path.ID, len(rows), rows
		paths = append(paths, row)
	}
	if len(seen) != 9 {
		return nil, fmt.Errorf("expected nine direct VRAM writes, found %d", len(seen))
	}
	return IRObject{"status": "all_decoded_direct_paths_classified", "method": "snesbuild xref v2 address-range census, with exact instruction bytes revalidated during extraction",
		"destination_ports": []string{"$2118", "$2119", "$00:2118", "$00:2119"}, "decoded_write_site_count": len(seen), "unclassified_path_count": 0, "paths": paths}, nil
}

func (d *Decoder) dmaLaunches(p destinationProfile) (IRObject, error) {
	roles := slices.Clone(destinationFacts.DMARoles)
	if len(p.DMA) == len(roles)-1 {
		roles = slices.DeleteFunc(roles, func(role string) bool { return role == "world_effect" })
	}
	if len(p.DMA) != len(roles) {
		return nil, fmt.Errorf("DMA launch role count changed")
	}
	rows := []IRObject{}
	seen := map[int]bool{}
	textTransfers, descriptors := 0, 0
	for index, item := range p.DMA {
		if item.ID != roles[index] || seen[item.Address] {
			return nil, fmt.Errorf("DMA role order or site uniqueness changed")
		}
		seen[item.Address] = true
		expected, name := "8d0b42", "sta_abs_mdasen"
		if item.ID == "dma_disable" {
			expected, name = "9c0b42", "stz_abs_mdasen"
		}
		if item.ID == "tilemap_record" || item.ID == "cgram_flicker" {
			expected, name = "8e0b42", "stx_abs_mdasen"
		}
		raw := instructionBytes(expected)
		if err := d.expectPC(item.Address, raw, item.ID); err != nil {
			return nil, err
		}
		detail := destinationFacts.DMADetails[item.ID]
		if len(detail) != 2 {
			return nil, fmt.Errorf("missing DMA role details")
		}
		if detail[1] == "known_text_staging_transfer" {
			textTransfers++
		}
		if item.ID == "generic_vram_descriptor" {
			descriptors++
		}
		rows = append(rows, IRObject{"id": item.ID, "write_site": pcString(item.Address), "instruction": name, "bytes_hex": fmt.Sprintf("% x", raw),
			"transfer": detail[0], "classification": detail[1], "language_bearing_candidate": false})
	}
	return IRObject{"status": "all_decoded_dma_launches_classified", "method": "snesbuild xref v2 $420B write census, with exact instruction bytes revalidated during extraction",
		"decoded_write_site_count": len(rows), "bg3_known_text_transfer_count": textTransfers, "generic_vram_descriptor_transfer_count": descriptors, "unclassified_count": 0, "sites": rows}, nil
}

func (d *Decoder) vramDescriptor(p destinationProfile) (IRObject, error) {
	patterns, ok := destinationFacts.DescriptorPatterns[p.DescriptorABI]
	if !ok || len(patterns) == 0 {
		return nil, fmt.Errorf("unknown VRAM descriptor ABI %q", p.DescriptorABI)
	}
	rows := []IRObject{}
	for _, item := range patterns {
		pattern, err := hex.DecodeString(item.Hex)
		if err != nil || len(pattern) == 0 {
			return nil, fmt.Errorf("invalid descriptor signature")
		}
		matches := scanPattern(d.rom, pattern)
		if len(matches) != 1 {
			return nil, fmt.Errorf("%s: expected one producer signature, found %d", item.ID, len(matches))
		}
		row := cloneIR(destinationFacts.DescriptorDetails[item.ID]).(IRObject)
		row["id"], row["site_pc24"] = item.ID, pcString(matches[0])
		rows = append(rows, row)
	}
	return IRObject{"status": "all_structural_producer_families_classified", "method": "exact producer/control-dependency signatures across the supported ROM hash",
		"abi": p.DescriptorABI, "slots": cloneIR(destinationFacts.DescriptorABIs[p.DescriptorABI]), "producer_or_dependency_family_count": len(rows),
		"unclassified_family_count": 0, "language_bearing_candidate_count": 0, "families": rows,
		"limitation": "The signatures cover the structurally identified descriptor producers and dependencies; they do not replace the separate computed-pointer write audit."}, nil
}

func (d *Decoder) indirectDestinations(p destinationProfile) (IRObject, error) {
	rows := []IRObject{}
	seen := map[int]bool{}
	for _, path := range p.Indirect {
		detail, ok := destinationFacts.IndirectDetails[path.ID]
		if !ok {
			return nil, fmt.Errorf("unknown indirect-write family %q", path.ID)
		}
		expected, err := hex.DecodeString(path.Hex)
		if err != nil || len(expected) == 0 {
			return nil, fmt.Errorf("invalid indirect instruction signature")
		}
		sites := []IRObject{}
		for _, site := range path.Sites {
			if seen[site] {
				return nil, fmt.Errorf("duplicate indirect write at %s", pcString(site))
			}
			seen[site] = true
			if err := d.expectPC(site, expected, path.ID); err != nil {
				return nil, err
			}
			sites = append(sites, IRObject{"write_site": pcString(site), "bytes_hex": fmt.Sprintf("% x", expected)})
		}
		row := cloneIR(detail).(IRObject)
		row["id"], row["language_bearing_candidate"], row["write_site_count"], row["write_sites"] = path.ID, false, len(sites), sites
		rows = append(rows, row)
	}
	rejected, err := d.rejectedDestinations(p.IndirectRejected, "non_executable_or_width_confused_decode")
	if err != nil {
		return nil, err
	}
	if p.IndirectCount != nil && *p.IndirectCount != len(seen)+len(rejected) {
		return nil, fmt.Errorf("inconsistent indirect-store evidence count")
	}
	return IRObject{"status": "all_identified_pointer_destinations_classified", "method": "rooted US decoded-store census plus structurally matched and data-flow-revalidated counterparts in every exact supported regional executable",
		"family_count": len(rows), "live_write_site_count": len(seen), "rejected_decode_count": len(rejected), "decoded_reference_count": p.IndirectCount,
		"unclassified_count": 0, "language_bearing_candidate_count": 0, "families": rows, "rejected_candidates": rejected}, nil
}

package localization

import (
	"cmp"
	"fmt"
	"slices"
)

type sourceInterval struct{ start, end int }

func mergeIntervals(intervals []sourceInterval) ([]sourceInterval, error) {
	sorted := append([]sourceInterval{}, intervals...)
	slices.SortFunc(sorted, func(a, b sourceInterval) int {
		if a.start != b.start {
			return cmp.Compare(a.start, b.start)
		}
		return cmp.Compare(a.end, b.end)
	})
	merged := []sourceInterval{}
	for _, span := range sorted {
		if span.end < span.start {
			return nil, fmt.Errorf("source interval ends before it starts")
		}
		if len(merged) == 0 || span.start > merged[len(merged)-1].end {
			merged = append(merged, span)
		} else {
			merged[len(merged)-1].end = max(merged[len(merged)-1].end, span.end)
		}
	}
	return merged, nil
}
func intervalStats(intervals []sourceInterval) (int, int, error) {
	merged, err := mergeIntervals(intervals)
	total := 0
	for _, span := range merged {
		total += span.end - span.start
	}
	return len(merged), total, err
}
func (b *catalogBuilder) localOwnership(census *NativeSourceCensus, menu *NativeMenuCatalog, resolution IRObject) (IRObject, error) {
	live := map[string]bool{}
	for _, reference := range irRows(census.SourceReferenceSeeds, "references") {
		if id := irString(reference, "resolved_record_id"); id != "" {
			live[id] = true
		}
	}
	bounded := []string{"action_stage_name", "action_hud_label", "title_and_mode_menu", "sound_test_menu", "town_name", "enemy_name", "dynamic_lookup_text", "offering_text", "offering_text_native", "ending_text"}
	dormant := []string{"angel_dialogue", "angel_dialogue_native", "town_dialogue", "town_dialogue_native", "post_offering_or_ending_native", "dialogue_consumer_seed"}
	records := append(append([]*NativeMessage{}, b.messages...), menu.Segments...)
	textIntervals := []sourceInterval{}
	counts := map[string]int{}
	unclassified := []string{}
	classified := []IRObject{}
	for index, record := range records {
		textIntervals = append(textIntervals, sourceInterval{record.start, record.end})
		ownership := "unclassified"
		switch {
		case live[record.ID]:
			ownership = "live_consumer_text"
		case !recordHasLanguage(record):
			ownership = "verified_non_language_empty_record"
		case record.Classification == "typed_numeric_only":
			ownership = "verified_non_language_numeric_descriptor"
		case record.Classification == "typed_non_language_indicator":
			ownership = "verified_non_language_ui_indicator"
		case slices.Contains(bounded, record.Category):
			ownership = "bounded_consumer_catalog_text"
		case slices.Contains(dormant, record.Category):
			ownership = "dormant_or_release_variant_text"
		case index >= len(b.messages):
			ownership = "bounded_consumer_catalog_text"
		default:
			unclassified = append(unclassified, record.ID)
		}
		record.SourceOwnership = ownership
		counts[ownership]++
		classified = append(classified, IRObject{"record_id": record.ID, "source_ownership": ownership, "file_offset": record.Source.FileOffset, "end_file_offset_exclusive": record.Source.End})
	}
	metadataIntervals := []sourceInterval{}
	metadata := []IRObject{}
	add := func(id string, start, count int, classification string) error {
		if _, err := b.d.span(start, count); err != nil {
			return err
		}
		metadataIntervals = append(metadataIntervals, sourceInterval{start, start + count})
		metadata = append(metadata, IRObject{"id": id, "classification": classification, "file_offset": fileOffset(start), "byte_count": count})
		return nil
	}
	addPointer := func(id, address string, count int) error {
		offset, err := parsedOffset(address)
		if err != nil {
			return err
		}
		return add(id, offset, count*2, "language_source_pointer_table")
	}
	for _, set := range b.pointers {
		if err := add(set.ID, set.SourceTable.offset, set.SourceTable.Count*2, "language_source_pointer_table"); err != nil {
			return nil, err
		}
	}
	for index, matrix := range irRows(census.NestedHandlerSources, "matrices") {
		if err := addPointer(fmt.Sprintf("nested_handler.%d.row_table", index), irString(matrix, "row_table_pc24"), matrix["row_count"].(int)); err != nil {
			return nil, err
		}
		for _, row := range irRows(matrix, "rows") {
			if err := addPointer(fmt.Sprintf("nested_handler.%d.city.%d", index, row["city_slot"].(int)), irString(row, "source_table_pc24"), row["pointer_count"].(int)); err != nil {
				return nil, err
			}
		}
	}
	composer := census.FixedComposerSources
	for _, row := range irRows(composer, "pointer_tables") {
		if err := addPointer("fixed_composer."+irString(row, "id"), irString(row, "source_table_pc24"), row["pointer_count"].(int)); err != nil {
			return nil, err
		}
	}
	if value := composer["direct_source_table"]; value != nil {
		row := value.(IRObject)
		if err := addPointer("fixed_composer.direct_sources", irString(row, "source_table_pc24"), row["pointer_count"].(int)); err != nil {
			return nil, err
		}
	}
	for _, row := range irRows(composer, "indexed_direct_sources") {
		if err := addPointer("fixed_composer.indexed."+irString(row, "id"), irString(row, "source_table_pc24"), row["pointer_count"].(int)); err != nil {
			return nil, err
		}
	}
	for _, row := range irRows(composer, "flow_sources") {
		if descriptor := irString(row, "descriptor_pc24"); descriptor != "" {
			offset, err := parsedOffset(descriptor)
			if err != nil {
				return nil, err
			}
			if err := add("fixed_composer.flow."+irString(row, "id")+".destination", offset, 2, "fixed_composer_destination_descriptor"); err != nil {
				return nil, err
			}
		}
	}
	if b.d.profile.Encoding == "dictionary-12" {
		if err := add("dictionary", b.d.profile.Dictionary, dictionaryBytes, "language_decoder_dictionary"); err != nil {
			return nil, err
		}
	}
	textCount, textBytes, err := intervalStats(textIntervals)
	if err != nil {
		return nil, err
	}
	metadataCount, metadataBytes, err := intervalStats(metadataIntervals)
	if err != nil {
		return nil, err
	}
	_, ownedBytes, err := intervalStats(append(textIntervals, metadataIntervals...))
	if err != nil {
		return nil, err
	}
	slices.Sort(unclassified)
	consumerComplete := census.destinations != nil && census.destinations.ConsumerComplete
	complete := consumerComplete && resolution["all_current_seeds_mapped"] == true && len(unclassified) == 0
	status := "incomplete_source_ownership"
	if complete {
		status = "complete_consumer_rooted_source_ownership"
	}
	return IRObject{"status": status, "complete": complete,
		"method": "exhaustive_text_destination_consumers_then_source_closure", "raw_printable_scan_rejected": true,
		"raw_printable_scan_reason": "Dictionary tokens and direct tile codes make arbitrary ROM data decode as plausible text; byte-likeness cannot establish language ownership.",
		"proof_obligations": IRObject{"whole_game_consumer_discovery_complete": consumerComplete, "all_consumer_source_references_mapped": resolution["all_current_seeds_mapped"],
			"all_bounded_source_records_classified": len(unclassified) == 0, "graphical_language_resources_deferred_to_separate_census": true},
		"record_count": len(records), "record_counts_by_ownership": counts, "unclassified_record_count": len(unclassified), "unclassified_record_ids": unclassified,
		"consumer_reference_count": len(irRows(census.SourceReferenceSeeds, "references")), "unique_consumer_source_count": resolution["unique_source_count"], "mapped_unique_consumer_source_count": resolution["mapped_unique_source_count"],
		"text_source_interval_count": textCount, "text_source_byte_count": textBytes, "metadata_interval_count": metadataCount, "metadata_byte_count": metadataBytes,
		"owned_rom_byte_count": ownedBytes, "outside_consumer_source_ownership_byte_count": len(b.d.rom) - ownedBytes,
		"outside_ownership_classification": "not_reachable_as_language_source_by_any_censused_live_text_path; graphical assets are classified by graphical_text_census",
		"metadata":                         metadata, "records": classified}, nil
}

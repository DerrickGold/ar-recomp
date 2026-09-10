package localizationkit

import (
	"fmt"
	"slices"
	"strings"
)

func operationFindings(records []*NativeMessage) []IRObject {
	counts := map[[2]string]int{}
	for _, record := range records {
		for _, op := range record.Operations {
			name, confidence := irString(IRObject(op), "op"), irString(IRObject(op), "confidence")
			unresolved := strings.HasPrefix(confidence, "unresolved") || slices.Contains([]string{"native_control", "native_glyphs", "native_diacritic", "truncated_native_control", "truncated_composer_control", "truncated_composer_record"}, name) ||
				((name == "format_number" || name == "insert_indexed_text") && op["value"] == nil)
			if !unresolved {
				continue
			}
			if name == "" {
				name = "missing_operation"
			}
			if confidence == "" {
				confidence = "not_declared"
			}
			counts[[2]string{name, confidence}]++
		}
	}
	keys := make([][2]string, 0, len(counts))
	for key := range counts {
		keys = append(keys, key)
	}
	slices.SortFunc(keys, func(a, b [2]string) int {
		if a[0] != b[0] {
			return strings.Compare(a[0], b[0])
		}
		return strings.Compare(a[1], b[1])
	})
	rows := []IRObject{}
	for _, key := range keys {
		rows = append(rows, IRObject{"operation": key[0], "confidence": key[1], "count": counts[key]})
	}
	return rows
}

// CheckNativeCoverage evaluates fresh extraction evidence and the independent
// cross-release route union. No catalogue or active pack is mutated. A single
// ROM gets an explicit missing-cross-release blocker, not a false complete
// result. This diagnostic gate is distinct from author-format validation.
func CheckNativeCoverage(catalogs ...*NativeCatalog) ([]IRObject, error) {
	if len(catalogs) == 0 {
		return nil, fmt.Errorf("coverage requires at least one catalogue")
	}
	alignment, err := AlignNativeCatalogs(catalogs...)
	if err != nil {
		return nil, err
	}
	reports := make([]IRObject, 0, len(catalogs))
	for _, catalog := range catalogs {
		if catalog.source == nil || catalog.Destinations == nil || catalog.Menu == nil || catalog.DynamicText == nil || catalog.Ownership == nil || catalog.Graphics == nil {
			return nil, fmt.Errorf("coverage requires fresh complete catalogue evidence")
		}
		report := catalog.coverageReport(alignment)
		reports = append(reports, report)
	}
	return reports, nil
}

func (c *NativeCatalog) coverageReport(alignment *NativeRouteUnion) IRObject {
	unaligned := func(records []*NativeMessage) int {
		count := 0
		for _, record := range records {
			if !slices.Contains([]string{"callsite_verified", "logical_routes_verified", "explicit_release_variant_resource", "verified_non_language_resource"}, record.AlignmentStatus) {
				count++
			}
		}
		return count
	}
	structured, menu := operationFindings(c.Messages), operationFindings(c.Menu.Segments)
	unresolved := 0
	for _, rows := range [][]IRObject{structured, menu} {
		for _, row := range rows {
			unresolved += row["count"].(int)
		}
	}
	unalignedText, unalignedMenu := unaligned(c.Messages), unaligned(c.Menu.Segments)
	blockers := []IRObject{}
	if c.Ownership["complete"] != true {
		blockers = append(blockers, IRObject{"id": "whole_rom_language_candidate_scan", "status": "incomplete", "count": c.Ownership["unclassified_record_count"],
			"detail": "Consumer-rooted source ownership is incomplete; every live source and bounded dormant/variant record must be classified."})
	}
	if c.Graphics["complete"] != true {
		blockers = append(blockers, IRObject{"id": "graphical_text_census", "status": "incomplete", "count": c.Graphics["resource_count"],
			"detail": "Language-bearing graphical surfaces do not yet all have stable ownership and replacement classifications."})
	}
	if !alignment.Complete {
		blockers = append(blockers, IRObject{"id": "cross_release_semantic_alignment", "status": "incomplete", "count": unalignedText + unalignedMenu,
			"detail": "Logical consumer routes have not yet been joined across all five supported release profiles."})
	}
	if !c.Destinations.ConsumerComplete {
		blockers = append(blockers, IRObject{"id": "text_consumer_reference_census", "status": "in_progress",
			"detail": "Known consumer families are censused, but exact release-specific direct, DMA, and indirect destination closure is not available for this profile."})
	}
	if c.DynamicText["complete"] != true {
		blockers = append(blockers, IRObject{"id": "dynamic_text_census", "status": "incomplete", "count": c.DynamicText["unresolved_operation_count"],
			"detail": "Observed substitutions or their language-bearing lookup targets do not yet all have typed semantic ownership."})
	}
	if unresolved != 0 {
		blockers = append(blockers, IRObject{"id": "unresolved_operations_or_glyphs", "status": "incomplete", "count": unresolved,
			"detail": "Native controls/glyphs remain lossless but are not fully typed for Unicode authoring."})
	}
	counts := IRObject{"structured_messages": len(c.Messages), "logically_aligned_structured_messages": len(c.Messages) - unalignedText, "unaligned_structured_messages": unalignedText,
		"pointer_sets": len(c.PointerSets), "menu_segments": len(c.Menu.Segments), "semantically_assigned_menu_segments": len(c.Menu.Segments) - unalignedMenu, "unassigned_menu_segments": unalignedMenu, "unresolved_operation_instances": unresolved}
	copyCounts := func(from IRObject, fields map[string]string) {
		for output, input := range fields {
			counts[output] = from[input]
		}
	}
	source, destination := c.source, c.Destinations
	counts["known_text_consumers"] = len(source.Consumers)
	callCount, branchWrappers := 0, 0
	for _, consumer := range source.Consumers {
		callCount += consumer["call_site_count"].(int)
	}
	for _, wrapper := range irRows(source.DialogueForwarding, "wrappers") {
		for _, call := range irRows(wrapper, "call_sites") {
			if call["source_origin"] == "branch_join_immediate_y" {
				branchWrappers++
			}
		}
	}
	counts["known_text_consumer_call_sites"], counts["branch_join_dialogue_wrapper_calls"] = callCount, branchWrappers
	copyCounts(source.Consumers[0], map[string]string{"unresolved_interactive_source_origins": "unresolved_source_origin_call_count", "nonadjacent_interactive_source_origins": "nonadjacent_source_origin_call_count", "branch_join_interactive_source_origins": "branch_join_immediate_y_call_count"})
	copyCounts(destination.BG3Buffer, map[string]string{"direct_bg3_buffer_write_sites": "direct_write_site_count", "bg3_writes_outside_known_text_consumers": "outside_known_text_consumer_count", "unclassified_base_bg3_write_sites": "outside_unclassified_count", "base_bg3_graphical_candidates": "outside_graphical_candidate_count", "base_bg3_graphical_text_sources": "outside_graphical_text_source_count"})
	copyCounts(destination.BG3Long, map[string]string{"direct_long_bg3_write_candidates": "raw_candidate_count", "decoded_direct_long_bg3_writes": "decoded_instruction_count", "noncode_direct_long_bg3_patterns": "noncode_pattern_count", "unclassified_direct_long_bg3_writes": "unclassified_count", "direct_long_bg3_graphical_candidates": "graphical_candidate_count", "direct_long_bg3_graphical_text_sources": "graphical_text_source_count", "direct_long_bg3_graphical_text_regions": "graphical_text_region_count"})
	copyCounts(c.ReferenceResolution, map[string]string{"consumer_reference_seed_sources": "unique_source_count", "unmapped_consumer_reference_seed_sources": "unmapped_unique_source_count"})
	copyCounts(source.NestedHandlerSources, map[string]string{"nested_handler_pointer_slots": "pointer_slot_count", "nested_handler_unique_targets": "unique_target_count"})
	copyCounts(c.SeedExpansion, map[string]string{"consumer_seed_expansion_records": "added_record_count"})
	composer := source.FixedComposerSources
	copyCounts(composer, map[string]string{"fixed_composer_pointer_slots": "pointer_slot_count", "fixed_composer_unique_pointer_targets": "unique_pointer_target_count", "fixed_composer_score_report_present": "score_report_present"})
	counts["fixed_composer_verified_direct_sources"] = len(irRows(composer, "direct_sources")) + len(irRows(composer, "indexed_direct_sources"))
	indexed, flowLanguage, numeric := 0, 0, 0
	for _, row := range irRows(composer, "indexed_direct_sources") {
		indexed += row["pointer_count"].(int)
	}
	for _, row := range irRows(composer, "flow_sources") {
		if row["included_in_language_source_seeds"] == true {
			flowLanguage++
		}
	}
	if composer["numeric_only_source"] != nil {
		numeric = 1
	}
	counts["fixed_composer_indexed_direct_targets"], counts["fixed_composer_numeric_only_sources"] = indexed, numeric
	counts["fixed_composer_direct_source_candidates"] = len(irRows(composer, "direct_source_candidates"))
	counts["fixed_composer_dynamic_reports"] = len(irRows(composer, "dynamic_reports"))
	counts["fixed_composer_stateful_flow_sources"], counts["fixed_composer_stateful_language_sources"] = len(irRows(composer, "flow_sources")), flowLanguage
	dynamicCount := 0
	for _, count := range c.DynamicText["operation_counts"].(map[string]int) {
		dynamicCount += count
	}
	counts["dynamic_operation_instances"] = dynamicCount
	copyCounts(c.DynamicText, map[string]string{"typed_dynamic_values": "typed_value_count", "unresolved_dynamic_operations": "unresolved_operation_count", "dynamic_language_lookup_tables": "language_lookup_table_count", "dynamic_language_lookup_targets": "language_lookup_target_count"})
	copyCounts(destination.BG3Decoded, map[string]string{"decoded_bg3_address_range_references": "decoded_reference_count", "rejected_nonlong_bg3_address_candidates": "rejected_nonlong_candidate_count", "decoded_nonlong_bg3_writes": "decoded_nonlong_bg3_write_count"})
	copyCounts(destination.VRAM, map[string]string{"direct_vram_port_write_sites": "decoded_write_site_count", "unclassified_direct_vram_paths": "unclassified_path_count"})
	counts["direct_vram_port_paths"] = len(irRows(destination.VRAM, "paths"))
	copyCounts(destination.DMA, map[string]string{"decoded_dma_launch_sites": "decoded_write_site_count", "bg3_known_text_dma_transfers": "bg3_known_text_transfer_count", "unclassified_dma_launches": "unclassified_count"})
	copyCounts(destination.Descriptor, map[string]string{"generic_vram_descriptor_families": "producer_or_dependency_family_count", "unclassified_vram_descriptor_families": "unclassified_family_count"})
	copyCounts(destination.Indirect, map[string]string{"indirect_write_families": "family_count", "live_indirect_write_sites": "live_write_site_count", "rejected_indirect_write_decodes": "rejected_decode_count", "unclassified_indirect_write_sites": "unclassified_count"})
	counts["regional_dialog_font_sources"] = 1
	copyCounts(destination.Font, map[string]string{"regional_dialog_font_script_references": "script_reference_count", "regional_dialog_font_tiles": "tile_count"})
	copyCounts(c.Ownership, map[string]string{"classified_language_source_records": "record_count", "unclassified_language_source_records": "unclassified_record_count", "language_source_bytes": "text_source_byte_count", "language_metadata_bytes": "metadata_byte_count"})
	copyCounts(c.Graphics, map[string]string{"classified_graphical_language_resources": "resource_count", "regional_graphical_font_resources": "font_resource_count", "graphical_tile_strip_or_tilemap_regions": "tile_strip_or_tilemap_region_count", "graphical_full_surfaces": "full_surface_count"})
	status := "incomplete"
	if len(blockers) == 0 {
		status = "complete"
	}
	return IRObject{"format": "actraiser-language-extraction-coverage", "format_version": 1, "release_id": c.releaseID, "locale": c.locale, "rom_sha256": c.romSHA256,
		"scope": "whole_game_language_content", "complete": len(blockers) == 0, "status": status, "counts": counts,
		"operation_findings": IRObject{"structured_messages": structured, "menu_segments": menu}, "blockers": blockers,
		"completion_rule": "Every language-bearing candidate must be classified as semantic text, typed dynamic text, graphical text, or verified non-text; no unresolved record may remain."}
}

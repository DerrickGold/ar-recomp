package localization

import (
	"fmt"
	"slices"
	"strings"
)

// Resolution never changes the supplied discovery evidence. Callers publish the
// returned references only after any required seed expansion has succeeded.
func resolveReferences(seeds []NativeSourceReference, messages, menu []*NativeMessage) (sourceReferenceResolution, error) {
	records := append(append([]*NativeMessage{}, messages...), menu...)
	resolved := make([]NativeSourceReference, 0, len(seeds))
	unique := map[string]bool{}
	for _, reference := range seeds {
		address := reference.SourcePC24
		offset, err := parsedOffset(address)
		if err != nil {
			return sourceReferenceResolution{}, err
		}
		var closest *NativeMessage
		exactCount := 0
		for _, record := range records {
			if record.start <= offset && offset < record.end {
				if record.start == offset {
					exactCount++
				}
				if closest == nil || record.start > closest.start || (record.start == closest.start && record.ID > closest.ID) {
					closest = record
				}
			}
		}
		if exactCount > 1 {
			return sourceReferenceResolution{}, fmt.Errorf("%s: duplicate exact source records", address)
		}
		reference.NativeSourceResolution = &NativeSourceResolution{}
		if closest != nil {
			id, within := closest.ID, offset-closest.start
			reference.ResolvedRecordID = &id
			reference.SourceOffsetWithinRecord = &within
			unique[address] = true
		} else if _, exists := unique[address]; !exists {
			unique[address] = false
		}
		resolved = append(resolved, reference)
	}
	unmapped := []string{}
	for source, mapped := range unique {
		if !mapped {
			unmapped = append(unmapped, source)
		}
	}
	slices.Sort(unmapped)
	return sourceReferenceResolution{
		References: resolved,
		Summary: NativeSourceResolutionSummary{
			UniqueSourceCount: len(unique), MappedUniqueSourceCount: len(unique) - len(unmapped),
			UnmappedUniqueSourceCount: len(unmapped), AllCurrentSeedsMapped: len(unmapped) == 0,
			UnmappedSourcePC24s: unmapped,
		},
	}, nil
}

func (b *catalogBuilder) expandSeeds(census *NativeSourceCensus, menu *NativeMenuCatalog) (NativeSourceResolutionSummary, IRObject, error) {
	resolution, err := resolveReferences(census.SourceReferenceSeeds.References, b.messages, menu.Segments)
	if err != nil {
		return NativeSourceResolutionSummary{}, nil, err
	}
	added := []string{}
	kinds := map[string][]string{}
	for _, reference := range resolution.References {
		if reference.ResolvedRecordID == nil {
			address := reference.SourcePC24
			kinds[address] = append(kinds[address], reference.ReferenceKind)
		}
	}
	sources := []string{}
	for source := range kinds {
		sources = append(sources, source)
	}
	slices.Sort(sources)
	for _, source := range sources {
		for _, kind := range kinds[source] {
			if !strings.HasPrefix(kind, "interpreter_") && !strings.HasPrefix(kind, "dialogue_wrapper_") {
				return NativeSourceResolutionSummary{}, nil, fmt.Errorf("%s: non-dialogue source %s is absent from bounded catalogues", b.p.ID, source)
			}
		}
		offset, err := parsedOffset(source)
		if err != nil {
			return NativeSourceResolutionSummary{}, nil, err
		}
		message, err := b.add(offset, -1, "dialogue_consumer_seed",
			fmt.Sprintf("dialogue.consumer_seed.%02x.%04x", offset/0x8000, 0x8000+offset%0x8000), "consumer_reference_seed")
		if err != nil {
			return NativeSourceResolutionSummary{}, nil, err
		}
		if !message.Terminated {
			return NativeSourceResolutionSummary{}, nil, fmt.Errorf("consumer source %s has no invocation terminator", source)
		}
		added = append(added, message.ID)
	}
	if len(sources) > 0 {
		resolution, err = resolveReferences(census.SourceReferenceSeeds.References, b.messages, menu.Segments)
		if err != nil {
			return NativeSourceResolutionSummary{}, nil, err
		}
	}
	census.SourceReferenceSeeds.References = resolution.References
	return resolution.Summary, IRObject{"added_record_count": len(added), "added_record_ids": added}, nil
}

func (b *catalogBuilder) dynamicCensus(records []*NativeMessage) IRObject {
	operations, values := map[string]int{}, map[string]int{}
	unresolved := []IRObject{}
	for _, record := range records {
		for _, operation := range record.Operations {
			op, _ := operation["op"].(string)
			if op != "format_number" && op != "insert_indexed_text" && op != "insert_master_name" {
				continue
			}
			operations[op]++
			value := operation["value"]
			if op == "insert_master_name" {
				value = "master_name"
			}
			if value == nil {
				unresolved = append(unresolved, IRObject{"record_id": record.ID, "operation": op,
					"native_address": operation["native_address"], "args_hex": operation["args_hex"]})
			} else {
				values[value.(string)]++
			}
		}
	}
	verified := map[string]bool{}
	for _, message := range b.messages {
		if message.VerifiedSemanticID != "" {
			verified[message.VerifiedSemanticID] = true
		}
	}
	missingSet := map[string]bool{}
	targets := 0
	for _, table := range b.p.LookupTables {
		targets += len(table.SemanticIDs)
		for _, id := range table.SemanticIDs {
			if !verified[id] {
				missingSet[id] = true
			}
		}
	}
	missing := []string{}
	for id := range missingSet {
		missing = append(missing, id)
	}
	slices.Sort(missing)
	complete := len(unresolved) == 0 && len(missing) == 0
	status := "unresolved_dynamic_substitutions"
	if complete {
		status = "all_observed_substitutions_typed"
	}
	return IRObject{"status": status, "complete": complete, "operation_counts": operations, "typed_value_count": len(values),
		"typed_value_use_counts": values, "declared_number_address_count": len(b.d.profile.NumberSemantics), "declared_indexed_lookup_count": len(b.d.profile.IndexedTextSemantics),
		"language_lookup_table_count": len(b.p.LookupTables), "language_lookup_target_count": targets, "unresolved_operation_count": len(unresolved),
		"unresolved_operations": unresolved, "missing_lookup_target_semantic_ids": missing,
		"native_numeric_formats": IRObject{"ordinary": "unsigned_decimal_from_16_bit_value", "score_84": "packed_bcd_4_digits_plus_trailing_zero", "score_86": "packed_bcd_6_digits_plus_trailing_zero"}}
}

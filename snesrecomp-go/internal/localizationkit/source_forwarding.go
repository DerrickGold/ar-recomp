package localizationkit

import "fmt"

func (s *sourceDiscovery) wrapperSources() (IRObject, error) {
	p, d := s.p, s.d
	relayCalls, err := d.callsTo(p.Relay.Entry, "jsr", true)
	if err != nil {
		return nil, err
	}
	if len(relayCalls) != p.Relay.Count {
		return nil, fmt.Errorf("dialogue source relay call count changed")
	}
	relaySources := []string{}
	for _, call := range relayCalls {
		y, err := d.directY(call)
		if err != nil || y == nil {
			return nil, fmt.Errorf("unresolved dialogue relay source at %s", pcString(call))
		}
		incoming, err := d.incomingY(call)
		if err != nil {
			return nil, err
		}
		kind := "dialogue_wrapper_relay_immediate_y"
		if len(incoming) > 0 {
			kind = "dialogue_wrapper_relay_branch_join_immediate_y"
		}
		for _, candidate := range candidateYs(incoming, *y) {
			address, err := sourcePC(p.DialogueBank, candidate)
			if err != nil {
				return nil, err
			}
			relaySources = append(relaySources, pcString(address))
			if err := s.reference(address, kind, call, "via_call_site"); err != nil {
				return nil, err
			}
		}
	}
	wrappers := []IRObject{}
	for index, wrapper := range p.Wrappers {
		calls, err := d.callsTo(wrapper.Entry, wrapper.Kind, true)
		if err != nil {
			return nil, err
		}
		if len(calls) != wrapper.Count {
			return nil, fmt.Errorf("dialogue wrapper %d call count changed", index)
		}
		rows := []IRObject{}
		for _, call := range calls {
			row := IRObject{"call_site": pcString(call)}
			y, err := d.directY(call)
			if err != nil {
				return nil, err
			}
			if y != nil {
				incoming, err := d.incomingY(call)
				if err != nil {
					return nil, err
				}
				addresses := []int{}
				for _, candidate := range candidateYs(incoming, *y) {
					address, err := sourcePC(wrapper.SourceBank, candidate)
					if err != nil {
						return nil, err
					}
					addresses = append(addresses, address)
				}
				kind := "dialogue_wrapper_adjacent_immediate_y"
				if len(incoming) > 0 {
					row["source_origin"], row["source_pc24s"] = "branch_join_immediate_y", pcStrings(addresses)
					row["incoming_branches"] = incomingRows(incoming)
					kind = "dialogue_wrapper_branch_join_immediate_y"
				} else {
					row["source_origin"], row["source_pc24"] = "adjacent_immediate_y", pcString(addresses[0])
				}
				for _, address := range addresses {
					if err := s.reference(address, kind, call, "via_call_site"); err != nil {
						return nil, err
					}
				}
			} else {
				offset, err := pc24Offset(call)
				if err != nil {
					return nil, err
				}
				data, err := d.span(offset-5, 5)
				if err != nil {
					return nil, err
				}
				if data[0] == 0xbf && data[4] == 0xa8 {
					table := word(data[1:]) | int(data[3])<<16
					row["source_origin"], row["source_table_pc24"], row["pointer_count"] = "indexed_long_pointer_table", pcString(table), 7
					targets, err := d.pointerTargets(table, 7, wrapper.SourceBank)
					if err != nil {
						return nil, err
					}
					for _, target := range targets {
						if err := s.reference(target, "dialogue_wrapper_indexed_pointer_table", call, "via_call_site"); err != nil {
							return nil, err
						}
					}
				} else if call == p.Relay.Entry+2 {
					row["source_origin"], row["relay_entry_pc24"] = "forwarded_y_from_source_relay", pcString(p.Relay.Entry)
				} else {
					return nil, fmt.Errorf("unresolved dialogue wrapper source at %s", pcString(call))
				}
			}
			rows = append(rows, row)
		}
		wrappers = append(wrappers, IRObject{"id": fmt.Sprintf("dialogue_wrapper_%d", index), "entry_pc24": pcString(wrapper.Entry),
			"call_kind": wrapper.Kind, "source_bank": fmt.Sprintf("$%02X", wrapper.SourceBank), "call_site_count": len(rows), "call_sites": rows})
	}
	return IRObject{"source_bank": fmt.Sprintf("$%02X", p.DialogueBank), "wrappers": wrappers,
		"source_relay": IRObject{"entry_pc24": pcString(p.Relay.Entry), "call_site_count": len(relayCalls), "source_pc24s": relaySources}}, nil
}

func (s *sourceDiscovery) validateMenuSource(id string, address int) error {
	offset, err := pc24Offset(address)
	if err != nil || offset < s.p.MenuStart || offset >= s.p.MenuEnd {
		return fmt.Errorf("%s source %s is outside menu catalogue", id, pcString(address))
	}
	return nil
}

func (s *sourceDiscovery) composerSources() (IRObject, IRObject, error) {
	p, d := s.p, s.d
	tables := []IRObject{}
	allTargets := []int{}
	for _, table := range p.Tables {
		targets, err := d.pointerTargets(table.Address, table.Count, table.Address>>16)
		if err != nil {
			return nil, nil, err
		}
		for index := range targets {
			targets[index] += table.Increment
			if err := s.validateMenuSource(table.ID, targets[index]); err != nil {
				return nil, nil, err
			}
			if err := s.reference(targets[index], "fixed_composer_"+table.ID+"_pointer", table.Address, "via_source_table"); err != nil {
				return nil, nil, err
			}
		}
		allTargets = append(allTargets, targets...)
		tables = append(tables, IRObject{"id": table.ID, "source_table_pc24": pcString(table.Address), "pointer_count": table.Count,
			"target_increment": table.Increment, "unique_target_count": len(distinctInts(targets)), "target_pc24s": pcStrings(targets)})
	}
	direct := []IRObject{}
	var directTable any
	if table := p.DirectTable; table != nil {
		targets, err := d.pointerTargets(table.Address, len(table.IDs), table.Address>>16)
		if err != nil {
			return nil, nil, err
		}
		for index, id := range table.IDs {
			targets[index] += table.Increment
			if err := s.validateMenuSource(id, targets[index]); err != nil {
				return nil, nil, err
			}
			if err := s.reference(targets[index], "fixed_composer_direct_"+id, table.Address, "via_source_table"); err != nil {
				return nil, nil, err
			}
			direct = append(direct, IRObject{"id": id, "source_pc24": pcString(targets[index]), "confidence": "call_flow_verified"})
		}
		directTable = IRObject{"source_table_pc24": pcString(table.Address), "pointer_count": len(table.IDs),
			"target_increment": table.Increment, "target_pc24s": pcStrings(targets)}
	}
	indexed := []IRObject{}
	for _, source := range p.IndexedDirect {
		if err := s.validateMenuSource(source.ID, source.Address); err != nil {
			return nil, nil, err
		}
		expected := []byte{8, byte(source.NativeIndex), byte(source.NativeIndex >> 8), byte(source.Table), byte(source.Table >> 8), 0}
		if err := d.expectPC(source.Address, expected, source.ID+" indexed selector"); err != nil {
			return nil, nil, err
		}
		if err := s.reference(source.Address, "fixed_composer_indexed_"+source.ID, p.ComposerEntry, "via_consumer_entry"); err != nil {
			return nil, nil, err
		}
		targets, err := d.pointerTargets(source.Table, len(source.IDs), source.Table>>16)
		if err != nil {
			return nil, nil, err
		}
		rows := []IRObject{}
		for index, id := range source.IDs {
			target := targets[index]
			if err := s.validateMenuSource(id, target); err != nil {
				return nil, nil, err
			}
			if err := s.reference(target, "fixed_composer_indexed_"+id, source.Table, "via_source_table"); err != nil {
				return nil, nil, err
			}
			rows = append(rows, IRObject{"id": id, "source_pc24": pcString(target)})
		}
		indexed = append(indexed, IRObject{"id": source.ID, "source_pc24": pcString(source.Address), "selector_opcode": "08",
			"native_index_address": localString(source.NativeIndex), "source_table_pc24": pcString(source.Table),
			"pointer_count": len(rows), "targets": rows, "confidence": "call_flow_verified"})
	}
	dynamic := []IRObject{}
	score := false
	for _, source := range p.Dynamic {
		if err := s.validateMenuSource(source.ID, source.Address); err != nil {
			return nil, nil, err
		}
		if err := s.reference(source.Address, "fixed_composer_dynamic_"+source.ID, p.ComposerEntry, "via_consumer_entry"); err != nil {
			return nil, nil, err
		}
		dynamic = append(dynamic, IRObject{"id": source.ID, "source_pc24": pcString(source.Address), "classification": "language_bearing_dynamic_report"})
		score = score || source.ID == "score_report"
	}
	var numeric any
	if p.Numeric != nil {
		if err := s.validateMenuSource("native_numeric_only", *p.Numeric); err != nil {
			return nil, nil, err
		}
		numeric = IRObject{"id": "native_numeric_only", "source_pc24": pcString(*p.Numeric), "classification": "typed_values_without_language_text", "included_in_language_source_seeds": false}
	}
	names := []IRObject{}
	for _, source := range p.NameEntry {
		if err := s.validateMenuSource(source.ID, source.Address); err != nil {
			return nil, nil, err
		}
		if err := s.reference(source.Address, "fixed_composer_name_entry_"+source.ID, p.ComposerEntry, "via_consumer_entry"); err != nil {
			return nil, nil, err
		}
		names = append(names, IRObject{"id": source.ID, "source_pc24": pcString(source.Address), "classification": "language_bearing_name_entry", "confidence": "call_flow_verified"})
	}
	calls, err := d.callsTo(p.ComposerEntry, "jsl", false)
	if err != nil {
		return nil, nil, err
	}
	expectedCount := 0
	for _, group := range p.Groups {
		if group.Count < 0 || group.Count > len(d.rom) {
			return nil, nil, fmt.Errorf("invalid composer group size")
		}
		expectedCount += group.Count
	}
	if len(calls) != expectedCount {
		return nil, nil, fmt.Errorf("composer call-site count changed: expected %d, found %d", expectedCount, len(calls))
	}
	sites := []IRObject{}
	counts := map[string]int{}
	for _, group := range p.Groups {
		for range group.Count {
			sites = append(sites, IRObject{"call_site": pcString(calls[len(sites)]), "surface_group": group.ID})
			counts[group.ID]++
		}
	}
	flows, err := s.composerFlows(calls)
	if err != nil {
		return nil, nil, err
	}
	status := "known_tables_direct_dynamic_censused"
	if len(flows) > 0 {
		status = "tables_direct_dynamic_and_stateful_flows_censused"
	}
	consumer := IRObject{"id": "fixed_text_composer", "entry_pc24": pcString(p.ComposerEntry), "call_kind": "long_jsl",
		"call_site_count": len(sites), "surface_group_counts": counts, "call_sites": sites}
	sources := IRObject{"status": status, "pointer_tables": tables, "pointer_slot_count": len(allTargets),
		"unique_pointer_target_count": len(distinctInts(allTargets)), "direct_sources": direct, "direct_source_table": directTable,
		"indexed_direct_sources": indexed, "direct_source_candidates": []IRObject{}, "dynamic_reports": dynamic,
		"score_report_present": score, "numeric_only_source": numeric, "name_entry_sources": names, "flow_sources": flows}
	return consumer, sources, nil
}

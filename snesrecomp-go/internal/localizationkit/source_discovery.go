package localizationkit

import "fmt"

// NativeSourceCensus accounts for the two native readers' known call paths.
// This is a dependency of complete extraction, not a whole-ROM coverage claim:
// ownership, source expansion, semantic routing and graphics are separate gates.
type NativeSourceCensus struct {
	destinations              *NativeDestinationCensus
	WholeGameCoverageComplete bool       `json:"whole_game_coverage_complete"`
	Consumers                 []IRObject `json:"consumers"`
	DialogueForwarding        IRObject   `json:"dialogue_forwarding"`
	NestedHandlerSources      IRObject   `json:"nested_handler_sources"`
	FixedComposerSources      IRObject   `json:"fixed_composer_sources"`
	SourceReferenceSeeds      IRObject   `json:"source_reference_seeds"`
	NativeDictionaryConsumers IRObject   `json:"native_dictionary_consumers"`
	NativeDialogueLayout      IRObject   `json:"native_dialogue_layout"`
}

// DiscoverNativeSources independently follows ROM call/pointer/control-flow
// evidence. It accepts no pre-extracted record offsets or decoded source list.
// Failure returns no partial census; callers must not publish partial results.
func (d *Decoder) DiscoverNativeSources() (*NativeSourceCensus, error) {
	if d == nil {
		return nil, fmt.Errorf("nil localization decoder")
	}
	profile, ok := sourceProfiles[d.profile.ID]
	if !ok {
		return nil, fmt.Errorf("no source profile for %q", d.profile.ID)
	}
	return d.discoverNativeSources(profile)
}

type sourceDiscovery struct {
	d             *Decoder
	p             sourceProfile
	references    []IRObject
	matrices      []IRObject
	matrixTargets []int
}

func (d *Decoder) discoverNativeSources(profile sourceProfile) (*NativeSourceCensus, error) {
	if profile.MenuStart < 0 || profile.MenuEnd <= profile.MenuStart || profile.MenuEnd > len(d.rom) {
		return nil, fmt.Errorf("invalid menu catalogue bounds")
	}
	for _, signature := range []struct {
		pc   int
		data []byte
		role string
	}{
		{profile.InteractiveEntry, interactiveSignature, "interactive_dialogue"},
		{profile.ComposerEntry, composerSignature, "fixed_text_composer"},
	} {
		if err := d.expectPC(signature.pc, signature.data, signature.role); err != nil {
			return nil, err
		}
	}
	dictionary, err := d.nativeDictionaryProof(profile)
	if err != nil {
		return nil, err
	}
	layout, err := d.nativeLayout(profile)
	if err != nil {
		return nil, err
	}
	s := sourceDiscovery{d: d, p: profile, references: []IRObject{}, matrices: []IRObject{}}
	interactive, err := s.interactiveSources()
	if err != nil {
		return nil, fmt.Errorf("%s interactive sources: %w", profile.ID, err)
	}
	forwarding, err := s.wrapperSources()
	if err != nil {
		return nil, fmt.Errorf("%s forwarding sources: %w", profile.ID, err)
	}
	composer, fixed, err := s.composerSources()
	if err != nil {
		return nil, fmt.Errorf("%s composer sources: %w", profile.ID, err)
	}
	unique := map[string]bool{}
	for _, reference := range s.references {
		unique[reference["source_pc24"].(string)] = true
	}
	return &NativeSourceCensus{
		Consumers:          []IRObject{interactive, composer},
		DialogueForwarding: forwarding,
		NestedHandlerSources: IRObject{"status": "pointer_matrix_censused", "matrices": s.matrices,
			"pointer_slot_count": len(s.matrixTargets), "unique_target_count": len(distinctInts(s.matrixTargets))},
		FixedComposerSources: fixed,
		SourceReferenceSeeds: IRObject{"status": "known_call_paths_censused", "reference_count": len(s.references),
			"unique_source_count": len(unique), "references": s.references},
		NativeDictionaryConsumers: dictionary, NativeDialogueLayout: layout,
	}, nil
}

func (s *sourceDiscovery) reference(address int, kind string, via int, edge string) error {
	if _, err := s.d.pcSpan(address, 1); err != nil {
		return err
	}
	if _, err := s.d.pcSpan(via, 1); err != nil {
		return err
	}
	if edge != "via_call_site" && edge != "via_source_table" && edge != "via_consumer_entry" {
		return fmt.Errorf("source reference needs exactly one known provenance edge")
	}
	s.references = append(s.references, IRObject{"source_pc24": pcString(address), "reference_kind": kind, edge: pcString(via)})
	return nil
}
func (s *sourceDiscovery) yReference(bank, local int, kind string, call int) error {
	address, err := sourcePC(bank, local)
	if err != nil {
		return err
	}
	return s.reference(address, kind, call, "via_call_site")
}

func (s *sourceDiscovery) interactiveSources() (IRObject, error) {
	p, d := s.p, s.d
	calls, err := d.callsTo(p.InteractiveEntry, "jsr", false)
	if err != nil {
		return nil, err
	}
	if len(calls) != p.InteractiveCallCount {
		return nil, fmt.Errorf("call count changed: expected %d, found %d", p.InteractiveCallCount, len(calls))
	}
	sites := []IRObject{}
	type indirectCall struct {
		call int
		row  IRObject
	}
	indirect := []indirectCall{}
	direct := []int{}
	joins := map[int]bool{}
	adjacentCount := 0
	for _, call := range calls {
		if call>>16 != p.InteractiveEntry>>16 {
			return nil, fmt.Errorf("bank-local interpreter has cross-bank raw call at %s", pcString(call))
		}
		y, err := d.directY(call)
		if err != nil {
			return nil, err
		}
		row := IRObject{"call_site": pcString(call)}
		if y == nil {
			if _, expected := p.BranchJoins[call]; expected {
				return nil, fmt.Errorf("branch join lost fallthrough source at %s", pcString(call))
			}
			row["source_origin"] = "control_flow_table_or_argument"
			indirect = append(indirect, indirectCall{call, row})
		} else {
			adjacentCount++
			incoming, err := d.incomingY(call)
			if err != nil {
				return nil, err
			}
			expected, known := p.BranchJoins[call]
			if len(incoming) != 0 {
				candidates := candidateYs(incoming, *y)
				if !known || len(candidates) != expected {
					return nil, fmt.Errorf("unexpected branch-join candidates at %s", pcString(call))
				}
				joins[call] = true
				row["source_origin"], row["fallthrough_source_y"] = "branch_join_immediate_y", localString(*y)
				row["candidate_source_y"], row["incoming_branches"] = localStrings(candidates), incomingRows(incoming)
				direct = append(direct, candidates...)
				for _, candidate := range candidates {
					if err := s.yReference(p.InteractiveEntry>>16, candidate, "interpreter_branch_join_immediate_y", call); err != nil {
						return nil, err
					}
				}
			} else {
				if known {
					return nil, fmt.Errorf("expected branch join missing at %s", pcString(call))
				}
				row["source_origin"], row["source_y"] = "adjacent_immediate_y", localString(*y)
				direct = append(direct, *y)
				if err := s.yReference(p.InteractiveEntry>>16, *y, "interpreter_adjacent_immediate_y", call); err != nil {
					return nil, err
				}
			}
		}
		sites = append(sites, row)
	}
	if len(joins) != len(p.BranchJoins) {
		return nil, fmt.Errorf("missing branch-joined interpreter calls")
	}
	if len(indirect) != len(p.Nonadjacent) {
		return nil, fmt.Errorf("non-adjacent interpreter layout changed")
	}
	for index, site := range indirect {
		origin := p.Nonadjacent[index]
		site.row["source_origin"] = origin
		if err := s.nonadjacentSource(site.call, site.row, origin); err != nil {
			return nil, err
		}
	}
	return IRObject{"id": "interactive_dialogue", "entry_pc24": pcString(p.InteractiveEntry),
		"call_kind": "bank_local_jsr", "call_site_count": len(sites),
		"adjacent_immediate_y_call_count": adjacentCount, "branch_join_immediate_y_call_count": len(joins),
		"unique_immediate_y_sources": len(distinctInts(direct)), "nonadjacent_source_origin_call_count": len(indirect),
		"unresolved_source_origin_call_count": 0, "call_sites": sites}, nil
}

func (s *sourceDiscovery) continuation(call int) (sourceContinuation, error) {
	for _, candidate := range s.p.Continuations {
		if candidate.Call == call {
			return candidate, nil
		}
	}
	return sourceContinuation{}, fmt.Errorf("unprofiled yield continuation at %s", pcString(call))
}
func (s *sourceDiscovery) decodeSource(consumer Consumer, address int, menuBound bool) (Record, error) {
	start, err := pc24Offset(address)
	if err != nil {
		return Record{}, err
	}
	limit := min(len(s.d.rom), start+maxRecordBytes)
	if menuBound {
		limit = s.p.MenuEnd
	}
	return s.d.DecodeRecord(consumer, start, limit, true)
}
func (s *sourceDiscovery) nonadjacentSource(call int, row IRObject, origin string) error {
	p, d := s.p, s.d
	switch origin {
	case "conditional_immediate":
		candidates, err := d.conditionalY(call)
		if err != nil {
			return err
		}
		row["target_resolution"], row["candidate_source_y"] = "verified_three_way_branch_join", localStrings(candidates)
		for _, candidate := range candidates {
			if err := s.yReference(p.InteractiveEntry>>16, candidate, "interpreter_conditional_immediate_y", call); err != nil {
				return err
			}
		}
	case "nested_handler_table":
		return s.handlerMatrix(call, row)
	case "offering_pointer_table":
		row["source_table_pc24"], row["pointer_count"] = pcString(p.OfferingTable), 21
		targets, err := d.pointerTargets(p.OfferingTable, 21, p.DialogueBank)
		if err != nil {
			return err
		}
		for _, target := range targets {
			if p.EndingTable != nil && target >= *p.EndingTable {
				continue
			}
			if err := s.reference(target, "interpreter_offering_pointer_table", call, "via_call_site"); err != nil {
				return err
			}
		}
	case "yield_continuation":
		continuation, err := s.continuation(call)
		if err != nil {
			return err
		}
		record, err := s.decodeSource(Interactive, continuation.Parent, false)
		if err != nil {
			return err
		}
		if offsetPC(record.End) != continuation.Source {
			return fmt.Errorf("yield continuation cursor changed at %s", pcString(call))
		}
		row["parent_source_pc24"], row["source_pc24"] = pcString(continuation.Parent), pcString(continuation.Source)
		row["target_resolution"] = "verified_returned_y_cursor"
		return s.reference(continuation.Source, "interpreter_yield_continuation", call, "via_call_site")
	case "dialogue_wrapper": // Enumerated separately, including the forwarding relay.
	default:
		return fmt.Errorf("unknown interpreter source origin %q", origin)
	}
	return nil
}

func (s *sourceDiscovery) handlerMatrix(call int, row IRObject) error {
	p, d := s.p, s.d
	offset, err := pc24Offset(call)
	if err != nil {
		return err
	}
	table := -1
	if d.profile.Encoding == "direct-glyph" {
		data, err := d.span(offset-24, 24)
		if err != nil {
			return err
		}
		for i := 0; i < 20; i++ {
			if data[i] == 0xbf {
				candidate := word(data[i+1:]) | int(data[i+3])<<16
				if candidate == p.HandlerTable {
					table = candidate
					break
				}
			}
		}
	} else {
		data, err := d.span(offset-7, 7)
		if err != nil || data[0] != 0x7d {
			return fmt.Errorf("handler lookup shape changed at %s", pcString(call))
		}
		table, err = sourcePC(p.DialogueBank, word(data[1:]))
		if err != nil {
			return err
		}
	}
	if table != p.HandlerTable {
		return fmt.Errorf("handler lookup table changed at %s", pcString(call))
	}
	row["source_table_pc24"] = pcString(table)
	rowTables, err := d.pointerTargets(table, 6, p.DialogueBank)
	if err != nil {
		return err
	}
	matrixRows := []IRObject{}
	matrixTargets := []int{}
	for index, rowTable := range rowTables {
		if rowTable != table+12+index*64 {
			return fmt.Errorf("nested handler row layout changed at %s", pcString(table))
		}
		targets, err := d.pointerTargets(rowTable, 32, p.DialogueBank)
		if err != nil {
			return err
		}
		matrixTargets = append(matrixTargets, targets...)
		for _, target := range targets {
			if err := s.reference(target, "interpreter_nested_handler_pointer", rowTable, "via_source_table"); err != nil {
				return err
			}
		}
		matrixRows = append(matrixRows, IRObject{"city_slot": index, "source_table_pc24": pcString(rowTable),
			"pointer_count": len(targets), "unique_target_count": len(distinctInts(targets)), "target_pc24s": pcStrings(targets)})
	}
	row["target_resolution"], row["row_count"], row["slots_per_row"] = "resolved_pointer_matrix", len(matrixRows), 32
	row["unique_source_count"] = len(distinctInts(matrixTargets))
	s.matrices = append(s.matrices, IRObject{"row_table_pc24": pcString(table), "row_count": len(matrixRows), "slots_per_row": 32,
		"pointer_slot_count": len(matrixTargets), "unique_target_count": len(distinctInts(matrixTargets)), "rows": matrixRows})
	s.matrixTargets = append(s.matrixTargets, matrixTargets...)
	return nil
}

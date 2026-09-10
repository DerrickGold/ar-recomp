package localizationkit

import (
	"fmt"
	"slices"
)

func (s *sourceDiscovery) flowDestination(id string, address int) (IRObject, error) {
	if err := s.validateMenuSource(id, address); err != nil {
		return nil, err
	}
	data, err := s.d.pcSpan(address, 2)
	if err != nil {
		return nil, err
	}
	if data[0] >= 32 || data[1] >= 32 {
		return nil, fmt.Errorf("%s has invalid BG3 destination %02X/%02X", id, data[0], data[1])
	}
	return IRObject{"column": int(data[0]), "row": int(data[1])}, nil
}

func (s *sourceDiscovery) composerFlows(calls []int) ([]IRObject, error) {
	rows := []IRObject{}
	flow := s.p.Flow
	if flow == nil {
		return rows, nil
	}
	if !slices.Contains(calls, flow.SelectorCall) {
		return nil, fmt.Errorf("message-speed selector is not a composer call")
	}
	y, err := s.d.directY(flow.SelectorCall)
	if err != nil || y == nil {
		return nil, fmt.Errorf("message-speed selector lost direct Y source")
	}
	selector, err := sourcePC(flow.SelectorCall>>16, *y)
	if err != nil {
		return nil, err
	}
	if err := s.validateMenuSource("message_speed_selector", selector); err != nil {
		return nil, err
	}
	scaleDescriptor, scale := selector+4, selector+6
	if err := s.validateMenuSource("message_speed_scale_labels", scale); err != nil {
		return nil, err
	}
	selectorRecord, err := s.decodeSource(FixedComposer, selector, true)
	if err != nil {
		return nil, err
	}
	if offsetPC(selectorRecord.End) != scaleDescriptor {
		return nil, fmt.Errorf("message-speed selector no longer ends at scale descriptor")
	}
	scaleRecord, err := s.decodeSource(FixedComposer, scale, true)
	if err != nil {
		return nil, err
	}
	sampleY, err := s.d.directY(flow.SampleCall)
	if err != nil || sampleY == nil {
		return nil, fmt.Errorf("message-speed sample lost direct Y source")
	}
	sample, err := sourcePC(flow.SampleCall>>16, *sampleY)
	if err != nil {
		return nil, err
	}
	if !scaleRecord.Terminated || offsetPC(scaleRecord.End) != sample {
		return nil, fmt.Errorf("message-speed scale no longer terminates at sample dialogue")
	}
	var choiceDescriptor, provenance int
	if flow.ChoiceDescriptor == nil {
		if flow.ChoiceYieldCall == nil {
			return nil, fmt.Errorf("choice-label continuation missing")
		}
		continuation, err := s.continuation(*flow.ChoiceYieldCall)
		if err != nil {
			return nil, err
		}
		record, err := s.decodeSource(Interactive, continuation.Source, false)
		if err != nil {
			return nil, err
		}
		if !record.Terminated || len(record.Operations) == 0 || record.Operations[len(record.Operations)-1]["op"] != "yield" {
			return nil, fmt.Errorf("choice-label continuation no longer returns descriptor cursor")
		}
		choiceDescriptor, provenance = offsetPC(record.End), *flow.ChoiceYieldCall
	} else {
		choiceDescriptor = *flow.ChoiceDescriptor
		if len(flow.ChoiceLoadSites) == 0 {
			return nil, fmt.Errorf("choice descriptor has no load-site evidence")
		}
		expected := []byte{0xa2, byte(choiceDescriptor), byte(choiceDescriptor >> 8)}
		for _, call := range flow.ChoiceLoadSites {
			if err := s.d.expectPC(call, expected, "choice descriptor load"); err != nil {
				return nil, err
			}
		}
		provenance = flow.ChoiceLoadSites[0]
	}
	scaleDestination, err := s.flowDestination("message_speed_scale_descriptor", scaleDescriptor)
	if err != nil {
		return nil, err
	}
	choiceDestination, err := s.flowDestination("choice_labels_descriptor", choiceDescriptor)
	if err != nil {
		return nil, err
	}
	choice := choiceDescriptor + 2
	if err := s.validateMenuSource("choice_labels", choice); err != nil {
		return nil, err
	}
	choiceRecord, err := s.decodeSource(FixedComposer, choice, true)
	if err != nil {
		return nil, err
	}
	if !choiceRecord.Terminated {
		return nil, fmt.Errorf("choice labels have no terminator")
	}
	rows = append(rows,
		IRObject{"id": "message_speed_selector", "source_pc24": pcString(selector), "classification": "typed_non_language_indicator",
			"included_in_language_source_seeds": false, "via_call_site": pcString(flow.SelectorCall), "confidence": "direct_call_flow_verified"},
		IRObject{"id": "message_speed_scale_labels", "source_pc24": pcString(scale), "descriptor_pc24": pcString(scaleDescriptor),
			"destination": scaleDestination, "classification": "fixed_composer_text", "included_in_language_source_seeds": true,
			"via_call_site": pcString(flow.SelectorCall), "confidence": "direct_call_and_adjacent_flow_verified"},
		IRObject{"id": "choice_labels", "source_pc24": pcString(choice), "descriptor_pc24": pcString(choiceDescriptor),
			"destination": choiceDestination, "classification": "fixed_composer_text", "included_in_language_source_seeds": true,
			"via_call_site": pcString(provenance), "confidence": "stateful_flow_and_destination_verified"},
	)
	if err := s.reference(scale, "fixed_composer_flow_message_speed_scale_labels", flow.SelectorCall, "via_call_site"); err != nil {
		return nil, err
	}
	if err := s.reference(choice, "fixed_composer_flow_choice_labels", provenance, "via_call_site"); err != nil {
		return nil, err
	}
	return rows, nil
}

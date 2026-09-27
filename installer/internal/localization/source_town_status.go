package localization

import (
	"encoding/binary"
	"fmt"
	"slices"
)

// Town status selects two descriptors (construction/sealing), or a blank
// descriptor on dismissal. Follow that producer through the shared composer
// wrapper; source addresses and regional destination cells come from the ROM.
func (s *sourceDiscovery) townStatusFlows(calls []int) ([]IRObject, error) {
	entry := s.p.Flow.TownStatus
	if entry == 0 {
		return nil, nil
	}
	code, err := s.d.pcSpan(entry, 26)
	if err != nil {
		return nil, err
	}
	if err := s.d.expectPC(entry, []byte{0x29, 0xff, 0x30, 0x0d, 0x0a, 0xaa, 0xc2, 0x20, 0xbf}, "town status selector"); err != nil {
		return nil, err
	}
	if err := s.d.expectPC(entry+12, []byte{0xaa, 0xe2, 0x20, 0x80, 0x03, 0xa2}, "town status descriptor selection"); err != nil {
		return nil, err
	}
	if code[20] != 0x20 || !slices.Equal(code[23:], []byte{0xab, 0x28, 0x6b}) {
		return nil, fmt.Errorf("town status composer call changed")
	}
	bank := entry & 0xff0000
	table := int(code[9]) | int(code[10])<<8 | int(code[11])<<16
	wrapper := bank | int(binary.LittleEndian.Uint16(code[21:23]))
	if !slices.Contains(calls, wrapper+9) {
		return nil, fmt.Errorf("town status wrapper is not a composer call")
	}
	if err := s.d.expectPC(wrapper, []byte{0x08, 0xc2, 0x20, 0xbd, 0, 0, 0xe8, 0xe8, 0x9b}, "town status destination/source wrapper"); err != nil {
		return nil, err
	}
	pointers, err := s.d.pcSpan(table, 4)
	if err != nil {
		return nil, err
	}
	descriptors := []int{
		bank | int(binary.LittleEndian.Uint16(pointers[:2])),
		bank | int(binary.LittleEndian.Uint16(pointers[2:])),
		bank | int(binary.LittleEndian.Uint16(code[18:20])),
	}
	ids := []string{"town_status_construction", "town_status_sealing_lair", "town_status_erase"}
	var rows []IRObject
	for i, descriptor := range descriptors {
		destination, err := s.flowDestination(ids[i], descriptor)
		if err != nil {
			return nil, err
		}
		source := descriptor + 2
		record, err := s.decodeSource(FixedComposer, source, true)
		if err != nil {
			return nil, err
		}
		if !record.Terminated {
			return nil, fmt.Errorf("%s has no terminator", ids[i])
		}
		classification := "fixed_composer_text"
		if i == 2 {
			classification = "typed_native_erase"
		} else if err := s.reference(source, "fixed_composer_flow_"+ids[i], wrapper+9, "via_call_site"); err != nil {
			return nil, err
		}
		rows = append(rows, IRObject{"id": ids[i], "source_pc24": pcString(source),
			"descriptor_pc24": pcString(descriptor), "destination": destination,
			"classification": classification, "included_in_language_source_seeds": i < 2,
			"via_call_site": pcString(wrapper + 9), "confidence": "status_selector_and_composer_wrapper_verified"})
	}
	return rows, nil
}

package localization

import (
	"fmt"
	"slices"
	"strconv"
)

// Census views are deliberately private and used only with freshly built Go
// source evidence. No GUI/user-authored JSON is trusted as a native census.
func irRows(object IRObject, key string) []IRObject { return object[key].([]IRObject) }
func irString(object IRObject, key string) string   { value, _ := object[key].(string); return value }
func parsePC(value string) (int, error) {
	if len(value) != 8 || value[0] != '$' || value[3] != ':' {
		return 0, fmt.Errorf("invalid PC24 text %q", value)
	}
	parsed, err := strconv.ParseUint(value[1:3]+value[4:], 16, 24)
	if err != nil {
		return 0, err
	}
	pc := int(parsed)
	if _, err := pc24Offset(pc); err != nil {
		return 0, err
	}
	return pc, nil
}
func parsedOffset(value string) (int, error) {
	pc, err := parsePC(value)
	if err != nil {
		return 0, err
	}
	return pc24Offset(pc)
}

type NativeMenuCatalog struct {
	Status            string           `json:"status"`
	Warning           string           `json:"warning"`
	SourceRange       IRObject         `json:"source_range"`
	ProvenSourceCount int              `json:"proven_source_count"`
	Segments          []*NativeMessage `json:"segments"`
}
type composerIdentity struct {
	candidates, owners []string
	classification     string
}

func (d *Decoder) fixedCatalog(p sourceProfile, census *NativeSourceCensus) (*NativeMenuCatalog, error) {
	identities := map[int]*composerIdentity{}
	add := func(address, candidate, owner, classification string) error {
		offset, err := parsedOffset(address)
		if err != nil {
			return err
		}
		if offset < p.MenuStart || offset >= p.MenuEnd {
			return fmt.Errorf("composer source %s outside bounded catalogue", address)
		}
		row := identities[offset]
		if row == nil {
			row = &composerIdentity{classification: classification}
			identities[offset] = row
		}
		if row.classification != classification {
			return fmt.Errorf("%s: conflicting composer classifications", address)
		}
		row.candidates = appendUnique(row.candidates, candidate)
		row.owners = appendUnique(row.owners, owner)
		return nil
	}
	sources := census.FixedComposerSources
	for _, table := range irRows(sources, "pointer_tables") {
		id := irString(table, "id")
		for index, target := range table["target_pc24s"].([]string) {
			if err := add(target, fmt.Sprintf("menu.%s.%02d", id, index), "pointer_table:"+id, "fixed_composer_text"); err != nil {
				return nil, err
			}
		}
	}
	for _, row := range irRows(sources, "direct_sources") {
		id := irString(row, "id")
		if err := add(irString(row, "source_pc24"), "menu."+id, "direct_source:"+id, "fixed_composer_text"); err != nil {
			return nil, err
		}
	}
	for _, row := range irRows(sources, "indexed_direct_sources") {
		id := irString(row, "id")
		if err := add(irString(row, "source_pc24"), "menu."+id, "indexed_source:"+id, "fixed_composer_text"); err != nil {
			return nil, err
		}
		for _, target := range irRows(row, "targets") {
			if err := add(irString(target, "source_pc24"), "menu."+irString(target, "id"), "indexed_target:"+id, "fixed_composer_text"); err != nil {
				return nil, err
			}
		}
	}
	for _, row := range irRows(sources, "dynamic_reports") {
		id := irString(row, "id")
		if err := add(irString(row, "source_pc24"), "status."+id, "dynamic_report:"+id, "typed_dynamic_text"); err != nil {
			return nil, err
		}
	}
	if numeric := sources["numeric_only_source"]; numeric != nil {
		if err := add(irString(numeric.(IRObject), "source_pc24"), "native.numeric_only", "numeric_only_descriptor", "typed_numeric_only"); err != nil {
			return nil, err
		}
	}
	for _, row := range irRows(sources, "name_entry_sources") {
		id := irString(row, "id")
		if err := add(irString(row, "source_pc24"), "name_entry."+id, "name_entry:"+id, "fixed_composer_text"); err != nil {
			return nil, err
		}
	}
	for _, row := range irRows(sources, "flow_sources") {
		id := irString(row, "id")
		if err := add(irString(row, "source_pc24"), "system."+id, "stateful_flow:"+id, irString(row, "classification")); err != nil {
			return nil, err
		}
	}
	positions := make([]int, 0, len(identities))
	for position := range identities {
		positions = append(positions, position)
	}
	slices.Sort(positions)
	segments := make([]*NativeMessage, 0, len(positions))
	for _, position := range positions {
		identity := identities[position]
		record, err := d.makeMessage(position, p.MenuEnd, FixedComposer)
		if err != nil {
			return nil, err
		}
		if !record.Terminated {
			return nil, fmt.Errorf("fixed composer record at %s has no terminator", cursorAddress(position))
		}
		if identity.classification == "typed_non_language_indicator" {
			record.Operations = []Operation{{"op": "insert_icon", "value": "message_speed_selector", "part_index": 0, "part_count": 1, "confidence": "mapped_semantic"}, record.Operations[len(record.Operations)-1]}
		}
		slices.Sort(identity.candidates)
		slices.Sort(identity.owners)
		record.CandidateSemanticIDs, record.Owners = identity.candidates, identity.owners
		record.Classification, record.AlignmentStatus = identity.classification, "role_verified_address_identity_pending"
		record.Confidence = "consumer_source_and_grammar_verified"
		units := visibleUnits(record.Operations)
		record.VisibleUnits = &units
		segments = append(segments, record)
	}
	return &NativeMenuCatalog{Status: "exact_consumer_roots_censused_semantic_ids_pending",
		Warning:           "Only call-flow/table-proven fixed-composer roots are decoded. Interleaved pointer, coordinate, and code bytes are deliberately not presented as text; the enclosing mixed range still requires whole-ROM byte ownership before the extraction gate can close.",
		SourceRange:       IRObject{"start_file_offset": fileOffset(p.MenuStart), "end_file_offset_exclusive": fileOffset(p.MenuEnd), "start_snes": cursorAddress(p.MenuStart), "byte_count": p.MenuEnd - p.MenuStart},
		ProvenSourceCount: len(segments), Segments: segments}, nil
}

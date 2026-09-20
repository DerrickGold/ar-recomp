package localization

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"slices"
	"strconv"
	"strings"
)

type NativeSemanticRoute struct {
	ID              string      `json:"id"`
	RecordID        string      `json:"source_record_id"`
	SourceOffset    int         `json:"source_offset_within_record"`
	Category        string      `json:"source_category"`
	Kind            string      `json:"route_kind"`
	Availability    string      `json:"availability"`
	Provenance      []string    `json:"provenance"`
	Operations      []Operation `json:"source_operations"`
	OperationCount  int         `json:"source_operation_count"`
	OperationSHA256 string      `json:"source_operations_sha256"`
}
type NativeSemanticCatalog struct {
	Status            string                 `json:"status"`
	Complete          bool                   `json:"complete"`
	IdentityUnit      string                 `json:"identity_unit"`
	RouteCount        int                    `json:"route_count"`
	RoutedRecordCount int                    `json:"routed_record_count"`
	DormantCount      int                    `json:"dormant_release_resource_count"`
	UnresolvedCount   int                    `json:"unresolved_consumer_reference_count"`
	UnresolvedIndices []int                  `json:"unresolved_consumer_reference_indices"`
	UnclassifiedCount int                    `json:"unclassified_language_record_count"`
	UnclassifiedIDs   []string               `json:"unclassified_language_record_ids"`
	Routes            []*NativeSemanticRoute `json:"routes"`
}

func pointerRoute(slot NativePointerSlot) (string, error) {
	cut := strings.LastIndexByte(slot.CandidateSemanticID, '.')
	if cut < 0 {
		return "", fmt.Errorf("invalid pointer semantic ID")
	}
	prefix := slot.CandidateSemanticID[:cut]
	index, err := strconv.Atoi(slot.CandidateSemanticID[cut+1:])
	if err != nil || index < 0 {
		return "", fmt.Errorf("invalid pointer semantic slot")
	}
	switch prefix {
	case "town_name":
		if index == 0 {
			return "town.name.table.sentinel", nil
		}
		if index > len(catalogFacts.CityKeys) {
			return "", fmt.Errorf("invalid town slot")
		}
		return "town.name." + catalogFacts.CityKeys[index-1], nil
	case "enemy_name":
		return fmt.Sprintf("enemy.name.slot_%02d", index), nil
	case "action.stage_name":
		if index >= len(catalogFacts.CityIDs) {
			return "", fmt.Errorf("invalid stage slot")
		}
		return "action.stage_name." + strings.Split(catalogFacts.CityIDs[index], ".")[1], nil
	case "offering":
		return fmt.Sprintf("dialogue.offering.slot_%02d", index), nil
	case "ending":
		return fmt.Sprintf("dialogue.ending.slot_%02d", index), nil
	}
	if strings.HasPrefix(prefix, "lookup.") {
		return strings.TrimPrefix(slot.CandidateSemanticID, "lookup."), nil
	}
	return fmt.Sprintf("resource.pointer.%s.slot_%02d", prefix, index), nil
}
func composerRoute(kind string, occurrence int) string {
	switch {
	case strings.HasPrefix(kind, "fixed_composer_") && strings.HasSuffix(kind, "_pointer"):
		table := strings.TrimSuffix(strings.TrimPrefix(kind, "fixed_composer_"), "_pointer")
		ids := catalogFacts.ComposerPointerRoutes[table]
		if occurrence >= 0 && occurrence < len(ids) {
			return ids[occurrence]
		}
	case strings.HasPrefix(kind, "fixed_composer_direct_"):
		return "sim.menu.offering_action." + strings.TrimPrefix(kind, "fixed_composer_direct_")
	case strings.HasPrefix(kind, "fixed_composer_indexed_"):
		id := strings.TrimPrefix(kind, "fixed_composer_indexed_")
		if id == "offering_action" {
			id = "selected"
		}
		return "sim.menu.offering_action." + id
	case strings.HasPrefix(kind, "fixed_composer_dynamic_"):
		return "status.report." + strings.TrimPrefix(kind, "fixed_composer_dynamic_")
	case strings.HasPrefix(kind, "fixed_composer_name_entry_"):
		return "name_entry." + strings.TrimPrefix(kind, "fixed_composer_name_entry_")
	case kind == "fixed_composer_flow_choice_labels":
		return "system.choice.yes_no"
	case kind == "fixed_composer_flow_message_speed_scale_labels":
		return "system.message_speed.scale_labels"
	}
	return ""
}

// Match Python's UTF-8, sorted-key, compact JSON hash contract. Go's encoder
// always escapes U+2028/U+2029; unescape only those actual escapes, preserving
// literal backslash-u text. All other quoting remains the standard library's.
func operationHash(operations []Operation) (string, error) {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(operations); err != nil {
		return "", err
	}
	encoded := bytes.TrimSuffix(buffer.Bytes(), []byte{'\n'})
	canonical := make([]byte, 0, len(encoded))
	for i := 0; i < len(encoded); {
		if encoded[i] == '\\' && i+1 < len(encoded) && encoded[i+1] == '\\' {
			canonical = append(canonical, encoded[i:i+2]...)
			i += 2
			continue
		}
		if bytes.HasPrefix(encoded[i:], []byte(`\u2028`)) {
			canonical = append(canonical, "\u2028"...)
			i += 6
			continue
		}
		if bytes.HasPrefix(encoded[i:], []byte(`\u2029`)) {
			canonical = append(canonical, "\u2029"...)
			i += 6
			continue
		}
		canonical = append(canonical, encoded[i])
		i++
	}
	return fmt.Sprintf("%x", sha256.Sum256(canonical)), nil
}

type routeBuilder struct {
	records map[string]*NativeMessage
	routes  map[string]*NativeSemanticRoute
}

func (r *routeBuilder) add(id, recordID string, offset int, kind, provenance string, regional bool) error {
	record := r.records[recordID]
	if record == nil {
		return fmt.Errorf("route %s has unknown record %s", id, recordID)
	}
	if offset < 0 || offset >= record.end-record.start {
		return fmt.Errorf("route %s offset outside source", id)
	}
	if existing := r.routes[id]; existing != nil {
		if existing.RecordID != recordID || existing.SourceOffset != offset {
			return fmt.Errorf("route %s has multiple sources", id)
		}
		existing.Provenance = appendUnique(existing.Provenance, provenance)
		return nil
	}
	category := record.Category
	if category == "" {
		category = record.Classification
	}
	if category == "" {
		category = "unclassified"
	}
	availability := "cross_release_candidate"
	if regional {
		availability = "release_variant"
	}
	r.routes[id] = &NativeSemanticRoute{ID: id, RecordID: recordID, SourceOffset: offset, Category: category, Kind: kind, Availability: availability, Provenance: []string{provenance}}
	return nil
}
func (b *catalogBuilder) semanticRoutes(census *NativeSourceCensus, menu *NativeMenuCatalog) (*NativeSemanticCatalog, error) {
	records := append(append([]*NativeMessage{}, b.messages...), menu.Segments...)
	r := routeBuilder{records: map[string]*NativeMessage{}, routes: map[string]*NativeSemanticRoute{}}
	for _, record := range records {
		r.records[record.ID] = record
	}
	offerings := []string{}
	for _, set := range b.pointers {
		for _, slot := range set.Slots {
			if slot.TargetID == nil {
				continue
			}
			id, err := pointerRoute(slot)
			if err != nil {
				return nil, err
			}
			if err := r.add(id, *slot.TargetID, 0, "pointer_table_slot", fmt.Sprintf("%s.slot_%02d", set.ID, slot.Slot), strings.HasPrefix(id, "resource.")); err != nil {
				return nil, err
			}
			if strings.HasPrefix(slot.CandidateSemanticID, "offering.") {
				offerings = append(offerings, id)
			}
		}
	}
	wrapperCalls := map[string][2]int{}
	for wi, wrapper := range irRows(census.DialogueForwarding, "wrappers") {
		for ci, call := range irRows(wrapper, "call_sites") {
			wrapperCalls[irString(call, "call_site")] = [2]int{wi, ci}
		}
	}
	nested := map[string]int{}
	for _, matrix := range irRows(census.NestedHandlerSources, "matrices") {
		for _, row := range irRows(matrix, "rows") {
			nested[irString(row, "source_table_pc24")] = row["city_slot"].(int)
		}
	}
	occurrences := map[[2]string]int{}
	alternatives := map[[2]int]int{}
	next := func(kind, provenance string) int {
		key := [2]string{kind, provenance}
		value := occurrences[key]
		occurrences[key]++
		return value
	}
	wrapperSix, relay := 0, 0
	unknown := []int{}
	for index, reference := range census.SourceReferenceSeeds.References {
		if reference.NativeSourceResolution == nil || reference.ResolvedRecordID == nil {
			unknown = append(unknown, index)
			continue
		}
		kind, recordID := reference.ReferenceKind, *reference.ResolvedRecordID
		if reference.SourceOffsetWithinRecord == nil {
			return nil, fmt.Errorf("resolved source %s has no offset", reference.SourcePC24)
		}
		if recordID == "" {
			unknown = append(unknown, index)
			continue
		}
		offset := *reference.SourceOffsetWithinRecord
		provenance := reference.ViaCallSite
		if provenance == "" {
			provenance = reference.ViaSourceTable
		}
		if provenance == "" {
			provenance = reference.ViaConsumerEntry
		}
		id := ""
		regional := false
		switch {
		case kind == "interpreter_nested_handler_pointer":
			occurrence := next(kind, provenance)
			city, ok := nested[provenance]
			if ok && city >= 0 && city < len(catalogFacts.CityKeys) && occurrence < 32 {
				id = fmt.Sprintf("simulation.event.%s.slot_%02d", catalogFacts.CityKeys[city], occurrence)
			}
		case kind == "interpreter_offering_pointer_table":
			occurrence := next(kind, provenance)
			if occurrence < len(offerings) {
				id = offerings[occurrence]
			}
		case strings.HasPrefix(kind, "interpreter_"):
			call, err := parsePC(reference.ViaCallSite)
			if err != nil {
				return nil, err
			}
			ids := b.p.InteractiveRoutes[call]
			occurrence := next(kind, provenance)
			if occurrence < len(ids) {
				id = ids[occurrence]
			}
			regional = strings.HasPrefix(id, "variant.")
		case strings.HasPrefix(kind, "dialogue_wrapper_relay_"):
			if relay < len(catalogFacts.CityKeys) {
				id = "dialogue.event.relay." + catalogFacts.CityKeys[relay]
			}
			relay++
		case strings.HasPrefix(kind, "dialogue_wrapper_"):
			position, ok := wrapperCalls[reference.ViaCallSite]
			if ok {
				wi, ci := position[0], position[1]
				alternative := alternatives[position]
				alternatives[position]++
				if wi == 6 {
					id = fmt.Sprintf("dialogue.event.wrapper_06.route_%02d", wrapperSix)
					wrapperSix++
				} else {
					normalized := ci
					if wi == 0 && b.p.Japanese {
						if ci == 2 {
							id = "variant.jp.dialogue.event.wrapper_00.extra_call_02"
							regional = true
						} else if ci > 2 {
							normalized--
						}
					}
					if id == "" {
						id = fmt.Sprintf("dialogue.event.wrapper_%02d.call_%02d.source_%02d", wi, normalized, alternative)
					}
				}
			}
		case strings.HasPrefix(kind, "fixed_composer_"):
			id = composerRoute(kind, next(kind, provenance))
		}
		if id == "" {
			unknown = append(unknown, index)
			continue
		}
		if err := r.add(id, recordID, offset, "consumer_route", kind+":"+provenance, regional); err != nil {
			return nil, err
		}
	}
	sorted := append([]*NativeMessage{}, records...)
	slices.SortStableFunc(sorted, func(a, c *NativeMessage) int { return a.start - c.start })
	uiIndices := map[string]int{}
	for _, record := range sorted {
		category := record.Category
		index := uiIndices[category]
		uiIndices[category]++
		id := ""
		regional := false
		switch {
		case category == "title_and_mode_menu" && index < len(b.p.TitleRouteIDs):
			id = b.p.TitleRouteIDs[index]
			regional = b.p.RegionalTitle || strings.HasSuffix(id, "professional") || strings.HasSuffix(id, "special")
		case category == "action_hud_label" && index < len(catalogFacts.ActionIDs):
			id = catalogFacts.ActionIDs[index]
		case category == "sound_test_menu" && index == 0:
			id = "sound_test.menu.labels"
		}
		if id != "" {
			if err := r.add(id, record.ID, 0, "bounded_ui_resource", fmt.Sprintf("%s.record_%02d", category, index), regional); err != nil {
				return nil, err
			}
		}
	}
	routed := map[string][]string{}
	for id, route := range r.routes {
		routed[route.RecordID] = append(routed[route.RecordID], id)
	}
	dormant := 0
	dormantIndices := map[string]int{}
	unclassified := []string{}
	for _, record := range records {
		ids := routed[record.ID]
		if len(ids) > 0 {
			slices.Sort(ids)
			record.SemanticRouteIDs = &ids
			record.AlignmentStatus = "logical_routes_verified"
		} else if !recordHasLanguage(record) || strings.HasPrefix(record.SourceOwnership, "verified_non_language_") {
			ids = []string{}
			record.SemanticRouteIDs = &ids
			record.AlignmentStatus = "verified_non_language_resource"
		} else if record.SourceOwnership == "dormant_or_release_variant_text" {
			category := record.Category
			if category == "" {
				category = "text"
			}
			index := dormantIndices[category]
			dormantIndices[category]++
			id := fmt.Sprintf("variant.%s.dormant.%s.resource_%02d", b.p.ID, category, index)
			if err := r.add(id, record.ID, 0, "dormant_retail_resource", "decoded_bounded_resource_without_live_reference", true); err != nil {
				return nil, err
			}
			ids = []string{id}
			record.SemanticRouteIDs = &ids
			record.AlignmentStatus = "explicit_release_variant_resource"
			dormant++
		} else {
			unclassified = append(unclassified, record.ID)
		}
	}
	menuIDs := map[string]bool{}
	for _, record := range menu.Segments {
		menuIDs[record.ID] = true
	}
	routes := make([]*NativeSemanticRoute, 0, len(r.routes))
	for _, route := range r.routes {
		record := r.records[route.RecordID]
		operations := record.Operations
		if route.SourceOffset != 0 {
			if menuIDs[record.ID] {
				return nil, fmt.Errorf("fixed-composer route %s enters inside %s", route.ID, record.ID)
			}
			decoded, err := b.d.DecodeRecord(Interactive, record.start+route.SourceOffset, record.end, true)
			if err != nil {
				return nil, err
			}
			if !decoded.Terminated {
				return nil, fmt.Errorf("route %s has no invocation boundary", route.ID)
			}
			operations = decoded.Operations
		}
		hash, err := operationHash(operations)
		if err != nil {
			return nil, err
		}
		route.Operations, route.OperationCount, route.OperationSHA256 = operations, len(operations), hash
		routes = append(routes, route)
	}
	slices.SortFunc(routes, func(a, c *NativeSemanticRoute) int { return strings.Compare(a.ID, c.ID) })
	slices.Sort(unclassified)
	complete := len(unknown) == 0 && len(unclassified) == 0
	status := "unresolved_logical_routes"
	if complete {
		status = "all_logical_routes_locally_verified"
	}
	return &NativeSemanticCatalog{Status: status, Complete: complete, IdentityUnit: "logical_consumer_route_not_physical_record_ordinal", RouteCount: len(routes), RoutedRecordCount: len(routed), DormantCount: dormant,
		UnresolvedCount: len(unknown), UnresolvedIndices: unknown, UnclassifiedCount: len(unclassified), UnclassifiedIDs: unclassified, Routes: routes}, nil
}

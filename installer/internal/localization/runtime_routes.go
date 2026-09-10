package localization

import (
	"fmt"
	"slices"
	"strconv"
	"strings"
)

// USRuntimeDialogueRoutes generates address-only adapter metadata from fresh
// Go discovery. No source prose is included, and no extraction JSON is trusted
// as an executable address inventory. Geometry remains owned by the game.
func (d *Decoder) USRuntimeDialogueRoutes() (IRObject, error) {
	if d == nil || d.profile.ID != "us" {
		return nil, fmt.Errorf("runtime routes require the exact US ROM")
	}
	catalog, err := d.BuildNativeCatalog()
	if err != nil {
		return nil, err
	}
	census := catalog.source
	wrappers := irRows(census.DialogueForwarding, "wrappers")
	var wrapperCalls []int
	nestedCall := 0
	for _, site := range irRows(census.Consumers[0], "call_sites") {
		call, err := parsePC(irString(site, "call_site"))
		if err != nil {
			return nil, err
		}
		switch irString(site, "source_origin") {
		case "dialogue_wrapper":
			wrapperCalls = append(wrapperCalls, call)
		case "nested_handler_table":
			if nestedCall != 0 {
				return nil, fmt.Errorf("ambiguous event dispatcher")
			}
			nestedCall = call
		}
	}
	if len(wrapperCalls) != len(wrappers) || nestedCall == 0 {
		return nil, fmt.Errorf("dialogue forwarding changed")
	}
	cities := catalogFacts.CityKeys
	matrices := irRows(census.NestedHandlerSources, "matrices")
	if len(matrices) != 1 {
		return nil, fmt.Errorf("event matrix changed")
	}
	selectors := map[string]int{}
	for _, row := range irRows(matrices[0], "rows") {
		city := row["city_slot"].(int)
		if city < 0 || city >= len(cities) {
			return nil, fmt.Errorf("event city outside range")
		}
		table, err := parsePC(irString(row, "source_table_pc24"))
		if err != nil {
			return nil, err
		}
		for slot := 0; slot < row["pointer_count"].(int); slot++ {
			selectors[fmt.Sprintf("simulation.event.%s.slot_%02d", cities[city], slot)] = table&0xffff + slot*2
		}
	}
	forwardedIndex, forwardedCall := -1, 0
	for index, wrapper := range wrappers {
		for _, site := range irRows(wrapper, "call_sites") {
			if irString(site, "source_origin") != "forwarded_y_from_source_relay" {
				continue
			}
			if forwardedIndex >= 0 {
				return nil, fmt.Errorf("ambiguous source relay")
			}
			forwardedIndex = index
			forwardedCall, err = parsePC(irString(site, "call_site"))
			if err != nil {
				return nil, err
			}
		}
	}
	if forwardedIndex < 0 {
		return nil, fmt.Errorf("source relay missing")
	}
	records := map[string]*NativeMessage{}
	for _, record := range append(slices.Clone(catalog.Messages), catalog.Menu.Segments...) {
		records[record.ID] = record
	}
	provenanceCall := func(route *NativeSemanticRoute, prefix string) (int, error) {
		for _, value := range route.Provenance {
			kind, pc, ok := strings.Cut(value, ":")
			if ok && strings.HasPrefix(kind, prefix) {
				return parsePC(pc)
			}
		}
		return 0, fmt.Errorf("%s lacks %s provenance", route.ID, prefix)
	}
	type runtimeRow struct {
		source, caller, context, selector, city int
		id                                      string
		units                                   []int
	}
	var rows []runtimeRow
	for _, route := range catalog.SemanticRoutes.Routes {
		if !slices.ContainsFunc([]string{"dialogue.event.", "dialogue.offering.", "sim.", "simulation.event.", "sky.", "system.", "variant.western.sim."}, func(prefix string) bool { return strings.HasPrefix(route.ID, prefix) }) ||
			!slices.Contains([]string{"consumer_route", "pointer_table_slot"}, route.Kind) ||
			!slices.ContainsFunc(route.Provenance, func(p string) bool {
				return strings.HasPrefix(p, "interpreter_") || strings.HasPrefix(p, "dialogue_wrapper_")
			}) {
			continue
		}
		record := records[route.RecordID]
		if record == nil || route.SourceOffset < 0 || route.SourceOffset >= record.end-record.start {
			return nil, fmt.Errorf("invalid runtime source %s", route.ID)
		}
		start := record.start + route.SourceOffset
		decoded, err := d.DecodeRecord(Interactive, start, record.end, true)
		if err != nil {
			return nil, fmt.Errorf("runtime page evidence %s: %w", route.ID, err)
		}
		expectedPages := 1
		for _, op := range route.Operations {
			if op["op"] == "page_break" {
				expectedPages++
			}
		}
		if !decoded.Terminated || len(decoded.nativePageUnits) != expectedPages || expectedPages > 8 ||
			slices.ContainsFunc(decoded.nativePageUnits, func(units int) bool { return units < 1 || units > 0xffff }) {
			return nil, fmt.Errorf("invalid runtime page evidence %s", route.ID)
		}
		r := runtimeRow{source: offsetPC(start), id: route.ID, selector: -1, city: -1, units: decoded.nativePageUnits}
		switch {
		case strings.HasPrefix(route.ID, "simulation.event."):
			selector, ok := selectors[route.ID]
			if !ok {
				return nil, fmt.Errorf("missing event selector %s", route.ID)
			}
			r.caller, r.selector = nestedCall+3, selector
		case strings.HasPrefix(route.ID, "dialogue.offering."):
			call, err := provenanceCall(route, "interpreter_offering_")
			if err != nil {
				return nil, err
			}
			slot, err := strconv.Atoi(route.ID[strings.LastIndexByte(route.ID, '_')+1:])
			if err != nil {
				return nil, err
			}
			r.caller, r.selector = call+3, slot*2
		case strings.HasPrefix(route.ID, "dialogue.event.relay."):
			width := 3
			if irString(wrappers[forwardedIndex], "call_kind") == "jsl" {
				width = 4
			}
			r.caller, r.context = wrapperCalls[forwardedIndex]+3, forwardedCall+width
			r.city = slices.Index(cities, route.ID[strings.LastIndexByte(route.ID, '.')+1:]) + 1
			if r.city == 0 {
				return nil, fmt.Errorf("unknown relay city")
			}
		case strings.HasPrefix(route.ID, "dialogue.event.wrapper_"):
			index, err := strconv.Atoi(strings.Split(strings.TrimPrefix(route.ID, "dialogue.event.wrapper_"), ".")[0])
			if err != nil || index < 0 || index >= len(wrappers) {
				return nil, fmt.Errorf("invalid wrapper id")
			}
			call, err := provenanceCall(route, "dialogue_wrapper_")
			if err != nil {
				return nil, err
			}
			width := 3
			if irString(wrappers[index], "call_kind") == "jsl" {
				width = 4
			}
			r.caller, r.context = wrapperCalls[index]+3, call+width
		default:
			call, err := provenanceCall(route, "interpreter_")
			if err != nil {
				return nil, err
			}
			r.caller = call + 3
		}
		rows = append(rows, r)
	}
	slices.SortFunc(rows, func(a, b runtimeRow) int {
		for _, pair := range [][2]int{{a.source, b.source}, {a.caller, b.caller}, {a.context, b.context}, {a.selector, b.selector}, {a.city, b.city}} {
			if pair[0] != pair[1] {
				return pair[0] - pair[1]
			}
		}
		return strings.Compare(a.id, b.id)
	})
	result := []IRObject{}
	seen := map[[5]int]bool{}
	for _, r := range rows {
		key := [5]int{r.source, r.caller, r.context, r.selector, r.city}
		if seen[key] {
			return nil, fmt.Errorf("ambiguous runtime identity %s", r.id)
		}
		seen[key] = true
		row := IRObject{"semantic_id": r.id, "source_pc24": pcString(r.source), "caller_pc24": pcString(r.caller), "native_page_units": r.units}
		if r.context != 0 {
			row["context_pc24"] = pcString(r.context)
		}
		if r.selector >= 0 {
			row["selector_x"] = r.selector
		}
		if r.city >= 0 {
			row["map_number"] = r.city
		}
		result = append(result, row)
	}
	if len(result) == 0 {
		return nil, fmt.Errorf("empty runtime route inventory")
	}
	return IRObject{"format": "actraiser-us-runtime-dialogue-routes", "version": 1, "source_profile": "us", "rom_sha256": d.profile.SHA256, "route_count": len(result), "routes": result}, nil
}

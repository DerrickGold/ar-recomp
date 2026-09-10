package localization

import (
	"reflect"
	"testing"
)

func TestCoverageOperationFindings(t *testing.T) {
	records := []*NativeMessage{{Operations: []Operation{
		{"op": "text", "value": "Hello"},
		{"op": "native_control"}, {"op": "native_control"},
		{"op": "insert_indexed_text"},
		{"op": "format_number", "value": "population"},
		{"op": "native_glyphs", "confidence": "unresolved_glyph", "codes": "A0"},
		{"op": "typed_future", "confidence": "unresolved_contract"},
		{"confidence": "unresolved_unknown"},
	}}}
	want := []IRObject{
		{"operation": "insert_indexed_text", "confidence": "not_declared", "count": 1},
		{"operation": "missing_operation", "confidence": "unresolved_unknown", "count": 1},
		{"operation": "native_control", "confidence": "not_declared", "count": 2},
		{"operation": "native_glyphs", "confidence": "unresolved_glyph", "count": 1},
		{"operation": "typed_future", "confidence": "unresolved_contract", "count": 1},
	}
	if actual := operationFindings(records); !reflect.DeepEqual(actual, want) {
		t.Fatal("unresolved operation ordering/ownership", actual)
	}
	for _, catalogs := range [][]*NativeCatalog{nil, {nil}, {{releaseID: "us", SemanticRoutes: &NativeSemanticCatalog{}}}} {
		if result, err := CheckNativeCoverage(catalogs...); err == nil || result != nil {
			t.Fatal("unverified evidence published coverage")
		}
	}
}

// Run on the independently compared retail catalogues as well as their full
// ROM mutations. One failed proof must not be hidden by every other green gate.
func checkCoverageBlockers(t *testing.T, catalogs []*NativeCatalog) {
	t.Helper()
	for _, id := range []string{"whole_rom_language_candidate_scan", "graphical_text_census", "text_consumer_reference_census", "dynamic_text_census", "unresolved_operations_or_glyphs"} {
		changed := *catalogs[0]
		switch id {
		case "whole_rom_language_candidate_scan":
			changed.Ownership = cloneIR(changed.Ownership).(IRObject)
			changed.Ownership["complete"] = false
		case "graphical_text_census":
			changed.Graphics = cloneIR(changed.Graphics).(IRObject)
			changed.Graphics["complete"] = false
		case "text_consumer_reference_census":
			destination := *changed.Destinations
			destination.ConsumerComplete = false
			changed.Destinations = &destination
		case "dynamic_text_census":
			changed.DynamicText = cloneIR(changed.DynamicText).(IRObject)
			changed.DynamicText["complete"] = false
		case "unresolved_operations_or_glyphs":
			changed.Messages = append([]*NativeMessage{}, changed.Messages...)
			changed.Messages = append(changed.Messages, &NativeMessage{AlignmentStatus: "logical_routes_verified", Operations: []Operation{{"op": "native_control"}}})
		}
		inputs := append([]*NativeCatalog{&changed}, catalogs[1:]...)
		reports, err := CheckNativeCoverage(inputs...)
		if err != nil {
			t.Fatal(err)
		}
		blockers := irRows(reports[0], "blockers")
		if reports[0]["complete"] != false || len(blockers) != 1 || blockers[0]["id"] != id {
			t.Fatal("coverage hid failed proof", id, reports[0])
		}
	}
}

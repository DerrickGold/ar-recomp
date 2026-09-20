package localization

import (
	"encoding/json"
	"reflect"
	"testing"
)

func TestSourceReferenceDiagnosticJSON(t *testing.T) {
	seed := NativeSourceReference{
		SourcePC24: "$00:8000", ReferenceKind: "interpreter_immediate_y",
		ViaCallSite: "$00:8100",
	}
	check := func(value any, expected string) {
		t.Helper()
		data, err := json.Marshal(value)
		if err != nil {
			t.Fatal(err)
		}
		var actual, want any
		if err := json.Unmarshal(data, &actual); err != nil {
			t.Fatal(err)
		}
		if err := json.Unmarshal([]byte(expected), &want); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(actual, want) {
			t.Fatalf("diagnostic JSON changed: %s; want %s", data, expected)
		}
	}
	check(seed, `{"source_pc24":"$00:8000","reference_kind":"interpreter_immediate_y","via_call_site":"$00:8100"}`)
	resolved, err := resolveReferences([]NativeSourceReference{seed}, []*NativeMessage{{ID: "example", start: 0, end: 2}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	check(resolved.References[0], `{"source_pc24":"$00:8000","reference_kind":"interpreter_immediate_y","via_call_site":"$00:8100","resolved_record_id":"example","source_offset_within_record":0}`)
	check(resolved.Summary, `{"unique_source_count":1,"mapped_unique_source_count":1,"unmapped_unique_source_count":0,"all_current_seeds_mapped":true,"unmapped_source_pc24s":[]}`)

	// Re-resolving against a different catalogue must clear the copied result,
	// preserve the previous result, and serialize unmapped IDs as explicit null.
	unmapped, err := resolveReferences(resolved.References, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	check(unmapped.References[0], `{"source_pc24":"$00:8000","reference_kind":"interpreter_immediate_y","via_call_site":"$00:8100","resolved_record_id":null}`)
	if *resolved.References[0].ResolvedRecordID != "example" || seed.NativeSourceResolution != nil {
		t.Fatal("resolution changed the input evidence")
	}
}

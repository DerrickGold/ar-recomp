package localization

import (
	"encoding/json"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

func TestNativeRelayFragmentSpacing(t *testing.T) {
	route := func(id, record, text string, offset int) *NativeSemanticRoute {
		return &NativeSemanticRoute{ID: id, RecordID: record, SourceOffset: offset, Category: "angel_dialogue",
			Operations: []Operation{{"op": "text", "value": text}, {"op": "end"}}}
	}
	first := route("dialogue.event.relay.fillmore", "packed", "First fragmentSecond fragmentClosing.", 0)
	second := route("dialogue.event.relay.bloodpool", "packed", "Second fragmentClosing.", 14)
	closing := route("dialogue.event.wrapper_02.call_00.source_00", "packed", "Closing.", 29)
	// A similar suffix in another physical record cannot introduce a break.
	other := route("dialogue.event.relay.aitos", "other", "fragmentClosing.", 6)
	routes := []*NativeSemanticRoute{closing, other, first, second}
	before, _ := json.Marshal(routes)
	layout := IRObject{"columns": 80, "space_delimited_words": true}
	messages, err := nativeAuthorMessages(routes, layout)
	if err != nil {
		t.Fatal(err)
	}
	var ops []AuthorOperation
	for _, message := range messages {
		if message.ID == first.ID {
			ops = message.Operations
		}
	}
	want := []AuthorOperation{{Op: "text", Value: "First fragment"}, {Op: "preferred_line"},
		{Op: "text", Value: "Second fragment"}, {Op: "preferred_line"}, {Op: "text", Value: "Closing."}, {Op: "end"}}
	if !slices.Equal(ops, want) {
		t.Fatalf("lost packed boundaries: %+v", ops)
	}
	after, _ := json.Marshal(routes)
	if string(before) != string(after) {
		t.Fatal("raw extraction evidence was changed")
	}
	for _, unknown := range []IRObject{nil, {"columns": 24, "space_delimited_words": false}} {
		if nativeAuthorFragmentBoundaries(first, routes, unknown) != first {
			t.Fatal("unprofiled native layout changed")
		}
	}
	corrupt := *second
	corrupt.Operations = []Operation{{"op": "text", "value": "Unrelated ending."}, {"op": "end"}}
	if nativeAuthorFragmentBoundaries(first, []*NativeSemanticRoute{first, &corrupt}, layout) != first {
		t.Fatal("offset alone was used to guess a boundary")
	}
	// Wide native fragments become separators, not mandatory fixed-width rows.
	narrow, err := nativeAuthorOperations(nativeAuthorFragmentBoundaries(first, routes, layout),
		IRObject{"columns": 12, "space_delimited_words": true})
	if err != nil {
		t.Fatal(err)
	}
	var text strings.Builder
	for _, op := range narrow {
		if op.Op == "text" {
			text.WriteString(op.Value)
		}
	}
	if text.String() != "First fragment Second fragment Closing." {
		t.Fatal(narrow)
	}
}

func TestNativeRelaySpacingUpgradePreservesEditsAndAppearance(t *testing.T) {
	style := texttemplate.Style{Scale: 125, Font: "body"}
	original := AuthorMessage{ID: "dialogue.event.relay.fillmore", Appearance: AuthorTextAppearance{Layout: "flow"},
		Operations: []AuthorOperation{{Op: "text", Value: "First fragmentSecond fragmentClosing.", Style: style}, {Op: "end"}}}
	fresh := AuthorMessage{ID: original.ID, Operations: []AuthorOperation{{Op: "text", Value: "First fragment"},
		{Op: "preferred_line"}, {Op: "text", Value: "Second fragment Closing."}, {Op: "end"}}}
	upgraded, ok := nativeRelaySpacingUpgrade(original, fresh)
	if !ok || upgraded.Appearance != original.Appearance || upgraded.Operations[0].Style != style || upgraded.Operations[2].Style != style {
		t.Fatal("spacing repair lost appearance", upgraded)
	}
	if _, ok := nativeRelaySpacingUpgrade(upgraded, fresh); ok {
		t.Fatal("spacing repair was repeated")
	}
	edited := original
	edited.Operations = slices.Clone(original.Operations)
	edited.Operations[0].Value = "Custom wording."
	if _, ok := nativeRelaySpacingUpgrade(edited, fresh); ok {
		t.Fatal("edited baseline wording was replaced")
	}
	plain := original
	plain.Operations = slices.Clone(original.Operations)
	plain.Operations[0].Value = "OneTwo"
	simple := AuthorMessage{ID: plain.ID, Operations: []AuthorOperation{{Op: "text", Value: "One Two"}, {Op: "end"}}}
	got, ok := nativeRelaySpacingUpgrade(plain, simple)
	if !ok || got.Operations[0].Value != "One Two" {
		t.Fatal("space-only repair skipped")
	}
	if _, ok := nativeRelaySpacingUpgrade(got, simple); ok {
		t.Fatal("space-only repair was repeated")
	}
	if !reflect.DeepEqual(original.Operations[0], AuthorOperation{Op: "text", Value: "First fragmentSecond fragmentClosing.", Style: style}) {
		t.Fatal("original was mutated")
	}
}

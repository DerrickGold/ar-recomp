package localization

import (
	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
	"reflect"
	"slices"
	"strings"
	"testing"
)

func TestNativeAbilityReferences(t *testing.T) {
	cases := []struct{ id, text, name string }{
		{"sim.miracle.lightning.description", "A lightning bolt; lightning!", "miracle.lightning.name"},
		{"sim.miracle.rain.confirm", "Use rain?", "miracle.rain.name"},
		{"sim.miracle.sun.description", "The sunlight and sun.", "miracle.sun.name"},
		{"sim.miracle.wind.confirm", "Use wind?", "miracle.wind.name"},
		{"sim.miracle.earthquake.confirm", "Use earthquake?", "miracle.earthquake.name"},
		{"sky.magic.selected.fire", "Take Magical Fire.", "spell.fire.name"},
		{"sky.magic.selected.stardust", "Take Magical Stardust.", "spell.stardust.name"},
		{"sky.magic.selected.aura", "Take Magical Aura.", "spell.aura.name"},
		{"sky.magic.selected.light", "Take Magical Light.", "spell.light.name"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			style := texttemplate.Style{Scale: 125}
			ops := []AuthorOperation{{Op: "anchor", ID: "reset_text_cursor.00"}, {Op: "text", Value: tc.text, Style: style}, {Op: "end"}}
			after := nativeAbilityReferences(ops, tc.id)
			found := false
			for _, op := range after {
				if op.Op == "placeholder" {
					found = true
					if op.Name != tc.name || op.Style != style {
						t.Fatal("incorrect reference or lost styling", op)
					}
				}
			}
			if !found {
				t.Fatal("literal name remains")
			}
			if !reflect.DeepEqual(after, nativeAbilityReferences(after, tc.id)) {
				t.Fatal("non-idempotent normalization")
			}
			if ops[1].Value != tc.text {
				t.Fatal("mutated source")
			}
		})
	}
	soft := []AuthorOperation{{Op: "text", Value: "Take Magical "}, {Op: "preferred_line"}, {Op: "text", Value: "Fire. With this magic."}, {Op: "page"}, {Op: "text", Value: "Flames."}, {Op: "end"}}
	after := nativeAbilityReferences(soft, "sky.magic.selected.fire")
	if len(after) != 6 || after[1].Name != "spell.fire.name" || after[2].Value != ". With this magic." || after[3].Op != "page" {
		t.Fatal("soft-wrap conversion damaged content/control", after)
	}
	weather := []AuthorOperation{{Op: "text", Value: "The sun and rain after lightning."}, {Op: "end"}}
	if !reflect.DeepEqual(weather, nativeAbilityReferences(weather, "simulation.event.kasandora.slot_08")) {
		t.Fatal("weather prose changed")
	}
	verb := []AuthorOperation{{Op: "text", Value: "Magical Stardust makes stars rain down."}, {Op: "end"}}
	after = nativeAbilityReferences(verb, "sky.magic.selected.stardust")
	if after[1].Value != " makes stars rain down." {
		t.Fatal("verb changed", after)
	}
}
func TestAbilityNameReferenceMetadata(t *testing.T) {
	refs, err := AuthorReferences("us")
	if err != nil {
		t.Fatal(err)
	}
	for _, row := range abilityNames {
		if AbilityNameSource(row.Placeholder) != row.MessageID {
			t.Fatal(row)
		}
		found := false
		for _, ref := range refs {
			if len(ref.Placeholders) > 32 {
				t.Fatal("dialogue value budget exceeded", ref.ID)
			}
			for _, p := range ref.Placeholders {
				if p.Name == row.Placeholder {
					found = true
					if p.Kind != "localized_text" || p.SourceMessage != row.MessageID {
						t.Fatal(p)
					}
				}
			}
		}
		if !found {
			t.Fatal("name absent from picker", row.Placeholder)
		}
		route := authorContracts.routes[row.MessageID]
		if slices.Contains(route.Allowed, row.Placeholder) {
			t.Fatal("self-referencing menu name allowed")
		}
	}
	if strings.Contains(AbilityNameDefault("spell.fire.name"), "Sample") {
		t.Fatal("placeholder sample")
	}
}

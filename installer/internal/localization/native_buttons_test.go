package localization

import (
	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
	"reflect"
	"slices"
	"testing"
)

func TestNativeButtonReferences(t *testing.T) {
	id := "dialogue.event.wrapper_05.call_00.source_00"
	for _, text := range []string{"B / Y / START", `Knopf "B", Knopf "Y", Startknopf`, "Bボタン Yボタン START"} {
		style := texttemplate.Style{Scale: 120}
		ops := []AuthorOperation{{Op: "text", Value: text, Style: style}, {Op: "anchor", ID: "yield.01"}, {Op: "end"}}
		after := nativeButtonReferences(ops, id)
		var names []string
		for _, op := range after {
			if op.Op == "placeholder" {
				names = append(names, op.Name)
				if op.Style != style {
					t.Fatal("lost style")
				}
			}
		}
		if !slices.Equal(names, []string{"icon.button.b", "icon.button.y", "icon.button.start"}) {
			t.Fatal(names)
		}
		if !reflect.DeepEqual(after, nativeButtonReferences(after, id)) {
			t.Fatal("not idempotent")
		}
		if !reflect.DeepEqual(ops, nativeButtonReferences(ops, "sky.magic.selected.fire")) {
			t.Fatal("rewrote unrelated prose")
		}
	}
}
func TestHelpAndCommonButtonReferences(t *testing.T) {
	messages := nativeHelpMessages()
	if len(messages) != 36 {
		t.Fatal(len(messages))
	}
	script, err := EmitAuthorScript(messages, "help.artext")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = NewAuthorWorkspace("us", "partial", map[string]string{script.Path(): script.Text()}, ""); err != nil {
		t.Fatal(err)
	}
	refs, err := AuthorReferences("us")
	if err != nil {
		t.Fatal(err)
	}
	for _, ref := range refs {
		if ref.Presentation.Shape != "flow" {
			continue
		}
		for _, key := range []string{"b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r", "describe"} {
			if !slices.ContainsFunc(ref.Placeholders, func(value AuthorPlaceholder) bool {
				return value.Name == "button."+key && value.Kind == "localized_text"
			}) ||
				!slices.ContainsFunc(ref.Placeholders, func(value AuthorPlaceholder) bool { return value.Name == "icon.button."+key && value.Kind == "icon" }) {
				t.Fatal("incomplete button contract", ref.ID, key)
			}
		}
		if len(ref.Placeholders) > 32 {
			t.Fatal("dialogue value limit exceeded", ref.ID)
		}
	}
	locations := AuthorMessageLocations("sim.help.category.2")
	if len(locations) != 6 || locations[0].GroupKey != "group.help" {
		t.Fatal(locations)
	}
}

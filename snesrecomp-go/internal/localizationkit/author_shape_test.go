package localizationkit

import (
	"fmt"
	"os"
	"strings"
	"testing"
)

func TestUnicodeSeparatorsCannotBypassFixedFieldContracts(t *testing.T) {
	for _, separator := range []string{"\u0085", "\u2028", "\u2029"} {
		for _, id := range []string{"action.hud.player_label", "title.mode_select.with_save", "status.report.master_report"} {
			sources := map[string]string{"text/test.artext": ":: " + id + "\nFirst" + separator + "Second\n@end\n"}
			if _, err := NewAuthorWorkspace("us", "partial", sources, ""); err == nil || !strings.Contains(err.Error(), "line-separator controls") {
				t.Fatal("separator bypassed fixed shape", id)
			}
			if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
				authorRuntimeCheck(t, probe, "us", "partial", sources)
			}
		}
	}
}

func TestAuthorTableShapes(t *testing.T) {
	cases := []struct{ id, body, diagnostic string }{
		{"status.report.cities_report", "Town | {city_fillmore_population} | {city_fillmore_growth_state} | {city_fillmore_level} {city_fillmore_items}\n", "row 1 has 4 field(s)"},
		{"status.report.cities_report", "Town | {city_fillmore_population} | {city_fillmore_growth_state} | {city_fillmore_level} | {city_fillmore_items}\n", ""},
		{"status.report.score_report", "Name | ACT 1 | ACT 2 | Extra\n", "row 1 has 4 field(s)"},
		{"status.report.master_report", "LV | {master_level} | HP | {master_hp}\n", "row 1 has 4 field(s)"},
		{"status.report.master_report", "{master_name}\n@line\n@line\n@line\nLV | {master_level} | HP | {master_hp}\n", ""},
		{"status.report.master_report", "First\n" + strings.Repeat("@line\n", 17) + "Past the field\n", "row 18 has 1 field(s)"},
		{"status.report.cities_report", "Heading\n" + strings.Repeat("@line\n", 5) + "|||\n", ""}, // native divider
		{"system.message_speed.scale_labels", "0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9\n@line\n@line\nFast | {icon.ui.speed_direction} | Slow\n", ""},
		{"system.message_speed.scale_labels", "0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8\n", "row 1 has 9 field(s)"},
		{"system.message_speed.scale_labels", "Fast | {icon.ui.speed_direction} | Slow\n", "row 1 has 3 field(s)"},
		{"sky.menu.magic.fire", "Fire | extra\n", "row 1 has 2 field(s)"},
		{"sky.menu.magic.fire", "@empty\n", ""},
		{"dialogue.event.relay.aitos", "This | is | flowing | dialogue\n@page\nMore\n", ""},
	}
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	for i, tc := range cases {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			source := ":: " + tc.id + "\n" + tc.body
			sources := map[string]string{"text/test.artext": source}
			_, err := NewAuthorWorkspace("us", "partial", sources, "")
			if tc.diagnostic == "" && err != nil || tc.diagnostic != "" && (err == nil || !strings.Contains(err.Error(), tc.diagnostic)) {
				t.Fatalf("expected %q, got %v", tc.diagnostic, err)
			}
			if probe != "" {
				authorRuntimeCheck(t, probe, "us", "partial", sources)
			}
		})
	}
	// A target's shape applies through aliases too; a valid score row must not
	// allow the same four-field source to escape a Cities consumer's checks.
	alias := ":: status.report.cities_report\n@alias status.report.master_report\n:: status.report.master_report\nName\n@line\n@line\n@line\nLV | Level | HP | Health\n"
	if _, err := NewAuthorWorkspace("us", "partial", map[string]string{"a": alias}, ""); err == nil || !strings.Contains(err.Error(), "table row 4 has 4 field(s)") {
		t.Fatal("unsupported alias accepted")
	}
	if probe != "" {
		authorRuntimeCheck(t, probe, "us", "partial", map[string]string{"text/test.artext": alias})
	}
}

func TestAuthorRegionalSpeedShape(t *testing.T) {
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	for _, profile := range []string{"us", "eu-en", "de", "fr", "jp"} {
		for _, count := range []int{8, 10} {
			source := ":: system.message_speed.scale_labels\n" + strings.Repeat("tick | ", count-1) + "tick\n"
			sources := map[string]string{"text/test.artext": source}
			_, err := NewAuthorWorkspace(profile, "partial", sources, "")
			valid := (profile == "jp") == (count == 8)
			if (err == nil) != valid {
				t.Fatalf("%s/%d: %v", profile, count, err)
			}
			if probe != "" {
				authorRuntimeCheck(t, probe, profile, "partial", sources)
			}
		}
	}
}

func TestAuthorRowRulesAreDetached(t *testing.T) {
	refs, _ := AuthorReferences("us")
	for _, ref := range refs {
		if ref.Presentation.Table == nil {
			continue
		}
		ref.Presentation.Table.Kind = "poison"
		for i := range ref.Presentation.Table.Rules {
			ref.Presentation.Table.Rules[i].FirstLine = 255
			if len(ref.Presentation.Table.Rules[i].Fields) > 0 {
				ref.Presentation.Table.Rules[i].Fields[0] = 10
			}
		}
	}
	if _, err := NewAuthorWorkspace("us", "partial", map[string]string{"a": ":: status.report.master_report\nName\n"}, ""); err != nil {
		t.Fatal("mutable author rules escaped", err)
	}
	refs, _ = AuthorReferences("us")
	for _, ref := range refs {
		if ref.Presentation.Table != nil && ref.Presentation.Table.Kind == "poison" {
			t.Fatal("mutable table escaped")
		}
	}
}

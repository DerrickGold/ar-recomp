package localization

import (
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

func TestVersionedTemplateAppearance(t *testing.T) {
	const source = ":: example.dialogue\n@layout flow\n@font body\n@style retail\n@scale 110%\n@numerals upright\nHP <i>{hp:02}</i> <span font=\"hud\" scale=\"80%\">gold</span> \\<literal>\n@end\n"
	script, err := ParseAuthorScriptVersion(source, "message.artext", 2)
	if err != nil {
		t.Fatal(err)
	}
	m := script.Messages()[0]
	if m.Appearance.Layout != "flow" || m.Appearance.Style.Scale != 110 || m.Operations[1].Style.Italic != 2 || m.Operations[3].Style.Font != "hud" {
		t.Fatalf("appearance lost: %#v", m)
	}
	emitted, err := EmitAuthorScriptVersion(script.Messages(), "emitted.artext", 2)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(authorPresentation(m.Operations), authorPresentation(emitted.Messages()[0].Operations)) {
		t.Fatal("emission changed styling")
	}
	if _, err := script.ReplaceBody(m.ID, "<span color=\"#FFD36A\">new</span>\n@end\n"); err != nil {
		t.Fatal(err)
	}
	if _, err := ParseAuthorScript(source, "v1.artext"); err == nil {
		t.Fatal("v1 accepted new commands")
	}
	legacy, err := ParseAuthorScript(":: example.dialogue\n<i>literal</i>\n@end\n", "v1.artext")
	if err != nil {
		t.Fatal(err)
	}
	if got := legacy.Messages()[0].Operations[0]; got.Value != "<i>literal</i>" || got.Style != (texttemplate.Style{}) {
		t.Fatal("legacy text was reinterpreted")
	}
}

func TestPresentationDefaultsAreUnambiguous(t *testing.T) {
	for _, body := range []string{
		"@font body\n@font hud\nx\n", "x\n@font body\n", "@scale 401%\nx\n",
		"@numerals mystery\nx\n", "@font body\n@alias other\n", "<i>unclosed\n@end\n",
	} {
		if _, err := ParseAuthorScriptVersion(":: example.dialogue\n"+body, "fixture", 2); err == nil {
			t.Fatalf("accepted %q", body)
		}
	}
}

func TestStyledSoftLinesKeepScopeUntilStructure(t *testing.T) {
	source := ":: example.dialogue\n<i>first\n# comment\nsecond {value}</i>\n@line\nnext\n"
	script, err := ParseAuthorScriptVersion(source, "soft.artext", 2)
	if err != nil {
		t.Fatal(err)
	}
	ops := script.messages[0].Operations
	if ops[0].Value != "first second " || ops[0].Style.Italic != 2 || ops[1].SourceLine != 4 || ops[1].Style.Italic != 2 || ops[3].Style.Italic != 0 {
		t.Fatalf("soft lines changed presentation: %#v", ops)
	}
	for _, separator := range []string{"@line", "@page", "", "@wait 4"} {
		if _, err := ParseAuthorScriptVersion(":: example.dialogue\n<i>first\n"+separator+"\nsecond</i>\n", "bad.artext", 2); err == nil {
			t.Fatal("style crossed explicit structure")
		}
	}
	if _, err := ParseAuthorScriptVersion(":: example.dialogue\nfirst\n<span color=\"bad\">second</span>\n", "bad.artext", 2); err == nil || !strings.Contains(err.Error(), "bad.artext:3:") {
		t.Fatalf("lost source line: %v", err)
	}
}

func TestLayoutContractAndAliasCompatibility(t *testing.T) {
	for _, tc := range []struct{ script, want string }{
		{":: title.copyright\n@layout centered_block\nFirst\n@line\nSecond\n@end\n", ""},
		{":: title.copyright\n@layout single_line_label\nFirst\n@line\nSecond\n@end\n", ""},
		{":: title.start_prompt\n@layout centered_block\nStart\n@end\n", "requires layout"},
		{":: action.hud.ready\n@layout centered_label\nReady\n@end\n:: action.hud.pause\n@alias action.hud.ready\n", ""},
		{":: action.hud.ready\n@layout mystery\nReady\n@end\n", "requires layout \"centered_label\""},
		{":: action.hud.ready\n@layout centered_label\nReady\n@end\n:: title.start_prompt\n@alias action.hud.ready\n", "title.start_prompt requires layout \"single_line_label\""},
	} {
		_, err := NewAuthorWorkspaceVersion("us", "partial", map[string]string{"layouts.artext": tc.script}, "", 2)
		if tc.want == "" {
			if err != nil {
				t.Fatal(err)
			}
		} else if err == nil || !strings.Contains(err.Error(), tc.want) || !strings.Contains(err.Error(), "layouts.artext:1:") {
			t.Fatalf("expected located %q, got %v", tc.want, err)
		}
	}
}

func TestGameAppearanceBudgets(t *testing.T) {
	for _, test := range []struct {
		name string
		body string
		want string
	}{
		{"appearances", func() string {
			var s strings.Builder
			for i := 0; i < 65; i++ {
				fmt.Fprintf(&s, `<span color="#%06X">x</span>`, i)
			}
			return s.String()
		}(), "64 distinct appearances"},
		{"spans", strings.Repeat(`<span color="#ffffff">x</span><span color="#000000">y</span>`, 257), "512 style spans"},
		{"variants", func() string {
			var s strings.Builder
			for i := 100; i < 133; i++ {
				if i == 116 {
					s.WriteString("\n@page\n")
				}
				fmt.Fprintf(&s, `<span scale="%d%%">x</span>`, i)
			}
			return s.String()
		}(), "32 font/size/italic"},
	} {
		t.Run(test.name, func(t *testing.T) {
			script, err := ParseAuthorScriptVersion(":: dialogue.event.relay.aitos\n"+test.body+"\n@end\n", "text/test.artext", 2)
			if err != nil {
				t.Fatal(err)
			}
			_, err = newAuthorWorkspace("us", "partial", []*AuthorScript{script}, "")
			if err == nil || !strings.Contains(err.Error(), test.want) {
				t.Fatalf("expected %s, got %v", test.want, err)
			}
		})
	}
}

package localizationkit

import (
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"slices"
	"strings"
	"testing"
)

func nativeAuthorRoute(category string, ops ...Operation) *NativeSemanticRoute {
	return &NativeSemanticRoute{ID: "action.hud.act_1", Category: category, Operations: ops}
}

func TestNativeSoundTestColumns(t *testing.T) {
	route := &NativeSemanticRoute{ID: "sound_test.menu.labels", Category: "sound_test_menu", Operations: []Operation{
		{"op": "text", "value": "Sound test"}, {"op": "line_break"}, {"op": "line_break"},
		{"op": "text", "value": " Music "}, {"op": "format_number", "value": "sound_music_id", "width": 2},
		{"op": "line_break"}, {"op": "line_break"},
		{"op": "text", "value": " Effect "}, {"op": "format_number", "value": "sound_effect_id", "width": 2}, {"op": "end"},
	}}
	ops, err := nativeAuthorOperations(route, nil)
	if err != nil {
		t.Fatal(err)
	}
	script, err := EmitAuthorScript([]AuthorMessage{{ID: route.ID, Operations: ops}}, "text/sound.artext")
	if err != nil || strings.Count(script.Text(), "|") != 2 {
		t.Fatal("missing explicit counter cells", err)
	}
	if _, err := NewAuthorWorkspace("us", "partial", map[string]string{script.Path(): script.Text()}, ""); err != nil {
		t.Fatal(err)
	}
	for _, body := range []string{
		"Heading\n@line\n@line\nMusic | {sound_music_id:02}\n@line\n@line\nEffect | {sound_effect_id:02}\n",
		"Heading\n@line\n@line\nMusic {sound_music_id:02}\n@line\n@line\nEffect {sound_effect_id:02}\n",
	} {
		source := ":: sound_test.menu.labels\n" + body
		if _, err := NewAuthorWorkspace("us", "partial", map[string]string{"text/sound.artext": source}, ""); err != nil {
			t.Fatal("valid split or legacy counter row rejected", err)
		}
		if _, err := NewAuthorWorkspace("us", "partial", map[string]string{"text/sound.artext": source + "@line\nextra\n"}, ""); err == nil {
			t.Fatal("sound-test text can escape its native rows")
		}
	}
}

func TestNativeAuthorWrappingAndControls(t *testing.T) {
	text := func(value string) Operation { return Operation{"op": "text", "value": value} }
	line, end := Operation{"op": "line_break"}, Operation{"op": "end"}
	layout := IRObject{"columns": 12, "space_delimited_words": true}
	for _, tc := range []struct {
		name, category string
		before, after  []Operation
		soft           bool
	}{
		{"overflow", "angel_dialogue", []Operation{text("ABCDEFGHI")}, []Operation{text("next word")}, true},
		{"exact-fit", "angel_dialogue", []Operation{text("ABCDEFG")}, []Operation{text("next word")}, false},
		{"short", "angel_dialogue", []Operation{text("Sir Dude")}, []Operation{text("Go")}, false},
		{"unknown-name", "angel_dialogue", []Operation{text("Sir "), {"op": "insert_master_name"}}, []Operation{text("next word")}, false},
		{"unknown-lookup", "angel_dialogue", []Operation{text("ABCDEFGHI")}, []Operation{{"op": "insert_indexed_text", "value": "selected_town"}}, false},
		{"decimal", "town_dialogue", []Operation{text("ABCDEFG"), {"op": "format_number", "value": "master_level", "width": 3}}, []Operation{text("next")}, true},
		{"bcd-unknown", "town_dialogue", []Operation{text("ABCDEFG"), {"op": "format_number", "value": "total_score", "width": 0x83}}, []Operation{text("next")}, false},
		{"icon", "angel_dialogue", []Operation{text("ABCDEFG"), {"op": "insert_icon", "value": "ui.heart"}}, []Operation{text("next")}, true},
		{"native-accent", "angel_dialogue", []Operation{text("e\u0301ABCDEF")}, []Operation{text("next")}, false},
		{"unknown-mark", "angel_dialogue", []Operation{text("ABCDEFGHI\ufe0f")}, []Operation{text("next")}, false},
		{"ending-preserved", "ending_text", []Operation{text("ABCDEFGHI")}, []Operation{text("next")}, false},
		{"post-offering-preserved", "post_offering_or_ending_native", []Operation{text("ABCDEFGHI")}, []Operation{text("next")}, false},
		{"menu-preserved", "fixed_composer", []Operation{text("ABCDEFGHI")}, []Operation{text("next")}, false},
		{"no-next-word", "angel_dialogue", []Operation{text("ABCDEFGHI")}, nil, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ops := append(append(append([]Operation{}, tc.before...), line), tc.after...)
			ops = append(ops, end)
			route := nativeAuthorRoute(tc.category, ops...)
			before, _ := json.Marshal(route)
			converted, err := nativeAuthorOperations(route, layout)
			if err != nil {
				t.Fatal(err)
			}
			hasLine := slices.ContainsFunc(converted, func(op AuthorOperation) bool { return op.Op == "line" })
			if hasLine == tc.soft {
				t.Fatalf("line intent changed: %+v", converted)
			}
			after, _ := json.Marshal(route)
			if string(before) != string(after) {
				t.Fatal("source was mutated")
			}
			if tc.soft {
				for _, unknown := range []IRObject{nil, {"columns": 12}, {"columns": 12, "space_delimited_words": false}, {"columns": 0, "space_delimited_words": true}} {
					if !nativeHardBreaks(route, unknown)[len(tc.before)] {
						t.Fatal("unprofiled/JP geometry reflowed")
					}
				}
			}
		})
	}
	// Leading/trailing placeholders must not consume their adjacent separators;
	// page/yield/reset boundaries cannot borrow words from the next page.
	route := nativeAuthorRoute("angel_dialogue", Operation{"op": "reset_text_cursor"}, Operation{"op": "insert_master_name"}, text(" ,  go"), line, Operation{"op": "page_break"}, text(" now "), Operation{"op": "format_number", "value": "master_level", "width": 3}, Operation{"op": "delay", "frames": 30}, Operation{"op": "toggle_text_state", "native_control": "0xFF"}, Operation{"op": "yield"}, end)
	ops, err := nativeAuthorOperations(route, layout)
	if err != nil {
		t.Fatal(err)
	}
	want := []AuthorOperation{{Op: "anchor", ID: "reset_text_cursor.00"}, {Op: "placeholder", Name: "master_name"}, {Op: "text", Value: " , go"}, {Op: "line"}, {Op: "page"}, {Op: "text", Value: " now "}, {Op: "placeholder", Name: "master_level", MinimumDigits: 3}, {Op: "anchor", ID: "delay.01"}, {Op: "anchor", ID: "toggle_text_state.02"}, {Op: "anchor", ID: "yield.03"}, {Op: "end"}}
	if !slices.Equal(ops, want) {
		t.Fatalf("lost boundary/placeholder/control: %+v", ops)
	}
}

func TestNativeAuthorTablesIconsAndPostLayoutAliases(t *testing.T) {
	text := func(value string) Operation { return Operation{"op": "text", "value": value} }
	icon := func(part int) Operation {
		return Operation{"op": "insert_icon", "value": "ui.speed_direction", "part_index": part, "part_count": 2}
	}
	line, end := Operation{"op": "line_break"}, Operation{"op": "end"}
	base := []Operation{text("1 2 3 4"), line, text("FAST"), icon(0), icon(1), text("SLOW"), end}
	// Digit source padding is separate from the native direction's two tiles.
	base[0] = text(" 1234 ")
	for _, id := range []string{"system.message_speed.scale_labels", "action.hud.act_1"} {
		r := nativeAuthorRoute("fixed_composer", base...)
		r.ID = id
		ops, err := nativeAuthorOperations(r, nil)
		if err != nil {
			t.Fatal(err)
		}
		if count := len(slices.DeleteFunc(append([]AuthorOperation{}, ops...), func(op AuthorOperation) bool { return op.Op != "placeholder" })); count != 1 {
			t.Fatal("multi-tile icon duplicated")
		}
		want := " 1234 "
		if id == "system.message_speed.scale_labels" {
			want = "1 | 2 | 3 | 4"
		}
		if ops[0].Value != want {
			t.Fatalf("wrong digit cells: %+v", ops)
		}
	}
	generic := nativeAuthorRoute("fixed_composer", base...)
	alias := *generic
	alias.ID = "action.hud.act_2"
	speed := *generic
	speed.ID = "system.message_speed.scale_labels"
	messages, err := nativeAuthorMessages([]*NativeSemanticRoute{&speed, &alias, generic}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if messages[0].ID != generic.ID || messages[1].Alias != generic.ID || messages[2].Alias != "" {
		t.Fatal("aliasing occurred before table conversion", messages)
	}

	// Continued population title keeps multiword labels; typed totals remain a
	// separate cell even with only one dictionary-emitted space before the value.
	report := []AuthorOperation{{Op: "text", Value: "POPULATION DU"}, {Op: "line"}, {Op: "text", Value: "  MONDE  ENTIER "}, {Op: "placeholder", Name: "total_population"}, {Op: "line"}, {Op: "text", Value: "TOWN   LEVEL"}, {Op: "line"}, {Op: "text", Value: "A   "}, {Op: "placeholder", Name: "master_level", MinimumDigits: 3}, {Op: "end"}}
	want := []AuthorOperation{{Op: "text", Value: "POPULATION DU"}, {Op: "line"}, {Op: "text", Value: "MONDE ENTIER"}, {Op: "text", Value: " | "}, {Op: "placeholder", Name: "total_population"}, {Op: "line"}, {Op: "text", Value: "TOWN   LEVEL"}, {Op: "line"}, {Op: "text", Value: "A"}, {Op: "text", Value: " | "}, {Op: "placeholder", Name: "master_level", MinimumDigits: 3}, {Op: "end"}}
	if got := nativeAuthorTable(report, "status.report.population_report"); !slices.Equal(got, want) {
		t.Fatalf("report fields changed: %+v", got)
	}
	for _, id := range []string{"status.report.master_report", "status.report.score_report"} {
		var rows []AuthorOperation
		for i := 0; i < 10; i++ {
			rows = append(rows, AuthorOperation{Op: "text", Value: "A  B C"}, AuthorOperation{Op: "line"})
		}
		rows = append(rows, AuthorOperation{Op: "end"})
		got := nativeAuthorTable(rows, id)
		var rendered strings.Builder
		for _, op := range got {
			if op.Op == "text" {
				rendered.WriteString(op.Value)
			}
			if op.Op == "line" {
				rendered.WriteByte('\n')
			}
		}
		lines := strings.Split(rendered.String(), "\n")
		if id == "status.report.master_report" {
			for i := 0; i < 10; i++ {
				expected := "A  B C"
				if i == 3 || i == 5 || i == 7 || i == 9 {
					expected = "A | B | C"
				}
				if lines[i] != expected {
					t.Fatalf("master row %d: %q", i, lines[i])
				}
			}
		} else if lines[3] != "A | B C" || lines[2] != "A  B C" || lines[5] != "A  B C" {
			t.Fatal("score column roles changed", lines)
		}
	}
}

func TestAuthorEmitterRoundTripAndRejection(t *testing.T) {
	source := ":: action.hud.act_1\n@@{{literal}} @inline\n@line\n\\#literal\n@paragraph\n\\;literal\n@page\n{master_level:03}\n@wait 30\n@anchor yield.00\n@end\n\n:: action.hud.act_2\n@alias action.hud.act_1\n"
	s, err := ParseAuthorScript(source, "source.artext")
	if err != nil {
		t.Fatal(err)
	}
	emitted, err := EmitAuthorScript(s.Messages(), s.Path())
	if err != nil {
		t.Fatal(err)
	}
	again, err := EmitAuthorScript(emitted.Messages(), s.Path())
	if err != nil || emitted.Text() != again.Text() {
		t.Fatal("non-canonical repeated emission", err)
	}
	for _, value := range []string{":: injected", "  # swallowed", "  @line", "\\#removed", "\\;removed", "\n:: injected", "\r", "\x00", "\xff", "   ", strings.Repeat("x", MaxAuthorTextBytes+1)} {
		if got, err := EmitAuthorScript([]AuthorMessage{{ID: "action.hud.act_1", Operations: []AuthorOperation{{Op: "text", Value: value}, {Op: "end"}}}}, "bad"); err == nil || got != nil {
			t.Fatalf("lossy text emitted: %q", value[:min(len(value), 30)])
		}
	}
	for _, ops := range [][]AuthorOperation{
		{{Op: "text", Value: "x", Name: "unexpected"}, {Op: "end"}}, {{Op: "placeholder", Name: "x", MinimumDigits: 10}}, {{Op: "native_control"}}, {{Op: "text", Value: "x"}, {Op: "end"}, {Op: "text", Value: "y"}}, {{Op: "anchor", ID: "x\n@page"}}, {{Op: "wait", Frames: 601}}, {{Op: "empty"}, {Op: "text", Value: "x"}},
		{{Op: "text", Value: "x"}, {Op: "text", Value: "y", Name: "unexpected"}, {Op: "end"}},
	} {
		if s, err := EmitAuthorScript([]AuthorMessage{{ID: "action.hud.act_1", Operations: ops}}, "bad"); err == nil || s != nil {
			t.Fatal("invalid operation emitted", ops)
		}
	}
	// Escape only a physical line prefix, not an @ at the start of a later text
	// fragment. The parser's implicit end is the same operation as explicit end.
	s, err = EmitAuthorScript([]AuthorMessage{{ID: "action.hud.act_1", Operations: []AuthorOperation{{Op: "placeholder", Name: "master_name"}, {Op: "text", Value: "@home {safe}"}}}}, "inline")
	if err != nil || !strings.Contains(s.Text(), "{master_name}@home {{safe}}") {
		t.Fatal("inline escape/end handling", err)
	}
	for _, args := range []string{"x  y", "\"quoted\" \\backslash", "\t@not-a-command"} {
		s, err := EmitAuthorScript([]AuthorMessage{{ID: "test.event", Operations: []AuthorOperation{{Op: "event", ID: "test", Arguments: args}, {Op: "end"}}}}, "syntax")
		if err != nil || s.messages[0].Operations[0].Arguments != args {
			t.Fatal("event syntax did not round-trip", err)
		}
	}
}

func TestNativeAuthorClearPaddingPreservesLinesAndRealSpacing(t *testing.T) {
	ops := []AuthorOperation{{Op: "text", Value: "Label "}, {Op: "placeholder", Name: "master_name"}, {Op: "text", Value: "  "}, {Op: "line"}, {Op: "text", Value: "   "}, {Op: "line"}, {Op: "end"}}
	want := append(append([]AuthorOperation{}, ops[:4]...), ops[5:]...)
	if got := stripNativePadding(ops); !slices.Equal(got, want) {
		t.Fatal("clear padding changed content", got)
	}
	if got := stripNativePadding([]AuthorOperation{{Op: "text", Value: "\u00a0"}}); len(got) != 1 {
		t.Fatal("Unicode content stripped as native padding")
	}
	for _, id := range []string{"action.hud.act_1", "action.hud.act_2"} {
		route := nativeAuthorRoute("fixed_composer", Operation{"op": "text", "value": "   "}, Operation{"op": "end"})
		route.ID = id
		m, err := nativeAuthorMessages([]*NativeSemanticRoute{route}, nil)
		if err != nil || len(m[0].Operations) != 2 || m[0].Operations[0].Op != "empty" {
			t.Fatal("empty native source not explicit", err)
		}
	}
	route := nativeAuthorRoute("fixed_composer", Operation{"op": "text", "value": "Not a complete row"})
	route.ID = "status.report.master_report"
	if _, err := nativeAuthorOperations(route, nil); err == nil {
		t.Fatal("unterminated report silently discarded")
	}
}

func TestNativeAuthorRejectsUnresolvedSourcesAndWrongProfile(t *testing.T) {
	for _, op := range []Operation{{"op": "unknown"}, {"op": "native_glyphs"}, {"op": "text", "value": nil}, {"op": "format_number", "value": nil}, {"op": "format_number", "value": "master_level", "width": "3"}, {"op": "insert_icon", "value": "ui.heart", "part_index": -1}, {"op": "text", "value": "\xff"}} {
		if _, err := nativeAuthorOperations(nativeAuthorRoute("angel_dialogue", op), nil); err == nil {
			t.Fatal("unresolved native operation accepted", op)
		}
	}
	for _, routes := range [][]*NativeSemanticRoute{nil, {nil}, {nativeAuthorRoute("x", Operation{"op": "end"}), nativeAuthorRoute("x", Operation{"op": "end"})}} {
		if _, err := nativeAuthorMessages(routes, nil); err == nil {
			t.Fatal("invalid routes accepted")
		}
	}
	manifest, err := ParsePackManifest(authorPackManifest, "pack.ini")
	if err != nil {
		t.Fatal(err)
	}
	metadata := manifest.Metadata()
	metadata.Coverage = "complete"
	if p, err := (*Decoder)(nil).BuildNativeAuthorPack(metadata); err == nil || p != nil {
		t.Fatal("nil decoder accepted")
	}
	d := &Decoder{profile: decoderProfile{ID: "jp"}}
	if p, err := d.BuildNativeAuthorPack(metadata); err == nil || p != nil {
		t.Fatal("wrong profile accepted")
	}
	metadata.SourceProfile = "jp"
	if p, err := d.BuildNativeAuthorPack(metadata); err == nil || p != nil {
		t.Fatal("regional source activated on US runtime")
	}
}

func TestNativeAuthorCompleteSourceRuntimeParity(t *testing.T) {
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	for _, profile := range []string{"us", "eu-en", "de", "fr", "jp"} {
		t.Run(profile, func(t *testing.T) {
			refs, err := AuthorReferences(profile)
			if err != nil {
				t.Fatal(err)
			}
			var routes []*NativeSemanticRoute
			for _, ref := range refs {
				ops := []Operation{}
				// Invented text only; no retail wording in the primary Go/C gate.
				// Fields the game reserves a single row for get a single row.
				ops = append(ops, Operation{"op": "text", "value": "Most excellent, élève! 日本語"})
				if ref.Presentation.MaximumLines != 1 {
					ops = append(ops, Operation{"op": "line_break"}, Operation{"op": "text", "value": "Party on!"})
				}
				if ref.Presentation.Keyboard != nil {
					// This census fixture intentionally omits keyboard wording;
					// full page/Unicode/input-shape parity has dedicated cases.
					ops = nil
				}
				for _, anchor := range ref.Anchors {
					ops = append(ops, Operation{"op": anchor[:strings.LastIndexByte(anchor, '.')]})
				}
				ops = append(ops, Operation{"op": "end"})
				routes = append(routes, &NativeSemanticRoute{ID: ref.ID, Category: "fixed_composer", Operations: ops})
			}
			m, err := ParsePackManifest(authorPackManifest, "pack.ini")
			if err != nil {
				t.Fatal(err)
			}
			metadata := m.Metadata()
			metadata.SourceProfile = profile
			metadata.Coverage = "complete"
			metadata.Target = "reference-only"
			if profile == "us" {
				metadata.Target = "us-runtime"
			}
			m, err = NewPackManifest(metadata, PackFonts{Primary: "builtin:actraiser-sans"}, []string{"text/source.artext"})
			if err != nil {
				t.Fatal(err)
			}
			p, err := nativeAuthorPack(m, routes, nil)
			if err != nil {
				t.Fatal(err)
			}
			if p.workspace.Stats().MessageCount != len(refs) {
				t.Fatal("routes lost")
			}
			slices.Reverse(routes)
			again, err := nativeAuthorPack(m, routes, nil)
			if err != nil || !reflect.DeepEqual(p.Files(), again.Files()) {
				t.Fatal("nondeterministic source order", err)
			}
			root := t.TempDir()
			writeAuthorTestFiles(t, root, p.Files())
			if probe != "" {
				assertAuthorPackRuntime(t, probe, root, p)
			}
			opened, err := OpenAuthorPack(root)
			if err != nil || opened.RuntimeRevision() != p.RuntimeRevision() {
				t.Fatal("source pack reopen changed", err)
			}
		})
	}
}

func FuzzAuthorEmitter(f *testing.F) {
	for _, value := range []string{"Excellent!", "@x", "#x", "  @x", "::x", "{é} 日本語", "\u00a0", "\n", "\\#x"} {
		f.Add(value)
	}
	f.Fuzz(func(t *testing.T, value string) {
		if len(value) > 8192 {
			return
		}
		s, err := EmitAuthorScript([]AuthorMessage{{ID: "action.hud.act_1", Operations: []AuthorOperation{{Op: "text", Value: value}, {Op: "end"}}}}, "fuzz")
		if err != nil {
			return
		}
		ops := s.Messages()[0].Operations
		if len(ops) != 2 || ops[0].Value != value || ops[1].Op != "end" {
			t.Fatal("accepted lossy emission")
		}
		again, err := EmitAuthorScript(s.Messages(), "fuzz")
		if err != nil || again.Text() != s.Text() {
			t.Fatal("emission not stable", err)
		}
	})
}

func BenchmarkNativeAuthorSource(b *testing.B) {
	refs, err := AuthorReferences("us")
	if err != nil {
		b.Fatal(err)
	}
	var routes []*NativeSemanticRoute
	for _, ref := range refs {
		ops := []Operation{}
		for _, anchor := range ref.Anchors {
			ops = append(ops, Operation{"op": anchor[:strings.LastIndexByte(anchor, '.')]})
		}
		ops = append(ops, Operation{"op": "text", "value": fmt.Sprintf("Excellent %s!", ref.ID)}, Operation{"op": "end"})
		routes = append(routes, &NativeSemanticRoute{ID: ref.ID, Operations: ops})
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		messages, err := nativeAuthorMessages(routes, nil)
		if err != nil {
			b.Fatal(err)
		}
		if _, err := EmitAuthorScript(messages, "bench"); err != nil {
			b.Fatal(err)
		}
	}
}

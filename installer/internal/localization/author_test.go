package localization

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"
)

const authorConfirm = ":: sky.action_mode.confirm\n@anchor reset_text_cursor.00\nExcellent, {master_name}!\n@anchor yield.01\n@end\n"

func TestAuthorScriptPreservesSourceAndDiagnosesLines(t *testing.T) {
	text := "\ufeff# translator note\r\n" + strings.ReplaceAll(authorConfirm, "\n", "\r\n") + "\r\n; untouched tail\r\n"
	s, err := ParseAuthorScript(text, "text/demo.artext")
	if err != nil {
		t.Fatal(err)
	}
	if s.Text() != text {
		t.Fatal("source changed during open")
	}
	messages := s.Messages()
	if messages[0].SourceLine != 2 || messages[0].Operations[1].SourceLine != 4 {
		t.Fatalf("lost source lines: %+v", messages)
	}
	messages[0].Operations[1].Value = "poison"
	if s.Messages()[0].Operations[1].Value == "poison" {
		t.Fatal("mutable parser state escaped")
	}
	for _, invalid := range []string{"::", ":: 0bad\n@end", ":: x\n", ":: x\n@empty\nHello", ":: x\n@empty\n@empty", ":: x\n@alias y\n@end", ":: x\nhello\n:: x\nworld", ":: x\n{bad", ":: x\n}", ":: x\n{value:00}", ":: x\n{value:010}", ":: x\n{value:１}", ":: x\n@wait 0", ":: x\n@wait 601", ":: x\n@wait ١", ":: x\n@wait 99999999999999999999999", ":: x\n@anchor 'unterminated", ":: x\n@unknown", "\xff", ":: x\nHi\x00", ":: x\n@page unwanted"} {
		bad, err := ParseAuthorScript(invalid, "invalid.artext")
		if err == nil || bad != nil {
			t.Fatalf("accepted invalid source %q", invalid)
		}
		var located *AuthorError
		if !errors.As(err, &located) || located.Path != "invalid.artext" {
			t.Fatalf("unlocated error: %v", err)
		}
	}
}

func TestAuthorScriptWhitespaceEscapesAndBudgets(t *testing.T) {
	s, err := ParseAuthorScript(":: dialogue.event.relay.aitos\n@@literal {{brace}}\n# note\n\\#literal  \n\n\u00a0\n@line\nx\u2028y\n@end\n", "a")
	if err != nil {
		t.Fatal(err)
	}
	ops := s.Messages()[0].Operations
	if ops[0].Value != "@literal {brace}" || ops[1].Value != " #literal  " || ops[2].Op != "paragraph" || ops[3].Value != "\u00a0" || ops[5].Value != "x\u2028y" {
		t.Fatalf("wrong whitespace/escapes: %+v", ops)
	}
	for _, test := range []struct{ name, valid, extra string }{
		{"pages", strings.Repeat("@page\n", 63), "@page\n"},
		{"waits", strings.Repeat("@wait 600\n", 6), "@wait 1\n"},
		{"operations", strings.Repeat("@line\n", 4095), "@line\n"},
		{"bytes", strings.Repeat("é", MaxAuthorTextBytes/2), "x"},
	} {
		t.Run(test.name, func(t *testing.T) {
			if _, err := ParseAuthorScript(":: dialogue.event.relay.aitos\n"+test.valid, "a"); err != nil {
				t.Fatalf("boundary rejected: %v", err)
			}
			if _, err := ParseAuthorScript(":: dialogue.event.relay.aitos\n"+test.valid+test.extra, "a"); err == nil {
				t.Fatal("over-limit accepted")
			}
		})
	}
	if _, err := ParseAuthorScript(strings.Repeat("#", MaxAuthorScriptBytes+1), "a"); err == nil {
		t.Fatal("oversized script accepted")
	}
}

func TestAuthorContractsAndAliasDependents(t *testing.T) {
	valid := []string{authorConfirm, ":: dialogue.event.relay.aitos\n@empty\n", ":: dialogue.event.relay.aitos\nFirst short\n@preferred-line\nSecond line\n", ":: dialogue.event.relay.aitos\n@alias dialogue.event.relay.bloodpool\n:: dialogue.event.relay.bloodpool\nExcellent!\n", ":: status.report.master_report\n{master_level:03}\n"}
	for _, text := range valid {
		s, err := ParseAuthorScript(text, "a")
		if err != nil {
			t.Fatal(err)
		}
		if _, err := ValidateAuthorScripts("us", "partial", s); err != nil {
			t.Fatal(err)
		}
		if _, err := ValidateAuthorScripts("us", "complete", s); err == nil {
			t.Fatal("incomplete corpus accepted as complete")
		}
	}
	bad := []string{
		strings.Replace(authorConfirm, "{master_name}", "{master_name:02}", 1),
		strings.Replace(authorConfirm, "{master_name}", "{lair_count}", 1),
		strings.Replace(authorConfirm, "@anchor reset_text_cursor.00\n", "", 1),
		strings.Replace(authorConfirm, "@anchor yield.01\n", "@anchor yield.01\n@page\n", 1),
		":: dialogue.event.relay.aitos\n@event mutate.game\n",
		":: dialogue.event.relay.aitos\n@alias dialogue.event.relay.bloodpool\n",
		":: dialogue.event.relay.aitos\n@alias dialogue.event.relay.bloodpool\n:: dialogue.event.relay.bloodpool\n@alias dialogue.event.relay.aitos\n",
		":: dialogue.event.relay.aitos\n@alias sky.action_mode.confirm\n" + authorConfirm,
		":: unknown.semantic\nExcellent!\n",
		":: action.hud.act_1\nACT\n@preferred-line\n1\n",
	}
	for _, text := range bad {
		s, err := ParseAuthorScript(text, "a")
		if err != nil {
			t.Fatal(err)
		}
		stats, err := ValidateAuthorScripts("us", "partial", s)
		if err == nil || stats != (AuthorValidationStats{}) {
			t.Fatalf("bad semantic candidate leaked success: %q", text)
		}
	}
	refs, err := AuthorReferences("us")
	if err != nil || len(refs) != 558 {
		t.Fatalf("refs: %d %v", len(refs), err)
	}
	for i := range refs {
		if len(refs[i].Anchors) > 0 {
			refs[i].Anchors[0] = "poison"
		}
		if len(refs[i].Placeholders) > 0 {
			refs[i].Placeholders[0].Kind = "poison"
		}
	}
	if _, err := ValidateAuthorScripts("invalid", "partial"); err == nil {
		t.Fatal("invalid profile accepted")
	}
	if _, err := AuthorReferences("invalid"); err == nil {
		t.Fatal("invalid picker profile accepted")
	}
}

func TestAuthorWorkspaceEditReopenTreeAndAtomicFailure(t *testing.T) {
	sources := map[string]string{
		"text/a.artext": "# keep header\r\n:: dialogue.event.relay.aitos\r\n@alias dialogue.event.relay.bloodpool\r\n",
		"text/b.artext": "; editor notes\n:: dialogue.event.relay.bloodpool\nMost excellent!\n@end\n\n" + authorConfirm,
	}
	progress := "# private review note\r\ndialogue.event.relay.bloodpool\twip\r\n"
	w, err := NewAuthorWorkspace("us", "partial", sources, progress)
	if err != nil {
		t.Fatal(err)
	}
	sources["text/a.artext"] = "poison"
	oldSources := w.Sources()
	view, ok := w.Message("dialogue.event.relay.bloodpool")
	if !ok || !view.Present || view.Status != TranslationWIP {
		t.Fatalf("view: %+v", view)
	}
	w2, err := w.EditMessage("dialogue.event.relay.bloodpool", "; keep my note\nTotally excellent!\n@page\nA second adventure.\n@end\n", TranslationDone)
	if err != nil {
		t.Fatal(err)
	}
	if w2.Sources()["text/a.artext"] != oldSources["text/a.artext"] || !strings.HasSuffix(w2.Sources()["text/b.artext"], authorConfirm) {
		t.Fatal("unrelated source changed")
	}
	if w.ProgressText() != progress || w2.ProgressText() != strings.Replace(progress, "\twip", "\tdone", 1) {
		t.Fatal("progress/comments were not preserved")
	}
	reopened, err := NewAuthorWorkspace("us", "partial", w2.Sources(), w2.ProgressText())
	if err != nil || !reflect.DeepEqual(reopened.Sources(), w2.Sources()) || reopened.ProgressText() != w2.ProgressText() {
		t.Fatal("save/reopen is not stable", err)
	}
	for _, body := range []string{"@alias dialogue.event.relay.aitos\n", "@empty\n:: action.hud.act_3\n@empty\n", "{master_name}\n", "@anchor reset_text_cursor.00\nHello\n"} {
		if next, err := w2.EditMessage("dialogue.event.relay.bloodpool", body, TranslationDone); err == nil || next != nil {
			t.Fatalf("unsafe edit accepted: %q", body)
		}
	}
	if _, err := w2.EditMessage("dialogue.event.relay.bloodpool", view.Body, "bogus"); err == nil {
		t.Fatal("invalid status accepted")
	}
	if _, err := w2.EditMessage("missing", view.Body, TranslationWIP); err == nil {
		t.Fatal("unknown edit accepted")
	}
	short, err := w2.EditMessage("dialogue.event.relay.bloodpool", "@empty\n", TranslationWIP)
	if err != nil {
		t.Fatal("intentional shortening failed", err)
	}
	if !strings.Contains(short.Sources()["text/b.artext"], "@empty") {
		t.Fatal("empty edit lost")
	}
	var total, present, done int
	for _, entry := range w2.Children("") {
		total += entry.Total
		present += entry.Present
		done += entry.Done
	}
	if total != 558 || present != 3 || done != 1 {
		t.Fatalf("tree counts %d %d %d", total, present, done)
	}
	children := w2.Children("action.hud")
	children[0].Label = "poison"
	if w2.Children("action.hud")[0].Label == "poison" {
		t.Fatal("mutable tree escaped")
	}
	if !reflect.DeepEqual(w.Sources(), oldSources) {
		t.Fatal("editing modified old snapshot")
	}
	for _, bad := range []string{"dialogue.event.relay.bloodpool\tbogus", "missing\tdone", "dialogue.event.relay.bloodpool\twip\ndialogue.event.relay.bloodpool\tdone", "\x00", "\xff"} {
		if _, err := NewAuthorWorkspace("us", "partial", oldSources, bad); err == nil {
			t.Fatalf("invalid progress accepted: %q", bad)
		}
	}
}

// Only test scaffolding writes files. Production editing stays in memory until
// the separately gated manifest/import/install transaction is implemented.
func authorRuntimeCheck(t *testing.T, probe, profile, coverage string, sources map[string]string) {
	t.Helper()
	w, goErr := NewAuthorWorkspace(profile, coverage, sources, "")
	root := t.TempDir()
	target := "reference-only"
	if profile == "us" {
		target = "us-runtime"
	}
	manifest := fmt.Sprintf("[pack]\nformat = actraiser-language-pack\nversion = 1\nid = test.alternative-us\nlocale = en-US\nname = Excellent Adventure\nautonym = English\nauthor = Test\nlicense = MIT\ndirection = auto\ntarget = %s\nsource_profile = %s\nfallback = native-us\ncoverage = %s\n[fonts]\nprimary = builtin:actraiser-sans\n[scripts]\n", target, profile, coverage)
	// Obtain exactly the same sorted source order used by the workspace.
	paths := make([]string, 0, len(sources))
	for path := range sources {
		paths = append(paths, path)
	}
	// Deterministic order matters to both source-line and operation comparisons.
	slices.Sort(paths)
	for _, path := range paths {
		manifest += "source = " + path + "\n"
		full := filepath.Join(root, filepath.FromSlash(path))
		if err := os.MkdirAll(filepath.Dir(full), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(sources[path]), 0600); err != nil {
			t.Fatal(err)
		}
	}
	manifestPath := filepath.Join(root, "pack.ini")
	if err := os.WriteFile(manifestPath, []byte(manifest), 0600); err != nil {
		t.Fatal(err)
	}
	output, cErr := exec.Command(probe, "--dump", manifestPath).CombinedOutput()
	if (goErr == nil) != (cErr == nil) {
		t.Fatalf("Go/C disagree: Go=%v C=%v\n%s", goErr, cErr, output)
	}
	if goErr != nil {
		return
	}
	var actual []AuthorMessage
	if err := json.Unmarshal(output, &actual); err != nil {
		t.Fatalf("C probe JSON: %v\n%s", err, output)
	}
	var expected []AuthorMessage
	for _, script := range w.scripts {
		expected = append(expected, script.Messages()...)
	}
	if !reflect.DeepEqual(actual, expected) {
		t.Fatalf("Go/C parsed operations differ:\nC=%+v\nGo=%+v", actual, expected)
	}
}

func TestAuthorRuntimeParity(t *testing.T) {
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	if probe == "" {
		t.Skip("set AR_AUTHOR_RUNTIME_PROBE to the C loader test executable; CTest supplies it")
	}
	if _, err := os.Stat(probe); err != nil {
		t.Fatal(err)
	}
	for _, profile := range []string{"us", "eu-en", "de", "fr", "jp"} {
		t.Run(profile, func(t *testing.T) {
			refs, err := AuthorReferences(profile)
			if err != nil {
				t.Fatal(err)
			}
			var script strings.Builder
			for i, ref := range refs {
				fmt.Fprintf(&script, ":: %s\n", ref.ID)
				if i%3 != 0 {
					script.WriteString("Excellent, e\u0301lève! 日本語 Ελληνικά العربية 👩‍🚀\n@line\n@@literal {{braces}}\n\nSecond paragraph.\n@page\nAnother adventure.\n@wait 30\n")
					for _, value := range ref.Placeholders {
						spec := ""
						if value.Kind == "number" {
							spec = ":03"
						}
						fmt.Fprintf(&script, "{%s%s} ", value.Name, spec)
					}
					script.WriteString("\n")
				}
				for _, anchor := range ref.Anchors {
					fmt.Fprintf(&script, "@anchor %s\n", anchor)
				}
				if i%3 == 0 {
					script.WriteString("@empty\n")
				}
				script.WriteString("@end\n\n")
			}
			authorRuntimeCheck(t, probe, profile, "complete", map[string]string{"text/all.artext": script.String()})
		})
	}
	for i, text := range []string{
		authorConfirm,
		strings.ReplaceAll(authorConfirm, "\n", "\r\n"),
		"\ufeff" + authorConfirm,
		":: dialogue.event.relay.aitos\n\u00a0\n@line\nx\u2028y\n@end\n",
		":: dialogue.event.relay.aitos\n@@literal {{braces}}\n\\#not a comment\n\\;nor this\n",
		":: dialogue.event.relay.aitos\n@alias dialogue.event.relay.bloodpool\n:: dialogue.event.relay.bloodpool\nExcellent!\n",
		":: dialogue.event.relay.aitos\n@alias dialogue.event.relay.bloodpool\n:: dialogue.event.relay.bloodpool\n@alias dialogue.event.relay.aitos\n",
		":: dialogue.event.relay.aitos\n@alias dialogue.event.relay.bloodpool\n",
		":: dialogue.event.relay.aitos\n@empty\nHello\n",
		":: dialogue.event.relay.aitos\n@wait 601\n",
		":: dialogue.event.relay.aitos\n@wait ١\n",
		":: dialogue.event.relay.aitos\n" + strings.Repeat("@page\n", 64),
		":: dialogue.event.relay.aitos\n" + strings.Repeat("@wait 600\n", 7),
		":: dialogue.event.relay.aitos\n" + strings.Repeat("é", MaxAuthorTextBytes/2+1),
		strings.Replace(authorConfirm, "{master_name}", "{master_name:02}", 1),
		strings.Replace(authorConfirm, "@anchor yield.01", "@anchor yield.01\n@page", 1),
		strings.Replace(authorConfirm, "reset_text_cursor.00", `"reset_text_cursor\.00"`, 1),
		strings.Replace(authorConfirm, "reset_text_cursor.00", `"reset_text_cursor.00"`, 1),
		":: dialogue.event.relay.aitos\n" + strings.Repeat("@line\n", 4095),
		":: dialogue.event.relay.aitos\n" + strings.Repeat("@line\n", 4096),
		":: dialogue.event.relay.aitos\n@end\ntrailing content\n",
		":: dialogue.event.relay.aitos\n@empty\n:: dialogue.event.relay.aitos\n@empty\n",
		":: dialogue.event.relay.aitos\n\x00\n",
		":: dialogue.event.relay.aitos\n\xff\n",
	} {
		t.Run(fmt.Sprintf("edge-%d", i), func(t *testing.T) {
			authorRuntimeCheck(t, probe, "us", "partial", map[string]string{"text/a.artext": text})
		})
	}
	t.Run("edited-save-reopen", func(t *testing.T) {
		w, err := NewAuthorWorkspace("us", "partial", map[string]string{"text/sky.artext": authorConfirm}, "")
		if err != nil {
			t.Fatal(err)
		}
		for _, body := range []string{
			"@anchor reset_text_cursor.00\nExcellent, {master_name}!\n@page\nTime for a most triumphant adventure.\n@wait 20\n@anchor yield.01\n@end\n",
			"@anchor reset_text_cursor.00\nReady?\n@anchor yield.01\n@end\n",
			"@anchor reset_text_cursor.00\n@empty\n@anchor yield.01\n@end\n",
		} {
			w, err = w.EditMessage("sky.action_mode.confirm", body, TranslationWIP)
			if err != nil {
				t.Fatal(err)
			}
			authorRuntimeCheck(t, probe, "us", "partial", w.Sources())
			w, err = NewAuthorWorkspace("us", "partial", w.Sources(), w.ProgressText())
			if err != nil {
				t.Fatal(err)
			}
		}
	})
	t.Run("cross-file-alias-edit", func(t *testing.T) {
		sources := map[string]string{
			"text/a.artext": ":: dialogue.event.relay.aitos\n@alias status.report.master_report\n",
			"text/b.artext": ":: status.report.master_report\nMost excellent!\n@end\n",
		}
		w, err := NewAuthorWorkspace("us", "partial", sources, "")
		if err != nil {
			t.Fatal(err)
		}
		authorRuntimeCheck(t, probe, "us", "partial", sources)
		if next, err := w.EditMessage("status.report.master_report", "{master_level:02}\n", TranslationDone); err == nil || next != nil {
			t.Fatal("edit broke alias's placeholder contract")
		}
		sources["text/b.artext"] = ":: status.report.master_report\n{master_level:02}\n"
		authorRuntimeCheck(t, probe, "us", "partial", sources)
		authorRuntimeCheck(t, probe, "us", "partial", w.Sources())
	})
}

func FuzzAuthorScript(f *testing.F) {
	for _, text := range []string{authorConfirm, ":: dialogue.event.relay.aitos\n@empty\n", ":: a\n{{braces}}", "\ufeff:: a\r\n@@literal\r\n"} {
		f.Add(text)
	}
	f.Fuzz(func(t *testing.T, text string) {
		if len(text) > 1<<20 {
			return
		}
		s, err := ParseAuthorScript(text, "fuzz.artext")
		if err != nil {
			if s != nil {
				t.Fatal("partial result after error")
			}
			return
		}
		if s.Text() != text {
			t.Fatal("source changed")
		}
		for i, message := range s.Messages() {
			if i >= 8 {
				break
			} // Bound repeated reparses for generated many-message inputs.
			body, ok := s.Body(message.ID)
			if !ok {
				t.Fatal("missing body")
			}
			next, err := s.ReplaceBody(message.ID, body)
			if err != nil {
				t.Fatal("self-edit rejected", err)
			}
			// Replacing an unterminated final line may add its LF. Runtime
			// semantics and source lines must nevertheless remain identical.
			if !reflect.DeepEqual(s.Messages(), next.Messages()) {
				t.Fatal("self-edit changed semantics")
			}
		}
	})
}

func TestAuthorRegressions(t *testing.T) {
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	var cases []struct {
		ID, Profile, Coverage string
		Sources               map[string]string
		Valid                 bool
		Messages              []AuthorMessage
		Stats                 AuthorValidationStats
		Manifest, Progress    string
		ManifestMetadata      struct {
			Pack    PackMetadata
			Fonts   PackFonts
			Scripts []string
		} `json:"manifest_metadata"`
	}
	readRegressionFixture(t, "author-regressions.json.gz", &cases)
	if len(cases) == 0 {
		t.Fatal("empty fixture cases")
	}
	for _, c := range cases {
		t.Run(c.ID, func(t *testing.T) {
			w, err := NewAuthorWorkspace(c.Profile, c.Coverage, c.Sources, "")
			if (err == nil) != c.Valid {
				t.Fatalf("Go/fixture acceptance differs: expected=%v error=%v", c.Valid, err)
			}
			if c.Valid {
				var messages []AuthorMessage
				for _, script := range w.scripts {
					messages = append(messages, script.Messages()...)
				}
				if !reflect.DeepEqual(messages, c.Messages) || w.Stats() != c.Stats {
					t.Fatal("Go/fixture operation/source-line/stats mismatch")
				}
				reopened, err := NewAuthorWorkspace(c.Profile, c.Coverage, w.Sources(), w.ProgressText())
				if err != nil || !reflect.DeepEqual(w.Sources(), reopened.Sources()) {
					t.Fatal("fixture pack reopen lost content", err)
				}
				if c.Manifest != "" {
					files := map[string][]byte{"pack.ini": []byte(c.Manifest), "translation-progress.tsv": []byte(c.Progress)}
					for path, text := range c.Sources {
						files[path] = []byte(text)
					}
					p, err := LoadAuthorPack(packSnapshotFS(files))
					if err != nil {
						t.Fatal(err)
					}
					if p.Manifest().Metadata() != c.ManifestMetadata.Pack || !reflect.DeepEqual(p.Manifest().Fonts(), c.ManifestMetadata.Fonts) || !slices.Equal(p.Manifest().Sources(), c.ManifestMetadata.Scripts) || !reflect.DeepEqual(p.Files(), files) {
						t.Fatal("whole fixture pack metadata/progress/source snapshot changed")
					}
					root := t.TempDir()
					writeAuthorTestFiles(t, root, p.Files())
					if probe != "" {
						assertAuthorPackRuntime(t, probe, root, p)
					}
				}
			}
			if probe != "" {
				authorRuntimeCheck(t, probe, c.Profile, c.Coverage, c.Sources)
			}
		})
	}
}

func BenchmarkAuthorWorkspaceEdit(b *testing.B) {
	refs, err := AuthorReferences("us")
	if err != nil {
		b.Fatal(err)
	}
	var source strings.Builder
	for _, ref := range refs {
		fmt.Fprintf(&source, ":: %s\n", ref.ID)
		for _, anchor := range ref.Anchors {
			fmt.Fprintf(&source, "@anchor %s\n", anchor)
		}
		source.WriteString("@empty\n@end\n")
	}
	w, err := NewAuthorWorkspace("us", "complete", map[string]string{"text/all.artext": source.String()}, "")
	if err != nil {
		b.Fatal(err)
	}
	body := "@anchor reset_text_cursor.00\nExcellent, {master_name}!\n@anchor yield.01\n@end\n"
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := w.EditMessage("sky.action_mode.confirm", body, TranslationWIP); err != nil {
			b.Fatal(err)
		}
	}
}

// Fixed menus, cards and labels display exactly what their native surface
// reserves. Content past that is not a style choice; the game never shows it,
// so the editor must reject it here rather than let the player find out.
func TestAuthorPresentationContracts(t *testing.T) {
	refs, err := AuthorReferences("us")
	if err != nil {
		t.Fatal(err)
	}
	shapes := map[string]AuthorPresentation{}
	for _, ref := range refs {
		shapes[ref.ID] = ref.Presentation
	}
	for _, expected := range []struct {
		id string
		AuthorPresentation
	}{
		{"action.hud.act_1", AuthorPresentation{Shape: "fixed", MaximumPages: 1}},
		{"action.hud.act_label", AuthorPresentation{Shape: "fixed", MaximumPages: 1, MaximumLines: 1}},
		{"title.save_choice.labels", AuthorPresentation{Shape: "fixed", MaximumPages: 1, RequiredNonemptyLines: 2}},
		{"name_entry.prompt_and_alphabet", AuthorPresentation{Shape: "keyboard", Keyboard: &AuthorKeyboardShape{Rows: 5, Columns: 13, MaximumLines: 63, MaximumPageBytes: 3072}}},
		{"town.name.aitos", AuthorPresentation{Shape: "inline", MaximumPages: 1, MaximumLines: 1}},
		{"dialogue.event.relay.aitos", AuthorPresentation{Shape: "flow"}},
	} {
		if !reflect.DeepEqual(shapes[expected.id], expected.AuthorPresentation) {
			t.Fatalf("%s: %+v", expected.id, shapes[expected.id])
		}
	}

	for _, tc := range []struct{ text, want string }{
		{":: action.hud.act_1\nFirst card.\n@page\nNever displayed.\n", "pages beyond that are never shown"},
		{":: action.hud.act_label\nToo\n@line\ntall\n", "reserves 1 line(s); the message has 2"},
		{":: town.name.aitos\nAitos\n@line\nover two rows\n", "inline term reserves 1 line(s)"},
		{":: title.save_choice.labels\nContinue\n@line\nNew game\n@line\nA third option\n", "exactly 2 choice(s); the message has 3"},
		{":: title.save_choice.labels\nContinue\n", "exactly 2 choice(s); the message has 1"},
	} {
		s, err := ParseAuthorScript(tc.text, "a")
		if err != nil {
			t.Fatal(err)
		}
		_, err = ValidateAuthorScripts("us", "partial", s)
		if err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Fatalf("%q: %v", tc.text, err)
		}
	}

	// What must keep working: documented empties, native blank spacer rows,
	// multipage keyboards, and dialogue of any length.
	for _, text := range []string{
		":: title.save_choice.labels\n@empty\n",
		":: title.mode_select.with_save\nContinue\n@line\n@line\nNew game\n",
		":: name_entry.prompt_and_alphabet\n" + authorKeyboardPage + "@page\n" + authorKeyboardPage,
		":: dialogue.event.relay.aitos\nOne\n@page\nTwo\n@page\nThree\n",
		":: action.hud.act_1\nACT\n",
	} {
		s, err := ParseAuthorScript(text, "a")
		if err != nil {
			t.Fatal(err)
		}
		if _, err := ValidateAuthorScripts("us", "partial", s); err != nil {
			t.Fatalf("%q: %v", text, err)
		}
	}
}

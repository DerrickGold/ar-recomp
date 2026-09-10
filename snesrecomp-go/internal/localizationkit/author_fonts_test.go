package localizationkit

import (
	"bytes"
	"context"
	"io"
	"slices"
	"strings"
	"testing"
	"testing/fstest"
)

func TestAuthorFontStackTransaction(t *testing.T) {
	p := authorAdventureProject(t)
	before := p.ProjectRevision()
	files := map[string][]byte{"fonts/Primary.ttf": []byte("OTTOprimary fixture"), "fonts/Japanese.otf": []byte("OTTOfallback fixture")}
	stack := PackFonts{Primary: "fonts/Primary.ttf", Fallback: []string{"fonts/Japanese.otf", "builtin:actraiser-sans"}}
	next, err := p.WithFonts(stack, files)
	if err != nil {
		t.Fatal(err)
	}
	files[stack.Primary][0] = 'X'
	if p.ProjectRevision() != before || p.Pack().Manifest().Fonts().Primary != "builtin:actraiser-sans" || next.ProjectRevision() == before {
		t.Fatal("font edit mutated or reused the old snapshot")
	}
	if string(next.pack.fonts[stack.Primary]) != "OTTOprimary fixture" || next.Notes() != p.Notes() || next.pack.workspace != p.pack.workspace {
		t.Fatal("font edit copied/changed unrelated authoring state")
	}
	reordered, err := next.WithFonts(PackFonts{Primary: "fonts/Japanese.otf", Fallback: []string{"fonts/Primary.ttf"}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if &reordered.pack.fonts[stack.Primary][0] != &next.pack.fonts[stack.Primary][0] {
		t.Fatal("reordering recopied an unchanged font")
	}
	if _, _, err := reordered.Publication(PublicationOptions{ConfirmRights: true}); err == nil {
		t.Fatal("font license notice gate bypassed")
	}
	reordered, err = reordered.WithNotice("FONT-LICENSE.txt", "Synthetic fixture license")
	if err != nil {
		t.Fatal(err)
	}
	pub, _, err := reordered.Publication(PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	var archive bytes.Buffer
	if err := pub.WriteArchive(&archive, "publication"); err != nil {
		t.Fatal(err)
	}
	imported, err := ReadAuthorArchive(bytes.NewReader(archive.Bytes()), int64(archive.Len()))
	if err != nil {
		t.Fatal(err)
	}
	if imported.pack.manifest.fonts.Primary != "fonts/Japanese.otf" || !slices.Equal(imported.pack.manifest.fonts.Fallback, []string{"fonts/Primary.ttf"}) || len(imported.notices) != 1 {
		t.Fatal("publication lost ordered dependencies or license notices")
	}
	removed, err := reordered.WithFonts(PackFonts{Primary: "builtin:actraiser-sans"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(removed.pack.fonts) != 0 || len(reordered.pack.fonts) != 2 || len(removed.notices) != 1 {
		t.Fatal("removing dependencies damaged prior data or credits")
	}
}

func TestAuthorFontStackRejectsInvalidEdits(t *testing.T) {
	p := authorAdventureProject(t)
	for _, name := range []string{"../bad.ttf", "fonts\\bad.ttf", "text/sky.artext", "translation-progress.tsv", "pack.ini", "fonts/NUL.ttf", "text/sky.artext/font.ttf"} {
		if _, err := p.WithFonts(PackFonts{Primary: name}, map[string][]byte{name: {1}}); err == nil {
			t.Error("accepted", name)
		}
	}
	if _, err := p.WithFonts(PackFonts{Primary: "fonts/Missing.ttf"}, nil); err == nil {
		t.Fatal("missing dependency accepted")
	}
	if _, err := p.WithFonts(PackFonts{Primary: "fonts/Empty.ttf"}, map[string][]byte{"fonts/Empty.ttf": {}}); err == nil {
		t.Fatal("empty font accepted")
	}
	if _, err := p.WithFonts(p.pack.manifest.Fonts(), map[string][]byte{"fonts/Extra.ttf": {1}}); err == nil {
		t.Fatal("undeclared upload retained")
	}
	if _, err := p.WithFonts(PackFonts{Primary: "fonts/A.ttf", Fallback: []string{"fonts/a.ttf"}}, map[string][]byte{"fonts/A.ttf": {1}, "fonts/a.ttf": {2}}); err == nil {
		t.Fatal("case alias accepted")
	}
	native, err := NewSourceProject(p.pack)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := native.WithFonts(p.pack.manifest.Fonts(), nil); err == nil {
		t.Fatal("native reference edited")
	}
}

func TestManifestFontEditsPreserveOtherContent(t *testing.T) {
	for _, newline := range []string{"\n", "\r\n", "\r"} {
		text := strings.ReplaceAll(authorPackManifest, "[fonts]", "# metadata comment\n[fonts]\n# font comment")
		text = strings.ReplaceAll(text, "[scripts]", "# script comment\n[scripts]")
		text = "\ufeff" + strings.ReplaceAll(text, "\n", newline)
		m, err := ParsePackManifest(text, "pack.ini")
		if err != nil {
			t.Fatal(err)
		}
		next, err := m.WithFonts(PackFonts{Primary: "fonts/P.ttf", Fallback: []string{"fonts/F.ttf"}})
		if err != nil {
			t.Fatal(err)
		}
		for _, comment := range []string{"# metadata comment", "# font comment", "# script comment"} {
			if !strings.Contains(next.Text(), comment+newline) {
				t.Fatal("lost comment/line ending")
			}
		}
		if next.Metadata() != m.Metadata() || !slices.Equal(next.Sources(), m.Sources()) || m.Text() != text || !strings.HasPrefix(next.Text(), "\ufeff") {
			t.Fatal("unrelated manifest changed")
		}
	}
}

func coveredFixture(_ context.Context, fonts []FontCoverageSource, scalars []rune) (FontCoverageProbeResult, error) {
	r := FontCoverageProbeResult{Provided: make([]bool, len(scalars))}
	for _, font := range fonts {
		r.Fonts = append(r.Fonts, FontCoverageIdentity{font.Reference, strings.Repeat("0", 64)})
	}
	for i, scalar := range scalars {
		r.Provided[i] = scalar != '日'
	}
	return r, nil
}

func TestAuthorFontCoverageUsesParsedLiteralsAndSamples(t *testing.T) {
	p := authorAdventureProject(t)
	p, err := p.EditMessage("sky.action_mode.confirm", "# not displayed: 漢\n@anchor reset_text_cursor.00\nCafé 日 日 {master_name}\n@anchor yield.01\n@end\n", TranslationWIP)
	if err != nil {
		t.Fatal(err)
	}
	report, err := p.pack.CheckFontCoverage(context.Background(), coveredFixture, []string{"日"})
	if err != nil {
		t.Fatal(err)
	}
	if report.Complete || report.MissingCount != 1 || report.Missing[0].Codepoint != "U+65E5" || len(report.Missing[0].Locations) != 2 || !slices.Equal(report.DynamicValues, []string{"master_name"}) {
		t.Fatal(report)
	}
	if got := report.Missing[0].Locations[0]; got.MessageID != "sky.action_mode.confirm" || got.Source != "text/sky.artext" || got.Line == 0 {
		t.Fatal(got)
	}
	if _, err := p.pack.CheckFontCoverage(context.Background(), nil, nil); err == nil {
		t.Fatal("no backend silently accepted")
	}
	for _, bad := range []string{"\x00", "\xff", strings.Repeat("A", 16385)} {
		if _, err := p.pack.CheckFontCoverage(context.Background(), coveredFixture, []string{bad}); err == nil {
			t.Fatal("invalid sample accepted")
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := p.pack.CheckFontCoverage(ctx, coveredFixture, nil); err == nil {
		t.Fatal("cancelled check ran")
	}
	if _, err := p.pack.CheckFontCoverage(context.Background(), func(context.Context, []FontCoverageSource, []rune) (FontCoverageProbeResult, error) {
		return FontCoverageProbeResult{}, nil
	}, nil); err == nil {
		t.Fatal("incomplete response accepted")
	}
}

func TestFontCoverageReadersKeepSnapshotsImmutable(t *testing.T) {
	p := authorAdventureProject(t)
	p, err := p.WithFonts(PackFonts{Primary: "fonts/Fixture.ttf"}, map[string][]byte{"fonts/Fixture.ttf": []byte("OTTOfont bytes")})
	if err != nil {
		t.Fatal(err)
	}
	_, err = p.pack.CheckFontCoverage(context.Background(), func(ctx context.Context, fonts []FontCoverageSource, scalars []rune) (FontCoverageProbeResult, error) {
		for i := 0; i < 2; i++ {
			r := fonts[0].Open()
			data, err := io.ReadAll(r)
			r.Close()
			if err != nil || string(data) != "OTTOfont bytes" || fonts[0].Size != 14 {
				t.Fatal("not a fresh bounded snapshot", err)
			}
			data[0] = 'X'
		}
		return coveredFixture(ctx, fonts, scalars)
	}, nil)
	if err != nil {
		t.Fatal(err)
	}
}

func TestFontCoverageIncludesOnlyUnreplacedFallbackRoutes(t *testing.T) {
	load := func(body string) *AuthorPack {
		t.Helper()
		p, err := LoadAuthorPack(fstest.MapFS{"pack.ini": {Data: []byte(authorPackManifest)}, "text/sky.artext": {Data: []byte(body)}})
		if err != nil {
			t.Fatal(err)
		}
		return p
	}
	native := load(":: sim.menu.listen\n日\n@end\n:: sim.menu.message_speed\n@alias sim.menu.listen\n")
	partial := load(":: sim.menu.listen\nListen\n@end\n")
	report, err := partial.CheckFontCoverageWithFallback(context.Background(), coveredFixture, native, nil)
	if err != nil {
		t.Fatal(err)
	}
	if report.Complete || report.MissingCount != 1 || report.FallbackRevision == "" || report.ContentRevision == "" || report.PackageID != partial.Manifest().Metadata().ID {
		t.Fatal(report)
	}
	if locations := report.Missing[0].Locations; len(locations) != 1 || locations[0].MessageID != "sim.menu.message_speed" || locations[0].Source != "<native fallback>" {
		t.Fatal("fallback alias did not resolve within its owning source", locations)
	}
	replaced := load(":: sim.menu.listen\nListen\n@end\n:: sim.menu.message_speed\nSpeed\n@end\n")
	report, err = replaced.CheckFontCoverageWithFallback(context.Background(), coveredFixture, native, nil)
	if err != nil || !report.Complete {
		t.Fatal("replaced native text still required glyphs", report, err)
	}
}

func TestFontCoverageRejectsMismatchedBackendIdentities(t *testing.T) {
	p := authorAdventureProject(t).pack
	for _, mutation := range []func(*FontCoverageProbeResult){
		func(r *FontCoverageProbeResult) { r.Fonts[0].Reference = "another-font" },
		func(r *FontCoverageProbeResult) { r.Fonts[0].SHA256 = strings.Repeat("g", 64) },
		func(r *FontCoverageProbeResult) { r.Fonts = nil },
		func(r *FontCoverageProbeResult) { r.Provided = nil },
	} {
		_, err := p.CheckFontCoverage(context.Background(), func(ctx context.Context, fonts []FontCoverageSource, scalars []rune) (FontCoverageProbeResult, error) {
			r, err := coveredFixture(ctx, fonts, scalars)
			mutation(&r)
			return r, err
		}, nil)
		if err == nil {
			t.Fatal("malformed backend response accepted")
		}
	}
}

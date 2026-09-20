package localization

import (
	"context"
	"reflect"
	"strings"
	"testing"
	"testing/fstest"
)

func fontRolePack(t *testing.T) *AuthorPack {
	t.Helper()
	manifest := strings.Replace(authorPackManifest, "version = 1", "version = 2", 1) +
		"\n[font.hud]\n# Keep the font license beside the declaration.\nprimary = fonts/Hud.ttf\nfallback = builtin:actraiser-sans\n"
	pack, err := LoadAuthorPack(fstest.MapFS{
		"pack.ini":        {Data: []byte(manifest)},
		"text/sky.artext": {Data: []byte(":: action.hud.act_1\nBody <span font=\"hud\">Ω</span>\n")},
		"fonts/Hud.ttf":   {Data: []byte("OTTOfont fixture")},
	})
	if err != nil {
		t.Fatal(err)
	}
	return pack
}

func TestFontRoleDependenciesAndEdits(t *testing.T) {
	p := fontRolePack(t)
	fonts := p.manifest.Fonts()
	if len(fonts.Roles) != 1 || len(p.fonts) != 1 {
		t.Fatal("role dependency not loaded")
	}
	fonts.Roles[0].Fallback[0] = "builtin:other"
	if p.manifest.Fonts().Roles[0].Fallback[0] != "builtin:actraiser-sans" {
		t.Fatal("mutable manifest font stack escaped")
	}
	next, err := p.WithFonts(fonts, nil)
	if err != nil {
		t.Fatal(err)
	}
	if next.manifest.Version() != 2 || !strings.Contains(next.manifest.Text(), "# Keep the font license") {
		t.Fatal("font edit lost version or comments")
	}
	if _, err := next.WithFonts(PackFonts{Primary: fonts.Primary}, nil); err == nil {
		t.Fatal("removed a font role still used by text")
	}
	if _, err := ParsePackManifest(strings.Replace(next.manifest.Text(), "version = 2", "version = 1", 1), "pack.ini"); err == nil {
		t.Fatal("accepted v2 roles as v1")
	}

	// A saved project retains declarations and their dependencies together.
	project, err := NewSourceProject(next)
	if err != nil {
		t.Fatal(err)
	}
	store, err := NewAuthorStore(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Save(project, ""); err != nil {
		t.Fatal(err)
	}
	reopened, err := store.Open(project.pack.manifest.metadata.ID)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(reopened.pack.manifest.Fonts(), next.manifest.Fonts()) {
		t.Fatal("save/open changed font roles")
	}
}

func TestFontCoverageUsesEachRolesOwnFallbacks(t *testing.T) {
	p := fontRolePack(t)
	calls := 0
	probe := func(_ context.Context, fonts []FontCoverageSource, scalars []rune) (FontCoverageProbeResult, error) {
		calls++
		result := FontCoverageProbeResult{Provided: make([]bool, len(scalars))}
		for _, font := range fonts {
			result.Fonts = append(result.Fonts, FontCoverageIdentity{font.Reference, strings.Repeat("a", 64)})
		}
		for i, scalar := range scalars {
			result.Provided[i] = scalar != 'Ω'
			if fonts[0].Reference == "builtin:actraiser-sans" && scalar == 'Ω' {
				t.Fatal("HUD text checked against body stack")
			}
		}
		return result, nil
	}
	report, err := p.CheckFontCoverage(context.Background(), probe, nil)
	if err != nil {
		t.Fatal(err)
	}
	if calls != 2 || report.Complete || report.MissingCount != 1 || report.Missing[0].FontRole != "hud" {
		t.Fatalf("wrong role coverage: %#v", report)
	}
}

func TestMessageAndFontRolesAreOneTransaction(t *testing.T) {
	pack := fontRolePack(t)
	metadata := pack.Manifest().Metadata()
	metadata.ID = "combined.fonts"
	project, err := NewTranslationProject(pack, metadata)
	if err != nil {
		t.Fatal(err)
	}
	before := project.ProjectRevision()
	fonts := project.Pack().Manifest().Fonts()
	fonts.Roles[0].Name = "emphasis"
	next, err := project.EditMessageAndFonts("action.hud.act_1", "Body <span font=\"emphasis\">Ω</span>\n@end\n", TranslationWIP, fonts, nil)
	if err != nil {
		t.Fatal(err)
	}
	if next.Pack().Manifest().Fonts().Roles[0].Name != "emphasis" || project.ProjectRevision() != before {
		t.Fatal("role transaction mutated its input")
	}
	if _, err = project.EditMessageAndFonts("action.hud.act_1", "<span font=\"missing\">Broken</span>\n@end\n", TranslationWIP, fonts, nil); err == nil {
		t.Fatal("invalid final role accepted")
	}
}

func TestNativeFallbackRoleInheritsSelectedBody(t *testing.T) {
	native := fontRolePack(t)
	partial, err := LoadAuthorPack(fstest.MapFS{"pack.ini": {Data: []byte(strings.Replace(authorPackManifest, "version = 1", "version = 2", 1))}, "text/sky.artext": {Data: []byte(":: town.name.aitos\nAitos\n")}})
	if err != nil {
		t.Fatal(err)
	}
	probe := func(ctx context.Context, fonts []FontCoverageSource, scalars []rune) (FontCoverageProbeResult, error) {
		result, err := coveredFixture(ctx, fonts, scalars)
		for i, scalar := range scalars {
			if scalar == 'Ω' {
				result.Provided[i] = false
			}
		}
		return result, err
	}
	report, err := partial.CheckFontCoverageWithFallback(context.Background(), probe, native, nil)
	if err != nil {
		t.Fatal(err)
	}
	if report.MissingCount == 0 || report.Missing[0].FontRole != "body" {
		t.Fatalf("fallback role did not use body coverage: %#v", report)
	}
}

package localization

import (
	"strings"
	"testing"
	"testing/fstest"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

func TestTreatmentDefinitionsTravelWithTemplates(t *testing.T) {
	const definitions = "@define-style hud band=native:hud.band body=native:hud.body shadow=native:hud.shadow\n" +
		"@define-style quiet band=#223344 body=#223344 shadow=none shape=keyline\n"
	const body = ":: action.hud.act_1\n@style hud\nACT <span style=\"quiet\">I</span>\n@end\n"
	pack, err := LoadAuthorPack(fstest.MapFS{
		"pack.ini":        {Data: []byte(strings.Replace(authorPackManifest, "version = 1", "version = 2", 1))},
		"text/sky.artext": {Data: []byte(definitions + body)},
	})
	if err != nil {
		t.Fatal(err)
	}
	treatments := pack.Treatments()
	if len(treatments) != 2 || treatments[0].SourcePath != "text/sky.artext" || treatments[1].SourceLine != 2 {
		t.Fatal("definition origins lost")
	}
	message, err := pack.ResolvedMessage("action.hud.act_1")
	if err != nil {
		t.Fatal(err)
	}
	emitted, err := EmitAuthorScriptVersion([]AuthorMessage{message}, "publication.artext", 2, treatments...)
	if err != nil {
		t.Fatal(err)
	}
	if emitted.treatments[0].Definition != treatments[0].Definition {
		t.Fatal("emission changed inks")
	}
	if _, err := pack.EditMessage(message.ID, "@style missing\nACT I\n", TranslationWIP); err == nil {
		t.Fatal("undefined style accepted")
	}
	if _, err := emitted.ReplaceBody(message.ID, "@style quiet\nACT II\n"); err != nil {
		t.Fatal(err)
	}
	for _, definition := range []string{
		"@define-style bad band=#001122", "@define-style bad band=none body=#001122",
		"@define-style bad band=#001122 body=#112233 font=hud",
		"@define-style bad band=#001122 body=#112233 band=#445566",
		definitions + "@define-style hud band=#001122 body=#001122",
	} {
		if _, err := ParseAuthorScriptVersion(definition+"\n"+body, "bad.artext", 2); err == nil {
			t.Fatalf("accepted %q", definition)
		}
	}
}

func TestStylingDoesNotBypassUnchangedSourceGate(t *testing.T) {
	plain := []AuthorOperation{{Op: "text", Value: "Original words"}, {Op: "end"}}
	styled := []AuthorOperation{{Op: "text", Value: "Original "}, {Op: "text", Value: "words", Style: texttemplate.Style{Italic: 2}}, {Op: "end"}}
	if presentationDigest(plain) != presentationDigest(styled) {
		t.Fatal("styling changed the source wording digest")
	}
}

func TestDefinitionsOnlySourceAndInlineTermPolicy(t *testing.T) {
	definitions, err := ParseAuthorScriptVersion("@define-style prose band=#FFFFFF body=#FFFFFF\n", "styles.artext", 2)
	if err != nil {
		t.Fatal(err)
	}
	message, err := ParseAuthorScriptVersion(":: action.hud.act_1\n@style prose\nI\n@end\n", "messages.artext", 2)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := ValidateAuthorScripts("us", "partial", definitions, message); err != nil {
		t.Fatal(err)
	}
	if _, err := ValidateAuthorScripts("us", "partial", definitions); err == nil {
		t.Fatal("accepted definitions without messages")
	}
	for _, body := range []string{"@font body\nA slime", "A <i>slime</i>"} {
		term, err := ParseAuthorScriptVersion(":: enemy.name.slot_00\n"+body+"\n@end\n", "term.artext", 2)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := ValidateAuthorScripts("us", "partial", term); err == nil || !strings.Contains(err.Error(), "inherit appearance") {
			t.Fatalf("term appearance: %v", err)
		}
	}
}

func TestUnknownNativeInkRejectedBeforePreview(t *testing.T) {
	_, err := LoadAuthorPack(fstest.MapFS{
		"pack.ini":        {Data: []byte(strings.Replace(authorPackManifest, "version = 1", "version = 2", 1))},
		"text/sky.artext": {Data: []byte("@define-style bad band=native:hud.typo body=#FFFFFF\n:: action.hud.act_1\n@style bad\nACT I\n@end\n")},
	})
	if err == nil || !strings.Contains(err.Error(), "text/sky.artext:1") || !strings.Contains(err.Error(), "native:hud.typo") {
		t.Fatalf("missing source-located ink error: %v", err)
	}
}

package localization

import (
	"fmt"
	"os"
	"slices"
	"strings"
	"testing"
)

func TestContractCompleteStillReportsMissingLiveHudAndCredits(t *testing.T) {
	refs, _ := AuthorReferences("us")
	var text strings.Builder
	for _, ref := range refs {
		if !ref.RequiredForComplete {
			continue
		}
		fmt.Fprintf(&text, ":: %s\n", ref.ID)
		for _, anchor := range ref.Anchors {
			fmt.Fprintf(&text, "@anchor %s\n", anchor)
		}
		text.WriteString("@empty\n@end\n")
	}
	pack, err := LoadAuthorPack(projectMapFS(map[string][]byte{
		"pack.ini":        []byte(strings.Replace(authorPackManifest, "coverage = partial", "coverage = complete", 1)),
		"text/sky.artext": []byte(text.String()),
	}))
	if err != nil {
		t.Fatal(err)
	}
	p, err := NewSourceProject(pack)
	if err != nil {
		t.Fatal(err)
	}
	r := p.Coverage()
	if !r.ContractComplete || r.Required.Total != 495 || r.Required.Provided != 495 || len(r.Required.Missing) != 0 || r.LiveOptional.Total != 32 || r.LiveOptional.Provided != 0 || len(r.LiveOptional.Missing) != 32 {
		t.Fatalf("wrong required/live distinction: %+v", r)
	}
	if !slices.Contains(r.Dormant, "credits.special_mode") || !slices.Contains(r.Dormant, "sound_test.menu.labels") {
		t.Fatal(r.Dormant)
	}
	for _, s := range r.Surfaces {
		if s.Surface == "hud" && len(s.Missing) != 13 {
			t.Fatal(s)
		}
		if s.Surface == "credits" && len(s.Missing) != 19 {
			t.Fatal(s)
		}
		if s.Done != 0 || s.WIP != 0 || s.NotStarted != s.Provided {
			t.Fatal("presence invented review status", s)
		}
	}
	if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
		authorRuntimeCheck(t, probe, "us", "complete", map[string]string{"text/sky.artext": text.String()})
	}
}

func TestCoverageSeparatesReviewBaselineAndActualPublicationSelection(t *testing.T) {
	p := authorAdventureProject(t)
	r := p.Coverage()
	var provided, done, wip, unchanged int
	for _, s := range r.Surfaces {
		provided += s.Provided
		done += s.Done
		wip += s.WIP
		unchanged += s.UnchangedSource
	}
	if provided != 3 || done != 1 || wip != 1 || unchanged != 1 {
		t.Fatal(provided, done, wip, unchanged)
	}
	_, report, err := p.Publication(PublicationOptions{ConfirmRights: true, IncludeWIP: false})
	if err != nil {
		t.Fatal(err)
	}
	for _, s := range report.Coverage.Surfaces {
		if s.WIP != 0 || s.UnchangedSource != 0 {
			t.Fatal("unpublished content counted", s)
		}
		if s.Surface == "hud" && (!slices.Contains(s.Missing, "action.hud.act_1") || !slices.Contains(s.Missing, "action.hud.act_2")) {
			t.Fatal(s)
		}
	}
	_, report, err = p.Publication(PublicationOptions{ConfirmRights: true, IncludeWIP: true})
	if err != nil {
		t.Fatal(err)
	}
	for _, s := range report.Coverage.Surfaces {
		if s.Surface == "hud" && (s.WIP != 1 || s.Done != 0 || slices.Contains(s.Missing, "action.hud.act_2")) {
			t.Fatal("WIP/empty translation relabelled or omitted", s)
		}
	}
}

package localizationkit

import (
	"slices"
	"strings"
	"testing"
)

func TestAuthorLocationOrderAndCoverage(t *testing.T) {
	captions := AuthorNavigationCaptions()
	want := []string{"Title", "Sky Palace", "Fillmore", "Bloodpool", "Kasandora", "Aitos", "Marahna", "Northwall", "Death Heim", "End Credits"}
	roots := AuthorLocationRoots()
	var labels []string
	valid := map[string]string{}
	for _, r := range roots {
		labels = append(labels, r.Label)
		valid["place."+r.ID] = r.Label
	}
	if !slices.Equal(labels, want) {
		t.Fatal(labels)
	}
	roots[0].Label = "modified copy"
	if AuthorLocationRoots()[0].Label != "Title" {
		t.Fatal("caller mutated navigation registry")
	}
	for _, profile := range []string{"us", "eu-en", "de", "fr", "jp"} {
		refs, err := AuthorReferences(profile)
		if err != nil {
			t.Fatal(err)
		}
		for _, ref := range refs {
			locations := AuthorMessageLocations(ref.ID)
			if len(locations) == 0 {
				t.Fatal("unreachable route", ref.ID)
			}
			seen := map[string]bool{}
			for _, loc := range locations {
				if captions[loc.RootKey] != loc.RootLabel || captions[loc.GroupKey] != loc.GroupLabel || loc.TitleKey != "" && captions[loc.TitleKey] != loc.Title || loc.ContextKey != "" && captions[loc.ContextKey] == "" {
					t.Fatalf("unregistered navigation caption: %+v", loc)
				}
				if valid[loc.Root] != loc.RootLabel || loc.RootLabel == "" || loc.Title == "" || loc.GroupLabel == "" || !strings.HasPrefix(loc.Group, loc.Root+".") || seen[loc.Root] {
					t.Fatalf("invalid/duplicate location for %s: %+v", ref.ID, loc)
				}
				seen[loc.Root] = true
			}
		}
	}
	captions["root.sky"] = "modified"
	if AuthorNavigationCaptions()["root.sky"] != "Sky Palace" {
		t.Fatal("caller mutated caption registry")
	}
}

func TestAuthorLocationKnownRoutes(t *testing.T) {
	for id, want := range map[string]string{
		"title.selector.continue":                       "place.title.menus",
		"dialogue.event.wrapper_05.call_02.source_00":   "place.sky.start",
		"simulation.event.fillmore.slot_00":             "place.fillmore.story",
		"dialogue.event.relay.northwall":                "place.northwall.story",
		"city.fillmore.name":                            "place.fillmore.names",
		"action.stage_name.death_heim":                  "place.death_heim.names",
		"sky.action_mode.outcome.final_battle":          "place.death_heim.dialogue",
		"dialogue.ending.slot_00":                       "place.ending.dialogue",
		"variant.fr.dormant.angel_dialogue.resource_00": "place.sky.references",
	} {
		if got := AuthorMessageLocation(id).Group; got != want {
			t.Errorf("%s: %s want %s", id, got, want)
		}
	}
	if context := AuthorMessageLocation("city.fillmore.name").Context; !strings.Contains(context, "World-navigation labels") {
		t.Fatal("city-name editor help does not identify the world-navigation surface", context)
	}
	for _, id := range []string{"sim.menu.listen", "sim.miracle.rain.confirm", "system.save.confirm"} {
		locations := AuthorMessageLocations(id)
		if len(locations) < 6 {
			t.Fatal("missing shared town links", id, locations)
		}
		for _, loc := range locations {
			if !strings.Contains(loc.Context, "one translation and one progress status") {
				t.Fatal("unmarked shared link", loc)
			}
		}
	}
	// Malformed editor lookups must not panic or accidentally select a town.
	for _, id := range []string{"", "simulation.event.", "dialogue.event.relay.fillmore_extra"} {
		if loc := AuthorMessageLocation(id); loc.Group == "place.fillmore.story" {
			t.Fatal("prefix collision", id)
		}
	}
}

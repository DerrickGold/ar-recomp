package localizationkit

import (
	"slices"
	"strings"
)

// AuthorLocation is editorial navigation, not another runtime route/format.
// Stable semantic IDs remain the sole identity in saved scripts and contracts.
type AuthorLocation struct {
	Root          string `json:"root"`
	RootLabel     string `json:"root_label"`
	Group         string `json:"group"`
	GroupLabel    string `json:"group_label"`
	Title         string `json:"title"`
	Context       string `json:"context"`
	CategoryOrder int    `json:"-"`
}

type AuthorLocationRoot struct {
	ID    string
	Label string
}

// Playthrough order, deliberately independent of semantic-ID/alphabetic order.
var authorLocationRoots = [...]AuthorLocationRoot{
	{"title", "Title"}, {"sky", "Sky Palace"},
	{"fillmore", "Fillmore"}, {"bloodpool", "Bloodpool"},
	{"kasandora", "Kasandora"}, {"aitos", "Aitos"},
	{"marahna", "Marahna"}, {"northwall", "Northwall"},
	{"death_heim", "Death Heim"}, {"ending", "End Credits"},
}

func AuthorLocationRoots() []AuthorLocationRoot {
	return append([]AuthorLocationRoot(nil), authorLocationRoots[:]...)
}

var authorStartTitles = map[string]string{
	"dialogue.event.wrapper_05.call_00.source_00": "New game — before name entry",
	"dialogue.event.wrapper_05.call_01.source_00": "New game — after name entry",
	"dialogue.event.wrapper_05.call_02.source_00": "Resume saved game — welcome back after Continue",
	"name_entry.prompt_and_alphabet":              "Name entry — prompt and keyboard",
	"name_entry.prompt_and_hiragana":              "Name entry — hiragana keyboard",
	"name_entry.prompt_and_katakana":              "Name entry — katakana keyboard",
	"name_entry.selection_cursor":                 "Name entry — selection cursor",
}
var authorTitleWords = strings.NewReplacer("_", " ", ".", " / ")
var authorCategoryOrder = []string{"start", "story", "menus", "dialogue", "miracles", "offerings", "responses", "reports", "speed", "save", "confirmations", "names", "terms", "action", "references"}

func AuthorMessageLocation(id string) AuthorLocation {
	return AuthorMessageLocations(id)[0]
}

// Shared routes are navigation links, never independent copies of a message.
// Keep this presentation-only: extraction, control contracts, save identity,
// runtime dispatch and progress continue to use the original semantic ID.
func AuthorMessageLocations(id string) []AuthorLocation {
	makeLocations := func(roots []AuthorLocationRoot, category, label, prefix, context string) []AuthorLocation {
		out := make([]AuthorLocation, 0, len(roots))
		if len(roots) > 1 {
			context = "Shared message: these location links edit one translation and one progress status. " + context
		}
		for _, root := range roots {
			out = append(out, AuthorLocation{
				Root: "place." + root.ID, RootLabel: root.Label,
				Group: "place." + root.ID + "." + category, GroupLabel: label,
				Title: authorTitleWords.Replace(strings.TrimPrefix(id, prefix)), Context: strings.TrimSpace(context),
				CategoryOrder: slices.Index(authorCategoryOrder, category),
			})
		}
		return out
	}
	root := func(key, category, label, prefix, context string) []AuthorLocation {
		for _, r := range authorLocationRoots {
			if r.ID == key {
				return makeLocations([]AuthorLocationRoot{r}, category, label, prefix, context)
			}
		}
		return nil
	}
	towns := authorLocationRoots[2:8]
	if title, ok := authorStartTitles[id]; ok {
		out := root("sky", "start", "Introduction, name entry & Continue", "", "Sky Palace introduction or returning to an existing save from the title screen.")
		out[0].Title = title
		return out
	}
	// Explicit town-bearing routes take precedence over shared SIM categories.
	for _, r := range authorLocationRoots[2:9] {
		for _, prefix := range []string{"simulation.event." + r.ID + ".", "dialogue.event.relay." + r.ID} {
			if id == prefix || strings.HasPrefix(id, prefix) && strings.HasSuffix(prefix, ".") {
				return root(r.ID, "story", "Town story & requests", "simulation.event."+r.ID+".", "")
			}
		}
		if id == "city."+r.ID+".name" || id == "town.name."+r.ID || id == "action.stage_name."+r.ID {
			return root(r.ID, "names", "Location names & action title cards", "", "Action title cards use fitted, centered enhanced text.")
		}
	}
	switch {
	case strings.HasPrefix(id, "title."), strings.HasPrefix(id, "sound_test."):
		return root("title", "menus", "Menus & title text", "title.", "Title options support enhanced text. Logo, copyright and sound-test presentation remain native.")
	case strings.HasPrefix(id, "dialogue.ending."), strings.HasPrefix(id, "dialogue.event.wrapper_00.call_00."), strings.HasPrefix(id, "dialogue.event.wrapper_05.call_03."):
		return root("ending", "dialogue", "Ending dialogue", "dialogue.ending.", "Ending and credits rendering is a later phase.")
	case id == "sky.action_mode.outcome.final_battle":
		return root("death_heim", "dialogue", "Final battle dialogue", "sky.action_mode.", "Final-battle message delivered from the Sky Palace.")
	case strings.HasPrefix(id, "system.save."):
		return makeLocations(authorLocationRoots[1:8], "save", "Save progress & quit", "system.save.", "")
	case strings.HasPrefix(id, "system.message_speed."):
		return makeLocations(authorLocationRoots[1:8], "speed", "Message speed", "system.message_speed.", "")
	case strings.HasPrefix(id, "status.report."):
		return makeLocations(authorLocationRoots[1:8], "reports", "Status reports", "status.report.", "")
	case id == "system.choice.yes_no":
		return makeLocations(authorLocationRoots[1:8], "confirmations", "Yes / No choices", "system.choice.", "")
	case strings.HasPrefix(id, "sky.menu."):
		return root("sky", "menus", "Menus", "sky.menu.", "")
	case strings.HasPrefix(id, "sky."):
		return root("sky", "dialogue", "Dialogue — magic & battles", "sky.", "")
	case strings.HasPrefix(id, "sim.menu."):
		return makeLocations(towns, "menus", "Simulation menus", "sim.menu.", "")
	case strings.HasPrefix(id, "sim.miracle."):
		return makeLocations(towns, "miracles", "Miracles", "sim.miracle.", "")
	case strings.HasPrefix(id, "dialogue.offering."), strings.HasPrefix(id, "sim.offerings."), strings.HasPrefix(id, "sim.inventory."):
		return makeLocations(towns, "offerings", "Gifts & inventory", "dialogue.offering.", "")
	case strings.HasPrefix(id, "growth_state."), strings.HasPrefix(id, "enemy.name."):
		return makeLocations(towns, "terms", "Town & monster terms", "", "")
	case strings.HasPrefix(id, "action.hud."):
		return makeLocations(authorLocationRoots[2:9], "action", "Action HUD labels & messages", "action.hud.", "HUD labels, pause and stage messages support enhanced text; counters retain native formatting.")
	case strings.HasPrefix(id, "sim."), strings.HasPrefix(id, "dialogue.event."), strings.Contains(id, ".sim."):
		return makeLocations(towns, "responses", "Dialogue & event responses", "dialogue.event.", "Common event/reference routes; not a separate script for each town.")
	case strings.HasPrefix(id, "required_master_level."):
		return root("sky", "reports", "Status reports", "", "Required-level table reference.")
	}
	// Dormant/regional resources have no proven playthrough location. Keep them
	// reachable and clearly identified without inventing an active town event.
	return root("sky", "references", "Additional regional & table references", "", "Reference-only or dormant resource; active location is not established.")
}

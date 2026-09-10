package localization

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
	RootKey       string `json:"root_key"`
	GroupKey      string `json:"group_key"`
	TitleKey      string `json:"title_key,omitempty"`
	ContextKey    string `json:"context_key,omitempty"`
	Shared        bool   `json:"shared,omitempty"`
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
	"dialogue.event.wrapper_05.call_00.source_00": "title.before_name",
	"dialogue.event.wrapper_05.call_01.source_00": "title.after_name",
	"dialogue.event.wrapper_05.call_02.source_00": "title.continue",
	"name_entry.prompt_and_alphabet":              "title.keyboard",
	"name_entry.prompt_and_hiragana":              "title.hiragana",
	"name_entry.prompt_and_katakana":              "title.katakana",
	"name_entry.selection_cursor":                 "title.cursor",
}

// Presentation IDs only: these never select runtime routes or rewrite scripts.
// Hosts may translate the captions; English remains a deterministic fallback.
var authorNavigationCaptions = map[string]string{
	"group.start":          "Introduction, name entry & Continue",
	"group.story":          "Town story & requests",
	"group.names":          "Location names & action title cards",
	"group.title_menus":    "Menus & title text",
	"group.ending":         "Ending dialogue & credits",
	"group.final_battle":   "Final battle dialogue",
	"group.save":           "Save progress & quit",
	"group.speed":          "Message speed",
	"group.reports":        "Status reports",
	"group.confirmations":  "Yes / No choices",
	"group.sky_menus":      "Menus",
	"group.sky_dialogue":   "Dialogue — magic & battles",
	"group.sim_menus":      "Simulation menus",
	"group.miracles":       "Miracles",
	"group.offerings":      "Gifts & inventory",
	"group.terms":          "Town & monster terms",
	"group.action":         "Action HUD labels & messages",
	"group.responses":      "Dialogue & event responses",
	"group.references":     "Additional regional & table references",
	"context.start":        "Sky Palace introduction or returning to an existing save from the title screen.",
	"context.names":        "World-navigation labels use live plaque colors; action title cards use fitted, centered enhanced text.",
	"context.title":        "Title options and dormant sound-test labels support enhanced text. Logo and copyright artwork remain native.",
	"context.ending":       "Ending dialogue and credits support enhanced text. Credits pages keep native timing; copyright artwork remains native.",
	"context.final_battle": "Final-battle message delivered from the Sky Palace.",
	"context.action":       "HUD labels, pause and stage messages support enhanced text; counters retain native formatting.",
	"context.responses":    "Common event/reference routes; not a separate script for each town.",
	"context.level":        "Required-level table reference.",
	"context.references":   "Reference-only or dormant resource; active location is not established.",
	"title.before_name":    "New game — before name entry",
	"title.after_name":     "New game — after name entry",
	"title.continue":       "Resume saved game — welcome back after Continue",
	"title.keyboard":       "Name entry — prompt and keyboard",
	"title.hiragana":       "Name entry — hiragana keyboard",
	"title.katakana":       "Name entry — katakana keyboard",
	"title.cursor":         "Name entry — selection cursor",
	"context.shared":       "Shared message: these location links edit one translation and one progress status.",
}

// AuthorNavigationCaptions returns a detached caption inventory for host catalogs.
func AuthorNavigationCaptions() map[string]string {
	out := make(map[string]string, len(authorNavigationCaptions)+len(authorLocationRoots))
	for key, text := range authorNavigationCaptions {
		out[key] = text
	}
	for _, root := range authorLocationRoots {
		out["root."+root.ID] = root.Label
	}
	return out
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
		contextText := authorNavigationCaptions[context]
		if len(roots) > 1 {
			contextText = authorNavigationCaptions["context.shared"] + " " + contextText
		}
		for _, root := range roots {
			out = append(out, AuthorLocation{
				Root: "place." + root.ID, RootLabel: root.Label,
				Group: "place." + root.ID + "." + category, GroupLabel: authorNavigationCaptions[label],
				RootKey: "root." + root.ID, GroupKey: label, ContextKey: context, Shared: len(roots) > 1,
				Title: authorTitleWords.Replace(strings.TrimPrefix(id, prefix)), Context: strings.TrimSpace(contextText),
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
		out := root("sky", "start", "group.start", "", "context.start")
		out[0].Title = authorNavigationCaptions[title]
		out[0].TitleKey = title
		return out
	}
	// Explicit town-bearing routes take precedence over shared SIM categories.
	for _, r := range authorLocationRoots[2:9] {
		for _, prefix := range []string{"simulation.event." + r.ID + ".", "dialogue.event.relay." + r.ID} {
			if id == prefix || strings.HasPrefix(id, prefix) && strings.HasSuffix(prefix, ".") {
				return root(r.ID, "story", "group.story", "simulation.event."+r.ID+".", "")
			}
		}
		if id == "city."+r.ID+".name" || id == "town.name."+r.ID || id == "action.stage_name."+r.ID {
			return root(r.ID, "names", "group.names", "", "context.names")
		}
	}
	switch {
	case strings.HasPrefix(id, "credits."):
		return root("ending", "dialogue", "group.ending", "credits.", "context.ending")
	case strings.HasPrefix(id, "title."), strings.HasPrefix(id, "sound_test."):
		return root("title", "menus", "group.title_menus", "title.", "context.title")
	case strings.HasPrefix(id, "dialogue.ending."), strings.HasPrefix(id, "dialogue.event.wrapper_00.call_00."), strings.HasPrefix(id, "dialogue.event.wrapper_05.call_03."):
		return root("ending", "dialogue", "group.ending", "dialogue.ending.", "context.ending")
	case id == "sky.action_mode.outcome.final_battle":
		return root("death_heim", "dialogue", "group.final_battle", "sky.action_mode.", "context.final_battle")
	case strings.HasPrefix(id, "system.save."):
		return makeLocations(authorLocationRoots[1:8], "save", "group.save", "system.save.", "")
	case strings.HasPrefix(id, "system.message_speed."):
		return makeLocations(authorLocationRoots[1:8], "speed", "group.speed", "system.message_speed.", "")
	case strings.HasPrefix(id, "status.report."):
		return makeLocations(authorLocationRoots[1:8], "reports", "group.reports", "status.report.", "")
	case id == "system.choice.yes_no":
		return makeLocations(authorLocationRoots[1:8], "confirmations", "group.confirmations", "system.choice.", "")
	case strings.HasPrefix(id, "sky.menu."):
		return root("sky", "menus", "group.sky_menus", "sky.menu.", "")
	case strings.HasPrefix(id, "sky."):
		return root("sky", "dialogue", "group.sky_dialogue", "sky.", "")
	case strings.HasPrefix(id, "sim.menu."):
		return makeLocations(towns, "menus", "group.sim_menus", "sim.menu.", "")
	case strings.HasPrefix(id, "sim.miracle."):
		return makeLocations(towns, "miracles", "group.miracles", "sim.miracle.", "")
	case strings.HasPrefix(id, "dialogue.offering."), strings.HasPrefix(id, "sim.offerings."), strings.HasPrefix(id, "sim.inventory."):
		return makeLocations(towns, "offerings", "group.offerings", "dialogue.offering.", "")
	case strings.HasPrefix(id, "growth_state."), strings.HasPrefix(id, "enemy.name."):
		return makeLocations(towns, "terms", "group.terms", "", "")
	case strings.HasPrefix(id, "action.hud."):
		return makeLocations(authorLocationRoots[2:9], "action", "group.action", "action.hud.", "context.action")
	case strings.HasPrefix(id, "sim."), strings.HasPrefix(id, "dialogue.event."), strings.Contains(id, ".sim."):
		return makeLocations(towns, "responses", "group.responses", "dialogue.event.", "context.responses")
	case strings.HasPrefix(id, "required_master_level."):
		return root("sky", "reports", "group.reports", "", "context.level")
	}
	// Dormant/regional resources have no proven playthrough location. Keep them
	// reachable and clearly identified without inventing an active town event.
	return root("sky", "references", "group.references", "", "context.references")
}

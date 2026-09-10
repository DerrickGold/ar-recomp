package localizationkit

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"slices"
	"strconv"
	"strings"
)

// Native surface ABI facts, shared by Go discovery and data-only C generation.
// These are adapter bounds, not author-selected layout or rendered font sizes.
type nativeComposeSurface struct {
	SurfaceID        int    `json:"surface_id"`
	Destination      int    `json:"destination"`
	Region           [4]int `json:"region"`
	NativeFontPixels int    `json:"native_font_pixels"`
}

//go:embed data/us-compose-surfaces.json
var composeSurfaceJSON []byte
var nativeComposeSurfaces = func() map[string]nativeComposeSurface {
	var facts map[string]nativeComposeSurface
	if err := json.Unmarshal(composeSurfaceJSON, &facts); err != nil {
		panic("invalid native composer surface facts: " + err.Error())
	}
	return facts
}()

func nativeComposeKind(id string) string {
	switch id {
	case "action.hud.act_1", "action.hud.act_2":
		return "action_act"
	case "action.hud.clear":
		return "action_clear"
	case "action.hud.ready":
		return "action_ready"
	case "action.hud.pause":
		return "action_pause"
	case "action.hud.time_up":
		return "action_time_up"
	case "title.save_choice.labels":
		return "title_options"
	case "title.start_prompt":
		return "title_start"
	case "title.selector.professional":
		return "title_professional"
	case "sound_test.menu.labels":
		return "sound_test"
	case "name_entry.prompt_and_alphabet":
		return "name_entry"
	case "status.report.master_report":
		return "status_master"
	case "status.report.cities_report", "status.report.score_report":
		return "status_table"
	case "system.choice.yes_no":
		return "flow_choice"
	case "system.message_speed.scale_labels":
		return "flow_speed"
	}
	switch {
	case strings.HasPrefix(id, "action.stage_name."):
		return "action_stage"
	case strings.HasPrefix(id, "sky.menu.magic."), strings.HasPrefix(id, "sim.menu.possession."):
		return "menu_selection"
	case strings.HasPrefix(id, "sky.menu."), strings.HasPrefix(id, "sim.menu."):
		return "menu_heading"
	case strings.HasPrefix(id, "city.") && strings.HasSuffix(id, ".name"):
		return "city_name"
	}
	return ""
}

// USRuntimeComposeRoutes verifies native sources and indexed table identities.
// The returned address-only manifest contains no ROM text or executable code.
func (d *Decoder) USRuntimeComposeRoutes() (IRObject, error) {
	if d == nil || d.profile.ID != "us" {
		return nil, fmt.Errorf("runtime routes require the exact US ROM")
	}
	if err := d.verifyUSSoundTestComposer(); err != nil {
		return nil, err
	}
	catalog, err := d.BuildNativeCatalog()
	if err != nil {
		return nil, err
	}
	records := map[string]*NativeMessage{}
	for _, record := range append(slices.Clone(catalog.Messages), catalog.Menu.Segments...) {
		records[record.ID] = record
	}
	tables := map[string]IRObject{}
	for _, table := range irRows(catalog.source.FixedComposerSources, "pointer_tables") {
		tables[irString(table, "id")] = table
	}
	type row struct {
		source, table, selector int
		id, kind                string
		geometry                nativeComposeSurface
	}
	var rows []row
	for _, route := range catalog.SemanticRoutes.Routes {
		kind := nativeComposeKind(route.ID)
		if kind == "" {
			continue
		}
		geometry, ok := nativeComposeSurfaces[kind]
		if !ok {
			return nil, fmt.Errorf("missing native surface %s", kind)
		}
		record := records[route.RecordID]
		if record == nil || route.SourceOffset < 0 || route.SourceOffset >= record.end-record.start {
			return nil, fmt.Errorf("invalid runtime source %s", route.ID)
		}
		r := row{source: offsetPC(record.start + route.SourceOffset), id: route.ID, kind: kind, geometry: geometry, selector: -1}
		tableID := ""
		if strings.HasPrefix(route.ID, "sim.menu.possession.slot_") {
			tableID = "selected_possession"
			r.selector, err = strconv.Atoi(route.ID[strings.LastIndexByte(route.ID, '_')+1:])
			if err != nil {
				return nil, err
			}
		} else if strings.HasPrefix(route.ID, "sky.menu.magic.") {
			tableID = "selected_magic"
		}
		if tableID != "" {
			table := tables[tableID]
			if table == nil {
				return nil, fmt.Errorf("missing composer pointer table %s", tableID)
			}
			targets, ok := table["target_pc24s"].([]string)
			if !ok {
				return nil, fmt.Errorf("invalid composer targets")
			}
			r.table, err = parsePC(irString(table, "source_table_pc24"))
			if err != nil {
				return nil, err
			}
			if tableID == "selected_magic" {
				for index, pc := range targets {
					if pc == pcString(r.source) {
						if r.selector >= 0 {
							return nil, fmt.Errorf("ambiguous magic source %s", route.ID)
						}
						r.selector = index
					}
				}
			}
			if r.selector < 0 || r.selector >= len(targets) || targets[r.selector] != pcString(r.source) {
				return nil, fmt.Errorf("indexed composer source changed: %s", route.ID)
			}
		}
		rows = append(rows, r)
	}
	slices.SortFunc(rows, func(a, b row) int {
		for _, p := range [][2]int{{a.source, b.source}, {a.geometry.Destination, b.geometry.Destination}, {a.table, b.table}, {a.selector, b.selector}} {
			if p[0] != p[1] {
				return p[0] - p[1]
			}
		}
		return strings.Compare(a.id, b.id)
	})
	result := []IRObject{}
	seen := map[[4]int]bool{}
	for _, r := range rows {
		key := [4]int{r.source, r.geometry.Destination, r.table, r.selector}
		if seen[key] {
			return nil, fmt.Errorf("ambiguous runtime composer identity %s", r.id)
		}
		seen[key] = true
		value := IRObject{"semantic_id": r.id, "source_pc24": pcString(r.source), "destination": r.geometry.Destination,
			"surface_kind": r.kind, "surface_id": r.geometry.SurfaceID, "region": r.geometry.Region, "native_font_pixels": r.geometry.NativeFontPixels}
		if r.table != 0 {
			value["source_table_pc24"], value["source_selector"] = pcString(r.table), r.selector
		}
		result = append(result, value)
	}
	if len(result) == 0 {
		return nil, fmt.Errorf("empty runtime composer routes")
	}
	return IRObject{"format": "actraiser-us-runtime-compose-routes", "version": 1, "source_profile": "us", "rom_sha256": d.profile.SHA256, "route_count": len(result), "routes": result}, nil
}

// The modal redraws both counters at one fixed origin and closes with a blank
// source at the same origin. These are native lifetime facts used by the game
// adapter, not an instruction to enable the dormant debug menu.
func (d *Decoder) verifyUSSoundTestComposer() error {
	for _, site := range []struct {
		pc   int
		code []byte
	}{
		{0x0297e4, []byte{0xc2, 0x20, 0xa0, 0x71, 0x98, 0xa9, 0x0b, 0x08, 0x22, 0x60, 0xbf, 0x02, 0xe2, 0x20}},
		{0x029854, []byte{0xc2, 0x20, 0xa0, 0x96, 0x98, 0xa9, 0x0b, 0x08, 0x22, 0x60, 0xbf, 0x02, 0xe2, 0x20, 0xa9, 0x17, 0x8d, 0x2c, 0x21}},
	} {
		if err := d.expectPC(site.pc, site.code, "sound-test composition/lifetime"); err != nil {
			return err
		}
	}
	return nil
}

package localization

import (
	"fmt"
	"slices"
)

func (b *catalogBuilder) verifySlots(set *NativePointerSet, first int, ids []string) error {
	if first < 0 || first+len(ids) > len(set.Slots) {
		return fmt.Errorf("invalid semantic slot layout for %s", set.ID)
	}
	for index, id := range ids {
		slot := set.Slots[first+index]
		message := b.byOffset[slot.target]
		if slot.TargetID == nil || message == nil {
			return fmt.Errorf("%s: verified semantic points at sentinel", set.ID)
		}
		if err := verifySemantic(message, id); err != nil {
			return err
		}
	}
	return nil
}
func (b *catalogBuilder) structuredInventory() (IRObject, error) {
	p := b.p
	towns, _, err := b.pointerSet(p.TownTable, 7, -1, "town_name", "town_name")
	if err != nil {
		return nil, err
	}
	enemies, _, err := b.pointerSet(p.EnemyTable, 4, -1, "enemy_name", "enemy_name")
	if err != nil {
		return nil, err
	}
	if len(catalogFacts.CityIDs) < 6 {
		return nil, fmt.Errorf("missing city semantic facts")
	}
	if err := b.verifySlots(towns, 1, catalogFacts.CityIDs[:6]); err != nil {
		return nil, err
	}
	if err := b.verifySlots(enemies, 0, catalogFacts.EnemyIDs); err != nil {
		return nil, err
	}
	lookups := []IRObject{}
	for _, table := range p.LookupTables {
		set, _, err := b.pointerSet(table.Offset, len(table.SemanticIDs), -1, "dynamic_lookup_text", "lookup."+table.ID)
		if err != nil {
			return nil, err
		}
		if err := b.verifySlots(set, 0, table.SemanticIDs); err != nil {
			return nil, err
		}
		lookups = append(lookups, IRObject{"id": table.ID, "source_table": set.SourceTable, "target_count": len(table.SemanticIDs), "semantic_ids": append([]string{}, table.SemanticIDs...)})
	}
	suffix := ""
	if p.Japanese {
		suffix = "_native"
	}
	angels, err := b.sequential(p.AngelStart, p.AngelEnd, "angel_dialogue"+suffix, "dialogue.angel")
	if err != nil {
		return nil, err
	}
	townsText, err := b.sequential(p.TownStart, p.OfferingTable, "town_dialogue"+suffix, "dialogue.town")
	if err != nil {
		return nil, err
	}
	inventory := IRObject{"angel_sequential_records": len(angels), "town_sequential_records": len(townsText),
		"handler_table":         IRObject{"file_offset": fileOffset(p.HandlerTable), "end_file_offset_exclusive": fileOffset(p.TownStart), "byte_count": p.TownStart - p.HandlerTable},
		"dynamic_lookup_tables": lookups}
	if p.Japanese {
		if p.PostTextEnd == nil {
			return nil, fmt.Errorf("missing Japanese post-text bound")
		}
		_, ends, err := b.pointerSet(p.OfferingTable, 21, *p.PostTextEnd, "offering_text_native", "offering")
		if err != nil {
			return nil, err
		}
		if len(ends) == 0 {
			return nil, fmt.Errorf("offering table has no text targets")
		}
		post, err := b.sequential(slices.Max(ends), *p.PostTextEnd, "post_offering_or_ending_native", "post_text.unassigned")
		if err != nil {
			return nil, err
		}
		inventory["post_text_sequential_records"] = len(post)
		inventory["unicode_mapping"] = IRObject{"status": "all_observed_source_glyphs_mapped",
			"preservation": "Hiragana, katakana, Japanese punctuation, selectable spacing marks, and following DE/DF diacritics decode to Unicode. Verified cursor/selector tiles are typed icons; any future unassigned tile remains a lossless native_glyphs operation."}
	} else {
		if p.EndingTable == nil {
			return nil, fmt.Errorf("missing Western ending table")
		}
		if _, _, err := b.pointerSet(p.OfferingTable, 21, *p.EndingTable, "offering_text", "offering"); err != nil {
			return nil, err
		}
		if _, _, err := b.pointerSet(*p.EndingTable, 8, b.d.profile.Dictionary, "ending_text", "ending"); err != nil {
			return nil, err
		}
		inventory["dictionary"] = IRObject{"file_offset": fileOffset(b.d.profile.Dictionary), "snes": cursorAddress(b.d.profile.Dictionary), "entries": 128, "entry_bytes": 12}
	}
	stages, _, err := b.pointerSet(p.StageTable, 7, -1, "action_stage_name", "action.stage_name")
	if err != nil {
		return nil, err
	}
	if err := b.verifySlots(stages, 0, catalogFacts.CityIDs); err != nil {
		return nil, err
	}
	titles, err := b.sequential(p.TitleStart, p.TitleEnd, "title_and_mode_menu", "title_menu")
	if err != nil {
		return nil, err
	}
	action, err := b.sequential(p.ActionStart, p.ActionEnd, "action_hud_label", "action.hud")
	if err != nil {
		return nil, err
	}
	for index, start := range action {
		if index < len(catalogFacts.ActionIDs) && catalogFacts.ActionIDs[index] != "" {
			if err := verifySemantic(b.byOffset[start], catalogFacts.ActionIDs[index]); err != nil {
				return nil, err
			}
		}
	}
	sound, err := b.sequential(p.SoundStart, p.SoundEnd, "sound_test_menu", "sound_test")
	if err != nil {
		return nil, err
	}
	inventory["localized_ui"] = IRObject{"action_stage_name_pointer_slots": 7, "title_and_mode_menu_records": len(titles),
		"action_hud_label_records": len(action), "sound_test_menu_records": len(sound)}
	return inventory, nil
}

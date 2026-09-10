package localizationkit

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"reflect"
	"slices"
	"strings"
	"testing"
)

func catalogTestDecoder(t testing.TB, rom []byte) *Decoder {
	t.Helper()
	glyphs := map[byte]string{}
	for code := byte(0x20); code < 0x7f; code++ {
		glyphs[code] = string(rune(code))
	}
	d, err := newDecoder(rom, decoderProfile{ID: "test", Encoding: "direct-glyph", Glyphs: glyphs})
	if err != nil {
		t.Fatal(err)
	}
	return d
}

// The private pipeline consumes typed, freshly produced source evidence only.
func emptyCatalogCensus() *NativeSourceCensus {
	composer := IRObject{}
	for _, key := range []string{"pointer_tables", "direct_sources", "indexed_direct_sources", "dynamic_reports", "name_entry_sources", "flow_sources"} {
		composer[key] = []IRObject{}
	}
	return &NativeSourceCensus{
		SourceReferenceSeeds: IRObject{"references": []IRObject{}},
		FixedComposerSources: composer,
		NestedHandlerSources: IRObject{"matrices": []IRObject{}},
		DialogueForwarding:   IRObject{"wrappers": []IRObject{}},
	}
}

func TestCatalogProfilesAndAtomicFailure(t *testing.T) {
	seen := map[string]bool{}
	for _, p := range catalogFacts.Profiles {
		if seen[p.ID] || sourceProfiles[p.ID].ID != p.ID {
			t.Fatal("duplicate/unknown catalogue profile", p.ID)
		}
		seen[p.ID] = true
		if p.Japanese != (p.ID == "jp") || len(p.InteractiveRoutes) == 0 || len(p.TitleRouteIDs) == 0 {
			t.Fatal("missing regional catalogue contract", p.ID)
		}
		for _, bound := range []int{p.TownTable, p.EnemyTable, p.AngelStart, p.AngelEnd, p.HandlerTable, p.TownStart, p.OfferingTable, p.StageTable, p.TitleStart, p.TitleEnd, p.ActionStart, p.ActionEnd, p.SoundStart, p.SoundEnd} {
			if bound < 0 || bound >= romSize {
				t.Fatal("catalogue bound outside supported image", p.ID, bound)
			}
		}
	}
	if len(seen) != len(decoderFacts.Profiles) || len(catalogFacts.CityIDs) != 7 || len(catalogFacts.CityKeys) != 6 || len(catalogFacts.EnemyIDs) != 4 {
		t.Fatal("catalogue/reader semantic facts differ")
	}
	for _, d := range []*Decoder{nil, {rom: make([]byte, 64)}, {rom: make([]byte, 64), profile: decoderProfile{ID: "us"}}} {
		if result, err := d.BuildNativeCatalog(); err == nil || result != nil {
			t.Fatal("invalid decoder published partial catalogue", err)
		}
	}
}

func TestCatalogPointersAliasesAndBoundaries(t *testing.T) {
	rom := make([]byte, 128)
	copy(rom, []byte{0x20, 0x80, 0x20, 0x80, 0x22, 0x80, 0x30, 0x80})
	copy(rom[32:], []byte{'A', 0, 'A', 0})
	b := newCatalogBuilder(catalogTestDecoder(t, rom), catalogProfile{ID: "test"})
	set, ends, err := b.pointerSet(0, 4, 48, "town_name", "town_name")
	if err != nil || len(b.messages) != 2 || !reflect.DeepEqual(ends, []int{34, 34, 36}) {
		t.Fatalf("pointer aliases/identical separate records: %+v %v", set, err)
	}
	if *set.Slots[0].TargetID != *set.Slots[1].TargetID || *set.Slots[0].TargetID == *set.Slots[2].TargetID || set.Slots[3].TargetID != nil || set.Slots[3].Classification != "sentinel_or_next_table" {
		t.Fatal("pointer identity/sentinel lost")
	}
	message := b.messages[0]
	for i := 0; i < 2; i++ {
		alias, err := b.add(32, 48, "action_stage_name", "alias.stage", "decoded_structure")
		if err != nil || alias != message {
			t.Fatal("physical alias decoded twice")
		}
	}
	if !reflect.DeepEqual(message.CategoryAliases, []string{"action_stage_name"}) || !reflect.DeepEqual(message.CandidateSemanticIDs, []string{"town_name.00", "town_name.01", "alias.stage"}) {
		t.Fatal("alias order or uniqueness changed", message)
	}
	if err := verifySemantic(message, "town.test"); err != nil {
		t.Fatal(err)
	}
	if verifySemantic(message, "town.test") != nil || verifySemantic(message, "town.other") == nil || message.VerifiedSemanticID != "town.test" {
		t.Fatal("conflicting semantic identity accepted or corrupted")
	}
	if b.verifySlots(set, 3, []string{"sentinel"}) == nil || b.verifySlots(set, -1, []string{"bad"}) == nil {
		t.Fatal("invalid verified slot accepted")
	}
	for _, args := range [][3]int{{-1, 1, 48}, {127, 1, 48}, {0, -1, 48}, {0, 0x4001, 48}, {100, 1, -1}} {
		if result, _, err := b.pointerSet(args[0], args[1], args[2], "test", "test"); err == nil || result != nil {
			t.Fatal("bad pointer bounds/RAM pointer accepted", args)
		}
	}
	copy(rom, []byte{0xff, 0xff})
	bad := newCatalogBuilder(catalogTestDecoder(t, rom), catalogProfile{})
	if result, _, err := bad.pointerSet(0, 1, -1, "test", "test"); err == nil || result != nil {
		t.Fatal("out-of-image pointer accepted")
	}
	if _, err := b.sequential(32, 36, "sequence", "sequence"); err != nil {
		t.Fatal(err)
	}
	for _, end := range []int{31, 33, 129} {
		if _, err := b.sequential(32, end, "sequence", "sequence"); err == nil {
			t.Fatal("cached record/bounds overrun accepted", end)
		}
	}
	unterminated := newCatalogBuilder(catalogTestDecoder(t, []byte{'A', 'B'}), catalogProfile{})
	if _, err := unterminated.sequential(0, 2, "test", "test"); err == nil {
		t.Fatal("unterminated bounded record accepted")
	}
	for _, bounds := range [][2]int{{128, -1}, {32, 32}} {
		if message, err := b.d.makeMessage(bounds[0], bounds[1], Interactive); err == nil || message != nil {
			t.Fatal("empty record admitted by catalogue", bounds)
		}
	}
}

func TestCatalogReferenceContainmentAndExpansion(t *testing.T) {
	rom := []byte{'A', 'B', 'C', 'D', 0}
	b := newCatalogBuilder(catalogTestDecoder(t, rom), catalogProfile{ID: "test"})
	census := emptyCatalogCensus()
	refs := []IRObject{{"source_pc24": "$00:8000", "reference_kind": "interpreter_immediate_y"},
		{"source_pc24": "$00:8002", "reference_kind": "dialogue_wrapper_immediate_y"},
		{"source_pc24": "$00:8002", "reference_kind": "interpreter_immediate_y"}}
	census.SourceReferenceSeeds["references"] = refs
	resolution, expansion, err := b.expandSeeds(census, &NativeMenuCatalog{})
	if err != nil || resolution["all_current_seeds_mapped"] != true || expansion["added_record_count"] != 2 || len(b.messages) != 2 {
		t.Fatal("overlapping original seeds were discarded", resolution, expansion, err)
	}
	if refs[1]["resolved_record_id"] != b.messages[1].ID || refs[1]["source_offset_within_record"] != 0 || refs[2]["resolved_record_id"] != b.messages[1].ID {
		t.Fatal("exact/alias seed identity lost", refs)
	}
	refs = append(refs, IRObject{"source_pc24": "$00:8003"}, IRObject{"source_pc24": "$00:8005"})
	census.SourceReferenceSeeds["references"] = refs
	resolution, err = resolveReferences(census, b.messages, nil)
	if err != nil || refs[3]["resolved_record_id"] != b.messages[1].ID || refs[3]["source_offset_within_record"] != 1 || refs[4]["resolved_record_id"] != nil || resolution["unmapped_unique_source_count"] != 1 {
		t.Fatal("closest interval or exclusive end incorrect", resolution, err)
	}
	if _, err := resolveReferences(census, b.messages, []*NativeMessage{b.messages[0]}); err == nil {
		t.Fatal("ambiguous exact source accepted")
	}
	for _, kind := range []string{"fixed_composer_direct_test", "unknown"} {
		census.SourceReferenceSeeds["references"] = []IRObject{{"source_pc24": "$00:8000", "reference_kind": kind}}
		fresh := newCatalogBuilder(b.d, b.p)
		if _, _, err := fresh.expandSeeds(census, &NativeMenuCatalog{}); err == nil || len(fresh.messages) != 0 {
			t.Fatal("non-dialogue seed decoded using interactive grammar")
		}
	}
	for _, address := range []string{"", "$00:0001", "$80:8000", "0x008000", "$00:ZZZZ"} {
		if _, err := parsedOffset(address); err == nil {
			t.Fatal("invalid address accepted", address)
		}
	}
}

func TestCatalogFixedRootsAndIndicators(t *testing.T) {
	rom := []byte{'A', 0, 'B', 0, '<', '>', 0, 'Z', 'Z'}
	d := catalogTestDecoder(t, rom)
	census := emptyCatalogCensus()
	census.FixedComposerSources["direct_sources"] = []IRObject{{"id": "b", "source_pc24": "$00:8002"}, {"id": "a", "source_pc24": "$00:8002"}}
	census.FixedComposerSources["flow_sources"] = []IRObject{{"id": "selector", "source_pc24": "$00:8004", "classification": "typed_non_language_indicator"}}
	p := sourceProfile{MenuStart: 0, MenuEnd: len(rom)}
	menu, err := d.fixedCatalog(p, census)
	if err != nil || len(menu.Segments) != 2 || menu.Segments[0].start != 2 || !reflect.DeepEqual(menu.Segments[0].CandidateSemanticIDs, []string{"menu.a", "menu.b"}) {
		t.Fatal("composer scanned unowned bytes or lost aliases", menu, err)
	}
	indicator := menu.Segments[1]
	if len(indicator.Operations) != 2 || indicator.Operations[0]["op"] != "insert_icon" || indicator.Operations[1]["op"] != "end" || *indicator.VisibleUnits != 1 || indicator.Source.ByteCount != 3 {
		t.Fatal("selector not rewritten independently of its raw provenance", indicator)
	}
	census.FixedComposerSources["numeric_only_source"] = IRObject{"source_pc24": "$00:8002"}
	if result, err := d.fixedCatalog(p, census); err == nil || result != nil {
		t.Fatal("conflicting composer classification accepted")
	}
	delete(census.FixedComposerSources, "numeric_only_source")
	for _, address := range []string{"$00:8007", "$00:8009"} {
		census.FixedComposerSources["direct_sources"] = []IRObject{{"id": "bad", "source_pc24": address}}
		if result, err := d.fixedCatalog(p, census); err == nil || result != nil {
			t.Fatal("unterminated/out-of-range composer accepted", address)
		}
	}
}

func TestCatalogOwnershipAndDynamicDiagnostics(t *testing.T) {
	spans := []sourceInterval{{8, 10}, {3, 5}, {5, 8}, {0, 1}, {4, 7}, {12, 12}}
	original := slices.Clone(spans)
	count, size, err := intervalStats(spans)
	if err != nil || count != 3 || size != 8 || !reflect.DeepEqual(spans, original) {
		t.Fatal("overlap/adjacency union or caller ownership changed", count, size, err)
	}
	if _, err := mergeIntervals([]sourceInterval{{3, 2}}); err == nil {
		t.Fatal("reversed interval accepted")
	}
	if units := visibleUnits([]Operation{{"op": "text", "value": " A é あ\t\u00a0\u2028\x1c\x1d\x1e\x1f"}, {"op": "native_glyphs", "codes": "A0  A1\tA2"}, {"op": "insert_icon"}}); units != 7 {
		t.Fatal("Unicode/whitespace/glyph visibility mismatch", units)
	}
	b := newCatalogBuilder(catalogTestDecoder(t, make([]byte, 64)), catalogProfile{LookupTables: []catalogLookup{{SemanticIDs: []string{"test.b", "test.a"}}}})
	for _, operation := range []string{"insert_master_name", "format_number", "insert_indexed_text"} {
		if !recordHasLanguage(&NativeMessage{Operations: []Operation{{"op": operation}}}) {
			t.Fatal("dynamic-only language lost", operation)
		}
	}
	record := &NativeMessage{ID: "dynamic", Operations: []Operation{{"op": "insert_master_name"}, {"op": "format_number", "value": "population"}, {"op": "insert_indexed_text", "args_hex": "AA BB CC DD"}}}
	b.messages = []*NativeMessage{record, {VerifiedSemanticID: "test.a"}}
	dynamic := b.dynamicCensus(b.messages)
	if dynamic["complete"] != false || dynamic["unresolved_operation_count"] != 1 || !reflect.DeepEqual(dynamic["missing_lookup_target_semantic_ids"], []string{"test.b"}) {
		t.Fatal("dynamic unknowns hidden", dynamic)
	}
	unresolved := irRows(dynamic, "unresolved_operations")[0]
	if unresolved["native_address"] != nil || unresolved["args_hex"] != "AA BB CC DD" {
		t.Fatal("unresolved source arguments lost", unresolved)
	}
	b.messages = nil
	categories := []string{"angel_dialogue", "town_name", "unknown", "unknown", "unknown"}
	for index, category := range categories {
		record, err := b.add(index, index+1, category, fmt.Sprint(index), "test")
		if err != nil {
			t.Fatal(err)
		}
		record.Operations = []Operation{{"op": "text", "value": "A"}}
	}
	b.messages[3].Operations = []Operation{{"op": "format_number", "value": "population"}}
	b.messages[3].Classification = "typed_numeric_only"
	b.messages[4].Operations = []Operation{{"op": "end"}}
	census := emptyCatalogCensus()
	resolution, err := resolveReferences(census, b.messages, nil)
	if err != nil {
		t.Fatal(err)
	}
	ownership, err := b.localOwnership(census, &NativeMenuCatalog{}, resolution)
	want := []string{"dormant_or_release_variant_text", "bounded_consumer_catalog_text", "unclassified", "verified_non_language_numeric_descriptor", "verified_non_language_empty_record"}
	if err != nil || ownership["complete"] != false || ownership["unclassified_record_count"] != 1 || ownership["owned_rom_byte_count"] != 5 {
		t.Fatal("incomplete ownership hidden", ownership, err)
	}
	for index, record := range b.messages {
		if record.SourceOwnership != want[index] {
			t.Fatal("ownership classification", record)
		}
	}
}

func TestCatalogRoutesAndContinuationHashes(t *testing.T) {
	d := catalogTestDecoder(t, []byte{'A', 'B', 'C', 0})
	b := newCatalogBuilder(d, catalogProfile{ID: "test", InteractiveRoutes: map[int][]string{0x8100: {"test.continuation"}}})
	record, err := b.add(0, 4, "angel_dialogue", "test", "test")
	if err != nil {
		t.Fatal(err)
	}
	census := emptyCatalogCensus()
	census.SourceReferenceSeeds["references"] = []IRObject{{"source_pc24": "$00:8001", "reference_kind": "interpreter_immediate_y", "via_call_site": "$00:8100"}}
	if _, err := resolveReferences(census, b.messages, nil); err != nil {
		t.Fatal(err)
	}
	routes, err := b.semanticRoutes(census, &NativeMenuCatalog{})
	if err != nil || !routes.Complete || routes.RouteCount != 1 || routes.Routes[0].Operations[0]["value"] != "BC" || routes.Routes[0].SourceOffset != 1 {
		t.Fatal("continuation reused whole-record operations", routes, err)
	}
	r := routeBuilder{records: map[string]*NativeMessage{record.ID: record}, routes: map[string]*NativeSemanticRoute{}}
	for _, provenance := range []string{"a", "a", "b"} {
		if err := r.add("same", record.ID, 0, "test", provenance, false); err != nil {
			t.Fatal(err)
		}
	}
	if !reflect.DeepEqual(r.routes["same"].Provenance, []string{"a", "b"}) || r.add("same", record.ID, 1, "test", "c", false) == nil || r.add("bad", "missing", 0, "test", "", false) == nil || r.add("end", record.ID, 4, "test", "", false) == nil {
		t.Fatal("route source/provenance validation")
	}
	b.messages = nil
	if _, err := b.semanticRoutes(census, &NativeMenuCatalog{Segments: []*NativeMessage{record}}); err == nil {
		t.Fatal("interactive continuation decoded within fixed-composer record")
	}
	for _, candidate := range []string{"missing-slot", "town_name.-1", "town_name.9", "action.stage_name.7", "town_name.not-a-number"} {
		if _, err := pointerRoute(NativePointerSlot{CandidateSemanticID: candidate}); err == nil {
			t.Fatal("invalid semantic slot", candidate)
		}
	}
}

func TestCatalogOperationHashCanonicalJSON(t *testing.T) {
	// Handwritten canonical JSON, not a second invocation of Go's encoder.
	// Python's ensure_ascii=False preserves actual separators, while a literal
	// backslash-u remains escaped. Angle brackets/ampersands are not HTML escaped.
	value := "é\u2028\u2029<&>\\u2028\\\u2029\n\x01"
	canonical := "[{\"op\":\"text\",\"value\":\"é\u2028\u2029<&>\\\\u2028\\\\\u2029\\n\\u0001\"},{\"op\":\"end\"}]"
	want := fmt.Sprintf("%x", sha256.Sum256([]byte(canonical)))
	got, err := operationHash([]Operation{{"value": value, "op": "text"}, {"op": "end"}})
	if err != nil || got != want {
		t.Fatal("operation hash JSON contract differs", got, want, err)
	}
	if _, err := operationHash([]Operation{{"bad": make(chan int)}}); err == nil {
		t.Fatal("invalid JSON hash accepted")
	}
}

func TestCatalogCrossReleaseAlignment(t *testing.T) {
	ids := []string{"us", "eu-en", "de", "fr", "jp"}
	var catalogs []*NativeCatalog
	for _, id := range ids {
		catalogs = append(catalogs, &NativeCatalog{releaseID: id, SemanticRoutes: &NativeSemanticCatalog{Complete: true, Routes: []*NativeSemanticRoute{
			{ID: "shared", RecordID: "physical." + id}, {ID: "variant." + id, RecordID: "physical." + id},
		}}})
	}
	all, err := AlignNativeCatalogs(catalogs...)
	if err != nil || !all.Complete || all.RouteCount != 6 || all.AllReleaseCount != 1 || all.VariantCount != 5 || !reflect.DeepEqual(all.Routes[0].Releases, ids) {
		t.Fatal("cross-release logical identity", all, err)
	}
	slices.Reverse(catalogs)
	reversed, err := AlignNativeCatalogs(catalogs...)
	if err != nil || !reflect.DeepEqual(all, reversed) {
		t.Fatal("alignment depends on input order", reversed, err)
	}
	partial, err := AlignNativeCatalogs(catalogs[0])
	if err != nil || partial.Complete || !reflect.DeepEqual(partial.Missing, ids[:4]) || partial.AllReleaseCount != 0 {
		t.Fatal("missing releases hidden", partial, err)
	}
	catalogs[0].SemanticRoutes.Complete = false
	if report, err := AlignNativeCatalogs(catalogs...); err != nil || report.Complete {
		t.Fatal("incomplete local routes hidden", report, err)
	}
	for _, inputs := range [][]*NativeCatalog{{nil}, {catalogs[0], catalogs[0]}, {{releaseID: "unknown", SemanticRoutes: &NativeSemanticCatalog{}}}, {{releaseID: "us"}}} {
		if report, err := AlignNativeCatalogs(inputs...); err == nil || report != nil {
			t.Fatal("bad alignment inputs published report", inputs)
		}
	}
	catalogs[0].SemanticRoutes.Routes = append(catalogs[0].SemanticRoutes.Routes, catalogs[0].SemanticRoutes.Routes[0])
	if report, err := AlignNativeCatalogs(catalogs[0]); err == nil || report != nil {
		t.Fatal("duplicate route counted as another release")
	}
	empty, err := AlignNativeCatalogs()
	if err != nil || empty.Complete || !reflect.DeepEqual(empty.Missing, ids) || len(empty.Routes) != 0 {
		t.Fatal("empty alignment report", empty, err)
	}
}

func FuzzCatalogRecordBounds(f *testing.F) {
	for _, data := range [][]byte{nil, {'A', 0, 'B', 1}, {0x20, 0x80}, {9, 0xff, 0}, bytes.Repeat([]byte{'A'}, 32)} {
		f.Add(data, uint16(0))
	}
	f.Fuzz(func(t *testing.T, data []byte, start uint16) {
		if len(data) > 1024 {
			return
		}
		b := newCatalogBuilder(catalogTestDecoder(t, data), catalogProfile{ID: "test"})
		position := int(start)
		_, _ = b.sequential(position, len(data), "test", "test")
		_, _, _ = b.pointerSet(0, min(8, len(data)/2), -1, "test", "test")
		for _, record := range b.messages {
			if record.start < 0 || record.end > len(data) || record.end <= record.start || !strings.HasPrefix(record.ID, "native.test.") {
				t.Fatal("catalogue escaped input bounds", record)
			}
		}
	})
}

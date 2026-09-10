package localization

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

func syntheticProfile(id string) decoderProfile {
	profile := decoderProfile{ID: id, Encoding: "dictionary-12", Dictionary: 512,
		Glyphs: map[byte]string{0x20: " ", 0x40: " ", 'A': "A", 'B': "B", 'Z': "Z"}}
	if id == "de" || id == "fr" {
		profile.FullEntrySeparator = " "
	}
	return profile
}

func textOf(record Record) string {
	var result strings.Builder
	for _, op := range record.Operations {
		if op["op"] == "text" {
			result.WriteString(op["value"].(string))
		}
	}
	return result.String()
}

func TestDictionaryConsumerBoundaries(t *testing.T) {
	for _, id := range []string{"us", "eu-en", "de", "fr"} {
		for token := 0x80; token <= 0xff; token++ {
			for stop := 0; stop < 12; stop++ {
				for _, delimiter := range []byte{0x20, 0x00, 0x40} {
					rom := make([]byte, 2048)
					rom[0], rom[1] = byte(token), 'Z'
					entry := bytes.Repeat([]byte{'A'}, 12)
					entry[stop] = delimiter
					copy(rom[512+(token&127)*12:], entry)
					d, err := newDecoder(rom, syntheticProfile(id))
					if err != nil {
						t.Fatal(err)
					}
					for _, consumer := range []Consumer{Interactive, FixedComposer} {
						want := append([]byte(nil), entry...)
						if delimiter == 0x20 {
							want = want[:stop+1]
						} else if delimiter == 0 && consumer == FixedComposer {
							want = want[:stop]
						} else if consumer == Interactive && (id == "de" || id == "fr") {
							want = append(want, ' ')
						}
						got, err := d.dictionaryEntry(byte(token), consumer)
						if err != nil || !bytes.Equal(got, want) {
							t.Fatalf("%s %s token=%02X stop=%d delimiter=%02X: %X != %X (%v)", id, consumer, token, stop, delimiter, got, want, err)
						}
						record, err := d.DecodeRecord(consumer, 0, 3, true)
						if err != nil || !record.Terminated || record.End != 3 || !reflect.DeepEqual(record.DictionaryTokens, []string{fmt.Sprintf("%02X", token)}) {
							t.Fatalf("decoded dictionary cursor/token mismatch: %+v %v", record, err)
						}
					}
				}
			}
		}
	}
}

func TestConsumerControlsAndOwnership(t *testing.T) {
	rom := make([]byte, 2048)
	copy(rom, []byte{'A', 7, 'B', 1, 'Z', 0})
	d, err := newDecoder(rom, syntheticProfile("us"))
	if err != nil {
		t.Fatal(err)
	}
	// Mutating caller storage must not change an already verified decoder.
	rom[0] = 'Z'
	interactive, _ := d.DecodeRecord(Interactive, 0, 6, true)
	fixed, _ := d.DecodeRecord(FixedComposer, 0, 6, false)
	if interactive.End != 4 || fixed.End != 4 || textOf(interactive) != "A" || textOf(fixed) != "AB" {
		t.Fatalf("grammar/ownership mismatch: %+v / %+v", interactive, fixed)
	}
	if interactive.Operations[1]["op"] != "native_control" || fixed.Operations[1]["op"] != "composer_noop_control" {
		t.Fatal("shared storage must not merge grammars")
	}
	continued, _ := d.DecodeRecord(Interactive, 0, 6, false)
	if continued.End != 6 || !continued.Terminated || textOf(continued) != "AZ" {
		t.Fatal(continued)
	}
	for _, bounds := range [][2]int{{-1, 3}, {4, 3}, {0, 2049}} {
		if _, err := d.DecodeRecord(Interactive, bounds[0], bounds[1], true); err == nil {
			t.Fatalf("accepted bounds %v", bounds)
		}
	}
	if _, err := d.DecodeRecord("wrong", 0, 1, true); err == nil {
		t.Fatal("unknown reader")
	}
	if _, err := d.dictionaryEntry(0x7f, FixedComposer); err == nil {
		t.Fatal("invalid token")
	}
	if _, err := d.dictionaryEntry(0x80, "wrong"); err == nil {
		t.Fatal("unknown dictionary consumer")
	}
	if _, err := NewDecoder(make([]byte, romSize)); err == nil {
		t.Fatal("unrecognized ROM accepted")
	}
	if _, err := NewDecoder(make([]byte, romSize+512)); err == nil {
		t.Fatal("copier-header ROM silently normalized")
	}
	if _, err := newDecoder(make([]byte, 2047), syntheticProfile("us")); err == nil {
		t.Fatal("truncated dictionary accepted")
	}
}

func TestTruncatedControls(t *testing.T) {
	for _, consumer := range []Consumer{Interactive, FixedComposer} {
		for code, count := range map[byte]int{7: 1, 8: 4, 9: 3, 10: 6, 11: 1} {
			if consumer == FixedComposer && (code == 7 || code == 10) {
				continue
			}
			for available := 0; available < count; available++ {
				rom := make([]byte, 2048)
				rom[0] = code
				d, _ := newDecoder(rom, syntheticProfile("us"))
				record, err := d.DecodeRecord(consumer, 0, available+1, true)
				kind := "truncated_native_control"
				if consumer == FixedComposer {
					kind = "truncated_composer_control"
				}
				if err != nil || record.Terminated || record.End != available+1 || record.Operations[0]["op"] != kind {
					t.Fatalf("%s %02X/%d: %+v %v", consumer, code, available, record, err)
				}
			}
		}
	}
}

func TestContextualGlyphsAndKana(t *testing.T) {
	profile := decoderProfile{ID: "jp", Encoding: "direct-glyph", PopulationCodes: [2]byte{0x3a, 0x3b},
		Glyphs: map[byte]string{0xb6: "カ", 0x3a: "」", 0x3b: "「"}}
	position := 0
	profile.PopulationSource = &position
	d, _ := newDecoder([]byte{0x3a, 0x3b, 0xb6, 0xde, 0xdf, 0}, profile)
	interactive, _ := d.DecodeRecord(Interactive, 0, 6, true)
	fixed, _ := d.DecodeRecord(FixedComposer, 0, 6, true)
	if textOf(interactive) != "」「ガ\u309a" {
		t.Fatal(interactive)
	}
	if fixed.Operations[0]["value"] != "status.population" || fixed.Operations[1]["part_index"] != 1 {
		t.Fatal(fixed)
	}
	if textOf(fixed) != "ガ\u309a" {
		t.Fatal(fixed)
	}
	// Pointer identity, not shared atlas codes, grants icon semantics.
	outside, err := d.DecodeRecord(FixedComposer, 1, 6, true)
	if err != nil || textOf(outside) != "「ガ\u309a" {
		t.Fatalf("context leaked into adjacent source: %+v %v", outside, err)
	}
	d, _ = newDecoder([]byte{0xde, 0xdf, 0}, profile)
	orphan, _ := d.DecodeRecord(Interactive, 0, 3, true)
	if orphan.Operations[0]["op"] != "native_diacritic" || orphan.Operations[1]["kind"] != "handakuten" {
		t.Fatal(orphan)
	}
	if _, err := d.dictionaryEntry(0x80, FixedComposer); err == nil {
		t.Fatal("direct atlas treated as dictionary")
	}
}

func TestProfilesAndSourceBounds(t *testing.T) {
	ids, hashes := map[string]bool{}, map[string]bool{}
	for _, profile := range decoderFacts.Profiles {
		digest, err := hex.DecodeString(profile.SHA256)
		if err != nil || len(digest) != 32 || hashes[profile.SHA256] || ids[profile.ID] {
			t.Fatalf("invalid/duplicate embedded identity: %+v", profile)
		}
		ids[profile.ID], hashes[profile.SHA256] = true, true
	}
	if !reflect.DeepEqual(ids, map[string]bool{"us": true, "eu-en": true, "de": true, "fr": true, "jp": true}) {
		t.Fatalf("release coverage changed: %v", ids)
	}
	if _, err := newDecoder(nil, decoderProfile{}); err == nil {
		t.Fatal("unknown encoding accepted")
	}
	profile := decoderProfile{Encoding: "direct-glyph"}
	for _, position := range []int{-1, 1} {
		profile.NamePrefix = &position
		if _, err := newDecoder([]byte{0}, profile); err == nil {
			t.Fatal("invalid prefix source accepted")
		}
		profile.NamePrefix, profile.PopulationSource = nil, &position
		if _, err := newDecoder([]byte{0}, profile); err == nil {
			t.Fatal("invalid population source accepted")
		}
		profile.PopulationSource = nil
	}
	profile.FullEntrySeparator = "wrong"
	if _, err := newDecoder([]byte{0}, profile); err == nil {
		t.Fatal("invalid dictionary separator accepted")
	}
	profile.FullEntrySeparator = ""
	call := 0x8003
	profile.SpeedSourceCall = &call
	for _, rom := range [][]byte{{0}, {0xa0, 0, 0x80}, {0, 0, 0x80, 0}, {0xa0, 0xff, 0xff, 0}, {0xa0, 0, 0, 0}, {0xa0, 0, 0x80, 0}} {
		if _, err := newDecoder(rom, profile); err == nil {
			t.Fatalf("invalid speed-source proof accepted: %x", rom)
		}
	}
	profile.SpeedSourceCall = nil
	d, err := newDecoder(make([]byte, maxRecordBytes+1), profile)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := d.DecodeRecord(Interactive, 0, maxRecordBytes+1, true); err == nil {
		t.Fatal("oversized record accepted")
	}
	var absent *Decoder
	if _, err := absent.DecodeRecord(Interactive, 0, 0, true); err == nil {
		t.Fatal("nil decoder accepted")
	}
	for _, address := range []int{-1, 0x7fff, 0x800000} {
		if _, err := pc24Offset(address); err == nil {
			t.Fatalf("invalid LoROM source accepted: %x", address)
		}
	}
	if cursorAddress(0x8000) != "$01:8000" {
		t.Fatal("bank-crossing cursor is not LoROM")
	}
}

func TestNamePrefixIsConsumerSpecific(t *testing.T) {
	profile := syntheticProfile("us")
	prefix := 64
	profile.NamePrefix, profile.NameSeparator = &prefix, " "
	rom := make([]byte, 2048)
	rom[0] = 6
	copy(rom[prefix:], []byte{'A', 'B', 0})
	d, err := newDecoder(rom, profile)
	if err != nil {
		t.Fatal(err)
	}
	interactive, err := d.DecodeRecord(Interactive, 0, 2, true)
	if err != nil || len(interactive.Operations) != 4 || textOf(interactive) != "AB " || interactive.Operations[1]["op"] != "insert_master_name" {
		t.Fatalf("prefix/separator mismatch: %+v %v", interactive, err)
	}
	fixed, err := d.DecodeRecord(FixedComposer, 0, 2, true)
	if err != nil || len(fixed.Operations) != 2 || fixed.Operations[0]["op"] != "insert_master_name" {
		t.Fatalf("interactive prefix leaked into composer: %+v %v", fixed, err)
	}
	for _, invalid := range [][]byte{{6, 0}, {'A', 13, 0}, bytes.Repeat([]byte{'A'}, 32)} {
		copy(rom[prefix:], invalid)
		d, err := newDecoder(rom, profile)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := d.DecodeRecord(Interactive, 0, 2, true); err == nil {
			t.Fatalf("invalid name prefix silently exported: %x", invalid)
		}
	}
}

// ROM-free regression fixtures preserve independently decoded cases from the
// retired implementation. New discoveries and fixtures are maintained in Go.
type referenceMutation struct {
	ID       string `json:"id"`
	Offset   int    `json:"offset"`
	Data     []int  `json:"data"`
	Reject   bool   `json:"reject"`
	Expected any    `json:"expected"`
}

func mutatedDecoder(t *testing.T, d *Decoder, mutation referenceMutation) (*Decoder, error) {
	t.Helper()
	rom := append([]byte{}, d.rom...)
	if mutation.Offset < 0 || mutation.Offset > len(rom)-len(mutation.Data) {
		t.Fatal("invalid test mutation bounds")
	}
	for i, value := range mutation.Data {
		if value < 0 || value > 255 {
			t.Fatal("invalid test mutation byte")
		}
		rom[mutation.Offset+i] = byte(value)
	}
	// Bypass identity ONLY in tests to reach source/semantic proofs instead
	// of failing at the retail hash gate. No production API permits this.
	return newDecoder(rom, d.profile)
}

func TestNativeReaderRegressions(t *testing.T) {
	var groups []struct {
		ID               string              `json:"id"`
		ROM              []byte              `json:"rom"`
		Profile          decoderProfile      `json:"profile"`
		SourceExpected   any                 `json:"source_expected"`
		CatalogExpected  any                 `json:"catalog_expected"`
		SourceProfile    *sourceProfile      `json:"source_profile"`
		CatalogProfile   *catalogProfile     `json:"catalog_profile"`
		SourceMutations  []referenceMutation `json:"source_mutations"`
		CatalogMutations []referenceMutation `json:"catalog_mutations"`
		Cases            []struct {
			ID       string   `json:"id"`
			Consumer Consumer `json:"consumer"`
			Start    int      `json:"start"`
			Limit    int      `json:"limit"`
			Stop     bool     `json:"stop_on_yield"`
			Expected any      `json:"expected"`
		} `json:"cases"`
	}
	readRegressionFixture(t, "native-reader-regressions.json.gz", &groups)
	if len(groups) == 0 {
		t.Fatal("empty parity gate")
	}
	total := 0
	for _, group := range groups {
		d, err := newDecoder(group.ROM, group.Profile)
		if err != nil {
			t.Fatal(group.ID, err)
		}
		if len(group.Cases) == 0 && group.SourceExpected == nil && group.CatalogExpected == nil {
			t.Fatalf("%s has no vectors", group.ID)
		}
		if group.SourceExpected != nil {
			discover := func(decoder *Decoder) (*NativeSourceCensus, error) {
				if group.SourceProfile != nil {
					return decoder.discoverNativeSources(*group.SourceProfile)
				}
				return decoder.DiscoverNativeSources()
			}
			actual, err := discover(d)
			if err != nil {
				t.Fatalf("%s source discovery: %v", group.ID, err)
			}
			requireJSONEqual(t, group.ID+" source discovery", actual, group.SourceExpected)
			for _, mutation := range group.SourceMutations {
				changed, err := mutatedDecoder(t, d, mutation)
				var result *NativeSourceCensus
				if err == nil {
					result, err = discover(changed)
				}
				if mutation.Reject {
					if err == nil || result != nil {
						t.Fatalf("%s/%s: broken proof published a census", group.ID, mutation.ID)
					}
				} else {
					if err != nil {
						t.Fatalf("%s/%s: valid mutation rejected: %v", group.ID, mutation.ID, err)
					}
					requireJSONEqual(t, group.ID+"/"+mutation.ID, result, mutation.Expected)
				}
			}
			t.Logf("%s: native source census + %d mutations agree", group.ID, len(group.SourceMutations))
		}
		for _, test := range group.Cases {
			actual, err := d.DecodeRecord(test.Consumer, test.Start, test.Limit, test.Stop)
			if err != nil {
				t.Fatalf("%s/%s: %v", group.ID, test.ID, err)
			}
			encoded, err := json.Marshal(actual)
			if err != nil {
				t.Fatal(err)
			}
			var normalized any
			if err := json.Unmarshal(encoded, &normalized); err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(normalized, test.Expected) {
				want, _ := json.Marshal(test.Expected)
				t.Fatalf("%s/%s (%s $%06X..$%06X):\nactual: %s\nexpected: %s", group.ID, test.ID, test.Consumer, test.Start, test.Limit, encoded, want)
			}
			total++
		}
		if group.CatalogExpected != nil {
			build := func(decoder *Decoder) (*NativeCatalog, error) {
				if group.CatalogProfile != nil {
					if group.SourceProfile == nil {
						t.Fatal("synthetic catalogue requires source facts")
					}
					census, err := decoder.discoverNativeSources(*group.SourceProfile)
					if err != nil {
						return nil, err
					}
					return decoder.catalogFromSources(*group.CatalogProfile, *group.SourceProfile, census)
				}
				return decoder.BuildNativeCatalog()
			}
			catalog, err := build(d)
			if err != nil {
				t.Fatalf("%s catalogue: %v", group.ID, err)
			}
			requireJSONEqual(t, group.ID+" native catalogue", catalog, group.CatalogExpected)
			for _, mutation := range group.CatalogMutations {
				changed, err := mutatedDecoder(t, d, mutation)
				var actual *NativeCatalog
				if err == nil {
					actual, err = build(changed)
				}
				if mutation.Reject {
					if err == nil || actual != nil {
						t.Fatalf("%s/%s: invalid catalogue published: %v", group.ID, mutation.ID, err)
					}
				} else {
					if err != nil {
						t.Fatalf("%s/%s: valid catalogue rejected: %v", group.ID, mutation.ID, err)
					}
					requireJSONEqual(t, group.ID+"/"+mutation.ID, actual, mutation.Expected)
				}
			}
			t.Logf("%s: %d structured + %d composer records, %d routes agree", group.ID, len(catalog.Messages), len(catalog.Menu.Segments), catalog.SemanticRoutes.RouteCount)
			t.Logf("%s: %d catalogue mutations agree", group.ID, len(group.CatalogMutations))
		}
		if len(group.Cases) > 0 {
			t.Logf("%s: %d records agree", group.ID, len(group.Cases))
		}
	}
	t.Logf("%d independently decoded records agree", total)
}

func FuzzDecodeRecord(f *testing.F) {
	for _, value := range []string{"", "4100", "410742014200", "09ffff", "08aabbcc", "80ff00", "0bff00", "dede00"} {
		data, _ := hex.DecodeString(value)
		f.Add(data, true)
	}
	f.Fuzz(func(t *testing.T, data []byte, stop bool) {
		if len(data) > 4096 {
			return
		}
		for _, encoding := range []string{"direct-glyph", "dictionary-12"} {
			rom := data
			profile := decoderProfile{Encoding: encoding, Glyphs: map[byte]string{'A': "A"}}
			if encoding == "dictionary-12" {
				profile.Dictionary = 4096
				rom = make([]byte, 4096+dictionaryBytes)
				copy(rom, data)
				for i := 4096; len(data) > 0 && i < len(rom); i++ {
					rom[i] = data[(i-4096)%len(data)]
				}
			}
			d, err := newDecoder(rom, profile)
			if err != nil {
				t.Fatal(err)
			}
			for _, consumer := range []Consumer{Interactive, FixedComposer} {
				record, err := d.DecodeRecord(consumer, 0, len(data), stop)
				if err != nil || record.End < 0 || record.End > len(data) {
					t.Fatalf("bad bounds: %+v %v", record, err)
				}
				if _, err := json.Marshal(record); err != nil {
					t.Fatal(err)
				}
			}
		}
	})
}

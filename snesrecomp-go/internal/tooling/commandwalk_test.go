package tooling

import (
	"bytes"
	"encoding/json"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func commandWalkFixture(t *testing.T, cfg *config.Config) (romimage.Image, []shadowDecodeResult) {
	t.Helper()
	image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	fetch := results[2].commandStreams[0].FetchPC
	copy(image[0x900:], []byte{0xa6, 0x70, 0xb9, 0, 0, 0x95, 0x38, 0xc8, 0xc8, 0x4c, byte(fetch), byte(fetch >> 8)})
	copy(image[0x910:], []byte{0xb9, 0, 0, 0x85, 0x72, 0x6c, 0x72, 0})
	copy(image[0xa00:], []byte{0xb5, 0x38, 0x85, 0x72, 0x6c, 0x72, 0})
	copy(image[0x1201f:], []byte{0, 0x80, 0, 0x92, 0, 0x81, 0, 0x93, 0, 0x80, 0, 0x94})
	results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: 0x8a00}})
	for n, r := range results {
		g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		s := collectShadowCommandStreams(g)
		results[n] = shadowDecodeResult{entry: r.entry, commandStreams: s, commandRoots: collectShadowCommandRoots(g, s), streamInputs: collectShadowStreamInputs(g),
			commandPaths: collectShadowCommandPaths(g, cfg, s), forwardedFields: decoder.ForwardedIndirectFields(g)}
	}
	return image, results
}

func TestCommandWalkRootedOperandsAndRefetch(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	roots := resolveShadowCommandRoots(image, results)
	streams := mergeShadowCommandStreams(results)
	walks := walkShadowCommandStreams(image, streams, roots)
	if len(walks) != 1 {
		t.Fatalf("walks %+v", walks)
	}
	w := walks[0]
	if w.FirstFetchPC != 0x02a01f || w.StopReason != "native_refetch_unproven" || len(w.Steps) != 2 || len(w.Origins) != 1 {
		t.Fatalf("walk %+v", w)
	}
	for n, step := range w.Steps {
		if len(step.Operands) != 1 || len(step.Operands[0].Candidates) != 1 {
			t.Fatalf("step %+v", step)
		}
		p := step.Operands[0]
		if p.SourcePC != 0x02a021+uint32(n)*4 || p.Value != 0x9200+uint16(n)*0x100 || p.Candidates[0].PC != 0x9200+uint32(n)*0x100 {
			t.Fatalf("mixed stream DB and callback PB: %+v", p)
		}
	}
	if w.Steps[0].NextPC == nil || *w.Steps[0].NextPC != 0x02a023 || w.Steps[1].NextPC != nil {
		t.Fatal("invented callback return cursor")
	}
	// Reverse workers, keep complete provenance and never mutate inputs.
	before, _ := json.Marshal(streams)
	slices.Reverse(results)
	if !reflect.DeepEqual(walks, walkShadowCommandStreams(image, mergeShadowCommandStreams(results), resolveShadowCommandRoots(image, results))) {
		t.Fatal("worker order changed evidence")
	}
	after, _ := json.Marshal(streams)
	if !bytes.Equal(before, after) {
		t.Fatal("walk mutated metadata")
	}
	report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandWalks: walks}
	encoded, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	var decoded ShadowReport
	if err := json.Unmarshal(encoded, &decoded); err != nil || !reflect.DeepEqual(walks, decoded.CommandWalks) {
		t.Fatal("roundtrip", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, report, "text", true); err != nil || !strings.Contains(text.String(), "[COMMAND-WALK]") || !strings.Contains(text.String(), "$00:9200") {
		t.Fatal(text.String(), err)
	}
	facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
	if len(facts) != 0 || len(SelectStaticProvenRoutineEntryFacts(report)) != 0 {
		t.Fatal("conditional ROM words promoted into generation proofs")
	}
	if got := walkShadowCommandStreams(image, streams, nil); len(got) != 0 {
		t.Fatal("unrooted data scanned")
	}
}

func TestCommandWalkStopsBeforeGuessing(t *testing.T) {
	for _, tt := range []struct {
		name, reason string
		mutate       func([]ShadowCommandStream, []ShadowCommandRoot, romimage.Image)
	}{
		{"untagged", "untagged_data_path_unmodeled", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) { i[0x12020] = 0x01 }},
		{"missing command", "missing_or_ambiguous_native_command", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands = s[0].Commands[1:]
		}},
		{"conflicting command", "missing_or_ambiguous_native_command", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands = append(s[0].Commands, s[0].Commands[0])
		}},
		{"unknown bank", "operand_bank_or_ROM_unknown", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands[0].Deferred.StreamBankSource = "unknown"
		}},
		{"unmapped operand bank", "operand_bank_or_ROM_unknown", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			b := byte(0x7e)
			s[0].Commands[0].Deferred.StreamBankSource = "constant"
			s[0].Commands[0].Deferred.StreamBank = &b
		}},
		{"operand bank overflow", "operand_bank_or_ROM_unknown", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands[0].Deferred.OperandOffset = 0x10000
		}},
		{"unproven continuation", "native_refetch_unproven", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands[0].Advances = nil
		}},
		{"conflicting continuation", "native_refetch_unproven", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands[0].Advances = append(s[0].Commands[0].Advances, ShadowCommandAdvance{Status: "linear_native_refetch", CursorDelta: 6})
		}},
		{"cycle", "stream_cursor_cycle", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands[0].Advances = []ShadowCommandAdvance{{Status: "linear_native_refetch", CursorDelta: -2}}
		}},
		{"next bank overflow", "next_cursor_bank_boundary", func(s []ShadowCommandStream, r []ShadowCommandRoot, i romimage.Image) {
			s[0].Commands[0].Advances = []ShadowCommandAdvance{{Status: "linear_native_refetch", CursorDelta: 0xffff}}
		}},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			s := mergeShadowCommandStreams(results)
			roots := resolveShadowCommandRoots(image, results)
			tt.mutate(s, roots, image)
			w := walkShadowCommandStreams(image, s, roots)
			if len(w) != 1 || w[0].StopReason != tt.reason || len(w[0].Steps) > 1 {
				t.Fatalf("walk %+v", w)
			}
		})
	}
}

func TestCommandWalkNativeOverrides(t *testing.T) {
	for _, cfg := range []*config.Config{
		{HLEFunctions: map[uint16]string{0x8900: "HostCommand"}},
		{HLEFunctions: map[uint16]string{0x8907: "HostContinuation"}},
		{HLESPCUpload: []uint16{0x8900}},
		{Entries: []config.Entry{{Start: 0x8900, EntrySOffset: 2}}},
	} {
		image, results := commandWalkFixture(t, cfg)
		w := walkShadowCommandStreams(image, mergeShadowCommandStreams(results), resolveShadowCommandRoots(image, results))
		if len(w) != 1 || len(w[0].Steps) != 0 || (w[0].StopReason != "native_HLE_boundary" && w[0].StopReason != "authored_body_override") {
			t.Fatalf("override bypassed: %+v", w)
		}
	}
}

func TestCommandWalkBudgetAndUnmappedTargets(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	for n := 0; n < shadowCommandWalkLimit+2; n++ {
		image[2*n+1] = 0x80
	}
	s := ShadowCommandStream{SelectorPC: 0x8900, EntryCursorDelta: 2, SelectorMask: 3, Commands: []ShadowCommandPrefix{{Index: 0, EntryPC: 0x9000, Advances: []ShadowCommandAdvance{{Status: "linear_native_refetch"}}}}}
	w := walkShadowCommandStream(image, s, 0x8000, 0)
	if len(w.Steps) != shadowCommandWalkLimit || w.StopReason != "stream_command_budget" {
		t.Fatalf("budget %+v", w)
	}
	image, results := commandWalkFixture(t, nil)
	image[0x12021], image[0x12022] = 0, 0x12
	w = walkShadowCommandStreams(image, mergeShadowCommandStreams(results), resolveShadowCommandRoots(image, results))[0]
	if p := w.Steps[0].Operands[0]; p.Value != 0x1200 || len(p.Candidates) != 0 {
		t.Fatalf("unmapped word became code %+v", p)
	}
}

func TestCommandAdvanceRejectsEffectsAndRetainsOwnedEdges(t *testing.T) {
	for _, tt := range []struct {
		name   string
		code   []byte
		reason string
	}{
		{"linear", []byte{0xa6, 0x70, 0xb9, 0, 0, 0x95, 0x38, 0xc8, 0xc8, 0x4c, 0, 0x80}, "linear_native_refetch"},
		{"cursor clobber", []byte{0xa0, 0, 0x92, 0x4c, 0, 0x80}, "unsupported_LDY"},
		{"bank clobber", []byte{0xab, 0x4c, 0, 0x80}, "unsupported_PLB"},
		{"stack change", []byte{0x5a, 0x4c, 0, 0x80}, "unsupported_PHY"},
		{"native call", []byte{0x20, 0, 0x89, 0x4c, 0, 0x80}, "unsupported_JSR"},
		{"branch", []byte{0xd0, 1, 0xea, 0x4c, 0, 0x80}, "unsupported_BNE"},
		{"status change", []byte{0xe2, 0x20, 0x4c, 0, 0x80}, "unsupported_SEP"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			g, s := commandStreamFixture(t, 0, map[uint16][]byte{0x8800: tt.code})
			paths := collectShadowCommandPaths(g, nil, s)
			for _, p := range paths {
				if p.entry.PC == 0x8800 {
					a := summarizeShadowCommandAdvance(p, 0x8000)
					if a.Status != tt.reason {
						t.Fatalf("advance %+v", a)
					}
					return
				}
			}
			t.Fatal("missing command path")
		})
	}
	// Empty native history must not be manufactured by a missing sibling.
	p := shadowCommandPath{entry: decoder.DecodeKey{PC: 0x8000}, stopPC: 0x8100, reason: "outside_decoded_graph", context: analysis.EntryVariant{PC: 0x8000}}
	if s := summarizeShadowCommandAdvance(p, 0x8200); s.Status == "linear_native_refetch" {
		t.Fatal("invented missing owned tail")
	}
}

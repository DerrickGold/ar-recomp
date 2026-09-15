package tooling

import (
	"bytes"
	"encoding/json"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// Original fixture: tagged commands interspersed with ordinary data records.
// An unknown object value chooses refetch versus cursor publication/return.
func commandDataFixture(t *testing.T, cfg *config.Config) (romimage.Image, *decoder.Graph, []shadowDecodeResult, []ShadowCommandRoot) {
	t.Helper()
	image := make(romimage.Image, 3*0x8000)
	copy(image, []byte{
		0xb9, 0, 0, 0x30, 0x2b, // fetch; BMI selector
		0xc8, 0xc8, 0x29, 0, 0xff, 0x18, 0x75, 0x30, 0x95, 0x30, 0x10, 5,
		0xc8, 0xc8, 0x4c, 0, 0x80, // refetch with Y+4
		0xb9, 0, 0, 0x95, 0x34, 0xc8, 0xc8, 0x98, 0x95, 0x36, 0xab, 0x6b, // publish Y+4, then PLB/RTL
	})
	copy(image[0x30:], []byte{0xc8, 0xc8, 0xeb, 0x29, 3, 0, 0x0a, 0xaa, 0x7c, 0, 0x84})
	copy(image[0x400:], []byte{0, 0x88, 0x40, 0x88, 0x80, 0x88, 0xc0, 0x88})
	copy(image[0x800:], []byte{0xb9, 0, 0, 0x95, 0x38, 0xc8, 0xc8, 0x4c, 0, 0x80})
	copy(image[0x840:], []byte{0xb9, 0, 0, 0x85, 0x72, 0x6c, 0x72, 0})
	image[0x880], image[0x8c0] = 0x60, 0x60
	copy(image[0x900:], []byte{0xb5, 0x38, 0x85, 0x72, 0x6c, 0x72, 0})
	copy(image[0x12000:], []byte{0, 1, 0x12, 0x34, 0, 0x80, 0x10, 0x92, 0, 0x81, 0x10, 0x93})
	var results []shadowDecodeResult
	var engine *decoder.Graph
	for _, pc := range []uint16{0x8000, 0x8900} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		if pc == 0x8000 {
			engine = g
		}
		s := collectShadowCommandStreams(g)
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: uint32(pc)}, commandStreams: s,
			commandPaths: collectShadowCommandPaths(g, cfg, s), commandDataPaths: collectShadowCommandDataPaths(g, cfg, s), forwardedFields: decoder.ForwardedIndirectFields(g)})
	}
	if len(results[0].commandStreams) != 1 {
		t.Fatal("fixture selector missing")
	}
	s := results[0].commandStreams[0]
	first := uint32(0x02a000)
	roots := []ShadowCommandRoot{{SelectorPC: s.SelectorPC, FetchPC: s.FetchPC, References: []ShadowCommandRootReference{{FirstFetchPC: &first, StreamPC: &first, Status: "literal_call_path_ROM_stream_reference"}}}}
	return image, engine, results, roots
}

func TestCommandDataBranchesAndPublications(t *testing.T) {
	image, g, results, roots := commandDataFixture(t, nil)
	s := mergeShadowCommandStreams(results)
	if len(s) != 1 || len(s[0].DataPaths) != 2 {
		t.Fatalf("data paths %+v", s)
	}
	var refetch, publish *ShadowCommandDataPath
	for i := range s[0].DataPaths {
		p := &s[0].DataPaths[i]
		if len(p.Branches) != 2 || p.Branches[0].Taken || p.Branches[0].Mnemonic != "BMI" || p.CursorDelta != 4 {
			t.Fatalf("path %+v", p)
		}
		if p.Status == "linear_native_refetch" {
			refetch = p
		} else {
			publish = p
		}
	}
	if refetch == nil || publish == nil || refetch.Branches[1].Taken || !publish.Branches[1].Taken || publish.Status != "unsupported_PLB" {
		t.Fatalf("wrong native branches %+v", s[0].DataPaths)
	}
	if len(publish.Publications) != 1 || publish.Publications[0].Operand != 0x36 || publish.Publications[0].CursorDelta != 4 {
		t.Fatalf("publication %+v", publish.Publications)
	}
	walks := walkShadowCommandStreams(image, s, roots)
	if len(walks) != 2 {
		t.Fatalf("walks %+v", walks)
	}
	for _, w := range walks {
		if w.Steps[0].Kind != "untagged_data" || w.Steps[0].DataPath == nil {
			t.Fatal("lost native data provenance")
		}
		if w.StopReason == "native_data_unsupported_PLB" {
			if len(w.Steps) != 1 || w.Steps[0].NextPC != nil {
				t.Fatal("cursor publication invented later resumption")
			}
		} else {
			if len(w.Steps) != 3 || w.StopReason != "native_refetch_unproven" || *w.Steps[0].NextPC != 0x02a004 {
				t.Fatalf("refetch %+v", w)
			}
			if p := w.Steps[1].Operands[0]; p.SourcePC != 0x02a006 || p.Value != 0x9210 || p.Candidates[0].PC != 0x9210 {
				t.Fatalf("lost deferred word %+v", p)
			}
		}
	}
	// The immutable decoder instructions/edges are queried, never rewritten.
	before := graphCommandDataSnapshot(t, g)
	collectShadowCommandDataPaths(g, nil, results[0].commandStreams)
	if !bytes.Equal(before, graphCommandDataSnapshot(t, g)) {
		t.Fatal("mutated graph")
	}
	slices.Reverse(results)
	if !reflect.DeepEqual(walks, walkShadowCommandStreams(image, mergeShadowCommandStreams(results), roots)) {
		t.Fatal("worker order changed branches")
	}
	report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreams: s, CommandWalks: walks}
	encoded, _ := json.Marshal(report)
	var decoded ShadowReport
	if err := json.Unmarshal(encoded, &decoded); err != nil || !reflect.DeepEqual(walks, decoded.CommandWalks) {
		t.Fatal("JSON provenance lost", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, report, "text", true); err != nil || !strings.Contains(text.String(), "required-branches=") || !strings.Contains(text.String(), "cursor-publications=1") {
		t.Fatal(text.String(), err)
	}
	facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
	if len(facts) != 0 || len(SelectStaticProvenRoutineEntryFacts(report)) != 0 {
		t.Fatal("conditional data path promoted into code proof")
	}
	starts, raw, unique, targets := shadowCommandWalkCounts(append(append([]ShadowCommandWalk(nil), walks...), walks...))
	if starts != 1 || raw != 4 || unique != 2 || targets != 2 {
		t.Fatalf("repeated paths inflated source counts: %d starts, %d raw, %d unique, %d targets", starts, raw, unique, targets)
	}
}

func graphCommandDataSnapshot(t *testing.T, g *decoder.Graph) []byte {
	t.Helper()
	var instructions []*decoder.DecodedInstruction
	for _, k := range g.Order {
		instructions = append(instructions, g.Instructions[k])
	}
	b, err := json.Marshal(instructions)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestCommandDataNativeBarriers(t *testing.T) {
	for _, pc := range []uint16{0x8000, 0x8003, 0x8005, 0x8013} {
		image, _, r, roots := commandDataFixture(t, &config.Config{HLEFunctions: map[uint16]string{pc: "Host"}})
		for _, w := range walkShadowCommandStreams(image, mergeShadowCommandStreams(r), roots) {
			if len(w.Steps) > 1 {
				t.Fatalf("HLE bypassed at %04x: %+v", pc, w)
			}
		}
	}
	for _, tt := range []struct {
		name             string
		pc               uint32
		mnemonic, reason string
	}{
		{"cursor clobber", 0x8007, "LDY", "unsupported_LDY"},
		{"call", 0x8007, "JSR", "unsupported_JSR"},
		{"width", 0x8007, "SEP", "unsupported_SEP"},
		{"bank", 0x8007, "PLB", "unsupported_PLB"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			_, g, r, _ := commandDataFixture(t, nil)
			g.Instructions[decoder.DecodeKey{PC: tt.pc}].Instruction.Mnemonic = tt.mnemonic
			p := collectShadowCommandDataPaths(g, nil, r[0].commandStreams)
			if len(p) != 1 || p[0].Status != tt.reason {
				t.Fatalf("barrier %+v", p)
			}
		})
	}
}

func TestCommandDataAndWalkBudgets(t *testing.T) {
	image, _, r, roots := commandDataFixture(t, nil)
	s := mergeShadowCommandStreams(r)
	var refetch ShadowCommandDataPath
	for _, p := range s[0].DataPaths {
		if p.Status == "linear_native_refetch" {
			refetch = p
		}
	}
	refetch.CursorDelta = 0
	s[0].DataPaths = []ShadowCommandDataPath{refetch}
	w := walkShadowCommandStreams(image, s, roots)
	if len(w) != 1 || w[0].StopReason != "stream_cursor_cycle" || len(w[0].Steps) != 1 {
		t.Fatalf("cross-segment cycle %+v", w)
	}
	refetch.CursorDelta = 4
	s[0].DataPaths = []ShadowCommandDataPath{refetch}
	// A whole run of untagged bytes does not become an unbounded data scan.
	clear(image[0x12000:0x12400])
	w = walkShadowCommandStreams(image, s, roots)
	if len(w) != 1 || w[0].StopReason != "stream_command_budget" || len(w[0].Steps) != shadowCommandWalkLimit {
		t.Fatalf("record budget %+v", w)
	}
	// Repeated refetch/publication forks retain explicit incompleteness.
	s = mergeShadowCommandStreams(r)
	w = walkShadowCommandStreams(image, s, roots)
	if len(w) != shadowCommandWalkPathLimit {
		t.Fatalf("path budget count=%d", len(w))
	}
	found := false
	for _, p := range w {
		found = found || p.StopReason == "stream_path_budget"
	}
	if !found {
		t.Fatal("path budget did not report unexplored alternatives")
	}
}

func TestCommandDataUnavailableAndChangedEdges(t *testing.T) {
	_, g, r, _ := commandDataFixture(t, nil)
	delete(g.Instructions, decoder.DecodeKey{PC: 0x800b})
	p := collectShadowCommandDataPaths(g, nil, r[0].commandStreams)
	if len(p) != 1 || p[0].Status != "outside_decoded_graph" {
		t.Fatalf("decoded through missing boundary %+v", p)
	}
	_, g, r, _ = commandDataFixture(t, nil)
	g.Instructions[decoder.DecodeKey{PC: 0x800f}].Successors = g.Instructions[decoder.DecodeKey{PC: 0x800f}].Successors[:1]
	p = collectShadowCommandDataPaths(g, nil, r[0].commandStreams)
	if len(p) != 1 || p[0].Status != "ambiguous_native_branch" {
		t.Fatal("pruned arm assumed complete")
	}
}

func TestCommandDataNativeBudgetsAndPublicationValues(t *testing.T) {
	for _, tt := range []struct {
		name   string
		body   []byte
		reason string
	}{
		{"instruction budget", append(bytes.Repeat([]byte{0xea}, 70), 0x4c, 0, 0x80), "native_data_instruction_budget"},
		{"branch budget", append(bytes.Repeat([]byte{0xd0, 1, 0xea}, 10), 0x4c, 0, 0x80), "native_data_path_budget"},
		{"native cycle", []byte{0x80, 0xfe}, "native_data_cycle"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, _, _, _ := commandDataFixture(t, nil)
			copy(image[5:], []byte{0x4c, 0, 0x85})
			copy(image[0x500:], tt.body)
			g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			paths := collectShadowCommandDataPaths(g, nil, collectShadowCommandStreams(g))
			if len(paths) == 0 || len(paths) > shadowCommandDataPathLimit {
				t.Fatalf("path bound %d", len(paths))
			}
			for _, p := range paths {
				if p.Status != tt.reason {
					t.Fatalf("unexpected budget result %+v", p)
				}
			}
		})
	}
	for _, tt := range []struct {
		name         string
		body         []byte
		delta, count int
	}{
		{"saved A remains old Y", []byte{0x98, 0xc8, 0x95, 0x36, 0xab, 0x6b}, 4, 1},
		{"clobbered A is not Y", []byte{0x98, 0xa9, 1, 0, 0x95, 0x36, 0xab, 0x6b}, 0, 0},
		{"STY publishes current Y", []byte{0xc8, 0x94, 0x36, 0xab, 0x6b}, 5, 1},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, _, _, _ := commandDataFixture(t, nil)
			copy(image[0x1d:], tt.body)
			g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			paths := collectShadowCommandDataPaths(g, nil, collectShadowCommandStreams(g))
			found := false
			for _, p := range paths {
				if p.Status == "unsupported_PLB" {
					found = true
					if len(p.Publications) != tt.count || tt.count != 0 && p.Publications[0].CursorDelta != tt.delta {
						t.Fatalf("wrong publication %+v", p)
					}
				}
			}
			if !found {
				t.Fatalf("publication path not retained: %+v", paths)
			}
		})
	}
}

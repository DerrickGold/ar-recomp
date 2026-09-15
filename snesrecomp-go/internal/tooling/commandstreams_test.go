package tooling

import (
	"bytes"
	"encoding/json"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// Original synthetic code: word's high byte tags one of four native commands.
// These are command-prefix contracts, not a ROM script or a known game's data.
func commandStreamFixture(t *testing.T, bank byte, bodies map[uint16][]byte) (*decoder.Graph, []ShadowCommandStream) {
	t.Helper()
	image := make(romimage.Image, (int(bank)+1)*0x8000)
	put := func(pc uint16, code []byte) { copy(image[int(bank)*0x8000+int(pc)-0x8000:], code) }
	put(0x8000, []byte{0xb9, 0, 0, 0x30, 1, 0x60, 0xc8, 0xc8, 0xeb, 0x29, 3, 0, 0x0a, 0xaa, 0x7c, 0, 0x84})
	put(0x8400, []byte{0, 0x88, 0x40, 0x88, 0x80, 0x88, 0xc0, 0x88})
	for _, pc := range []uint16{0x8800, 0x8840, 0x8880, 0x88c0} {
		put(pc, []byte{0xea, 0x60})
	}
	for pc, code := range bodies {
		put(pc, code)
	}
	graph, err := decoder.DecodeFunction(image, bank, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	return graph, collectShadowCommandStreams(graph)
}

func TestShadowCommandOperandPrefixes(t *testing.T) {
	for _, bank := range []byte{0, 2} {
		g, streams := commandStreamFixture(t, bank, map[uint16][]byte{
			0x8800: {0xa6, 0x70, 0xb9, 0, 0, 0x85, 0x42, 0xc8, 0xc8, 0x8b, 0x4b, 0xab, 0x5a, 0x6c, 0x42, 0},
			0x8840: {0xb9, 0, 0, 0x85, 0x52, 0xc8, 0xc8, 0x5a, 0x8b, 0x4b, 0xab, 0xf4, 0x61, 0x88, 0x6c, 0x52, 0},
			0x8880: {0xf4, 1, 0x82, 0xab, 0xab, 0xb9, 3, 0, 0x85, 0x62, 0xc8, 0xc8, 0xb9, 0, 0, 0xc8, 0xc8, 0x94, 0x76, 0x5a, 0x8b, 0x4b, 0xab, 0xf4, 0xa0, 0x88, 0x6c, 0x62, 0},
		})
		if len(streams) != 1 || len(streams[0].Commands) != 4 {
			t.Fatalf("streams=%+v", streams)
		}
		s := streams[0]
		base := uint32(bank) << 16
		if s.SelectorPC != base|0x800e || s.FetchPC != base|0x8000 || s.SignTestPC != base|0x8003 || s.TablePC != base|0x8400 || s.SelectorMask != 3 || s.TagByteOffset != 1 || s.EntryCursorDelta != 2 {
			t.Fatalf("selector=%+v", s)
		}
		for n, c := range s.Commands[:3] {
			p := c.Callback
			if c.StopReason != "callback_operand_dispatch" || p == nil || p.Width != 2 || p.DecodedProgramBank != bank || *p.RequiredD != 0 {
				t.Fatalf("command%d=%+v", n, c)
			}
			if n < 2 && (p.StreamBankSource != "command_entry_db" || p.StreamBank != nil || p.OperandOffset != 0 || c.CursorDelta != 2) {
				t.Fatalf("invented stream bank/cursor: %+v", p)
			}
		}
		if s.Commands[0].Callback.PushedWord != nil {
			t.Fatal("invented a return continuation for shared cleanup")
		}
		if p := s.Commands[1].Callback; p.PushedWord == nil || *p.PushedWord != 0x8861 || p.PushedWordPC != base|0x884b {
			t.Fatalf("lost literal stack word: %+v", p)
		}
		if c := s.Commands[2]; c.CursorDelta != 4 || len(c.CursorStores) != 1 || c.Callback.OperandOffset != 3 || c.Callback.StreamBank == nil || *c.Callback.StreamBank != 0x82 {
			t.Fatalf("lost bank/operand/store evidence: %+v %+v", c, c.Callback)
		}
		if s.Commands[3].Callback != nil {
			t.Fatal("ordinary return became a callback")
		}
		// Neither extraction nor serialization may modify decoder metadata.
		snapshot := func() []byte {
			var ins []*decoder.DecodedInstruction
			for _, key := range g.Order {
				ins = append(ins, g.Instructions[key])
			}
			encoded, err := json.Marshal(ins)
			if err != nil {
				t.Fatal(err)
			}
			return encoded
		}
		before := snapshot()
		collectShadowCommandStreams(g)
		after := snapshot()
		if !bytes.Equal(before, after) {
			t.Fatal("report mutated graph")
		}
	}
}

func TestShadowCommandPrefixBarriers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		body   []byte
		reason string
	}{
		{"changed cursor", []byte{0xa0, 0, 0x90}, "unsupported_LDY"},
		{"unknown call", []byte{0x20, 0, 0x89}, "unsupported_JSR"},
		{"arithmetic", []byte{0x69, 1, 0}, "unsupported_ADC"},
		{"scratch overwrite", []byte{0x85, 0x42, 0x64, 0x42}, "unsupported_STZ"},
		{"unknown memory write", []byte{0x85, 0x42, 0x99, 0, 0x10}, "unmodeled_memory_write"},
		{"width change", []byte{0xe2, 0x20}, "unsupported_SEP"},
		{"ambiguous path", []byte{0xd0, 1, 0xea}, "unsupported_BNE"},
		{"cursor restore", []byte{0x7a}, "unsupported_PLY"},
		{"cycle", []byte{0x80, 0xfe}, "prefix_cycle"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			code := append([]byte{0xb9, 0, 0}, tt.body...)
			code = append(code, 0x85, 0x42, 0x6c, 0x42, 0)
			_, streams := commandStreamFixture(t, 0, map[uint16][]byte{0x8800: code, 0x8900: {0x60}})
			if len(streams) != 1 {
				t.Fatalf("missing selector: %+v", streams)
			}
			c := streams[0].Commands[0]
			if c.Callback != nil || c.StopReason != tt.reason {
				t.Fatalf("crossed barrier: %+v", c)
			}
		})
	}
}

func TestShadowCommandSelectorRequiresOwnedEdges(t *testing.T) {
	for _, mutate := range []func(*decoder.Graph){
		func(g *decoder.Graph) { g.Instructions[decoder.DecodeKey{PC: 0x8003}].Instruction.Mnemonic = "BPL" },
		func(g *decoder.Graph) { g.Instructions[decoder.DecodeKey{PC: 0x8009}].Instruction.Operand = 0xffff },
		func(g *decoder.Graph) { g.Instructions[decoder.DecodeKey{PC: 0x8000}].Instruction.Mnemonic = "LDX" },
		func(g *decoder.Graph) {
			g.Instructions[decoder.DecodeKey{PC: 0x8003}].Successors = []decoder.DecodeKey{{PC: 0x8005}}
		},
		func(g *decoder.Graph) {
			g.Instructions[decoder.DecodeKey{PC: 0x800e}].Instruction.DispatchEntries = nil
		},
	} {
		g, _ := commandStreamFixture(t, 0, nil)
		mutate(g)
		if got := collectShadowCommandStreams(g); len(got) != 0 {
			t.Fatalf("invented selector: %+v", got)
		}
	}
	g, _ := commandStreamFixture(t, 0, nil)
	// A configured data/HLE/sibling boundary is not permission to decode ahead.
	delete(g.Instructions, decoder.DecodeKey{PC: 0x8800})
	if c := collectShadowCommandStreams(g)[0].Commands[0]; c.Callback != nil || c.StopReason != "outside_decoded_graph" {
		t.Fatalf("crossed ownership boundary: %+v", c)
	}
}

func TestShadowCommandReportDeterministicAndNotFacts(t *testing.T) {
	_, streams := commandStreamFixture(t, 0, map[uint16][]byte{0x8800: {0xb9, 0, 0, 0x85, 0x42, 0x6c, 0x42, 0}})
	other := streams[0]
	other.Contexts = []analysis.EntryVariant{{PC: 0x8123, EntryMX: analysis.MXState{M: 1, X: 0}}}
	a := shadowDecodeResult{commandStreams: streams}
	b := shadowDecodeResult{commandStreams: []ShadowCommandStream{other}}
	got := mergeShadowCommandStreams([]shadowDecodeResult{a, b, a})
	if len(got) != 1 || len(got[0].Contexts) != 2 || !reflect.DeepEqual(got, mergeShadowCommandStreams([]shadowDecodeResult{b, a})) {
		t.Fatalf("unstable/doubled report: %+v", got)
	}
	report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreams: got}
	var out bytes.Buffer
	if err := WriteShadowReport(&out, report, "json", true); err != nil {
		t.Fatal(err)
	}
	facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
	if !strings.Contains(out.String(), `"command_streams"`) || !strings.Contains(out.String(), "independently_rooted_streams") || len(facts) != 0 {
		t.Fatal("report missing obligations or promoted prefixes into facts")
	}
	out.Reset()
	if err := WriteShadowReport(&out, report, "text", true); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "[COMMAND-STREAM]") || !strings.Contains(out.String(), "return contract") && !strings.Contains(out.String(), "stream roots unproven") {
		t.Fatal(out.String())
	}
}

func TestShadowCommandSiblingOwnershipAndSharedHandler(t *testing.T) {
	g, _ := commandStreamFixture(t, 0, map[uint16][]byte{0x8800: {0xb9, 0, 0, 0x85, 0x42, 0xc8, 0xc8, 0x6c, 0x42, 0}})
	entry := decoder.DecodeKey{PC: 0x8800}
	body := &decoder.Graph{Entry: entry, Instructions: make(map[decoder.DecodeKey]*decoder.DecodedInstruction)}
	for _, k := range g.Order {
		if k.PC >= 0x8800 && k.PC < 0x8840 {
			body.Order = append(body.Order, k)
			body.Instructions[k] = g.Instructions[k]
			delete(g.Instructions, k)
		}
	}
	// Two command indices intentionally share a native handler.
	g.Instructions[decoder.DecodeKey{PC: 0x800e}].Instruction.DispatchEntries[1] = 0x8800
	selector := shadowDecodeResult{commandStreams: collectShadowCommandStreams(g)}
	sibling := shadowDecodeResult{entry: decoder.Variant{Address: 0x8800}, commandPrefix: summarizeShadowCommandPrefix(body, entry, 0)}
	got := mergeShadowCommandStreams([]shadowDecodeResult{selector, sibling})
	if len(got) != 1 || len(got[0].Commands) != 4 {
		t.Fatalf("lost index/selector: %+v", got)
	}
	for _, c := range got[0].Commands[:2] {
		if c.Callback == nil || len(c.Contexts) != 1 || c.Contexts[0].PC != 0x8800 {
			t.Fatalf("failed exact sibling join: %+v", c)
		}
	}
	sibling.entry.X = 1
	if got := mergeShadowCommandStreams([]shadowDecodeResult{selector, sibling}); got[0].Commands[0].Callback != nil {
		t.Fatal("joined wrong-width sibling")
	}
	if got := mergeShadowCommandStreams([]shadowDecodeResult{selector}); got[0].Commands[0].Callback != nil {
		t.Fatal("invented body beyond ownership boundary")
	}
}

func TestShadowCommandBudgetAndStackWordProvenance(t *testing.T) {
	g, _ := commandStreamFixture(t, 0, map[uint16][]byte{0x8800: bytes.Repeat([]byte{0xea}, shadowCommandPrefixLimit+2)})
	c := summarizeShadowCommandPrefix(g, decoder.DecodeKey{PC: 0x8800}, 0)
	if c.StopReason != "prefix_budget_exhausted" || c.Callback != nil {
		t.Fatalf("unbounded prefix: %+v", c)
	}
	_, streams := commandStreamFixture(t, 0, map[uint16][]byte{
		0x8800: {0xf4, 0x82, 0x82, 0xab, 0xab, 0xb9, 0, 0, 0x85, 0x42, 0x8b, 0x8b, 0x6c, 0x42, 0},
	})
	p := streams[0].Commands[0].Callback
	if p == nil || p.StreamBank == nil || *p.StreamBank != 0x82 || p.PushedWord != nil {
		t.Fatalf("re-pushed bank bytes became a PEA continuation: %+v", p)
	}
}

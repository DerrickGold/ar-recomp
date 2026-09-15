package tooling

import (
	"bytes"
	"encoding/json"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func commandRootFixture(t *testing.T, caller, wrapper []byte, longTable bool) (romimage.Image, []shadowDecodeResult) {
	t.Helper()
	image := make(romimage.Image, 4*0x8000)
	put := func(bank byte, pc uint16, b []byte) { copy(image[int(bank)*0x8000+int(pc)-0x8000:], b) }
	engine := []byte{0xf4, 2, 2, 0xab, 0xab, 0x0a, 0x0a, 0xa8, 0xb9, 0, 0x90}
	if longTable {
		engine = []byte{0xf4, 2, 2, 0xab, 0xab, 0x0a, 0x0a, 0xaa, 0xbf, 0, 0x90, 3}
	}
	engine = append(engine, 0x95, 0x46, 0xa8, 0xa9, 0, 1, 0x95, 0x44, 0x74, 0x42, 0x88,
		0xb9, 0, 0, 0x30, 1, 0x60, 0xc8, 0xc8, 0xeb, 0x29, 3, 0, 0x0a, 0xaa, 0x7c, 0, 0x88)
	put(0, 0x8000, caller)
	put(0, 0x8100, wrapper)
	put(0, 0x8200, engine)
	put(0, 0x8800, []byte{0, 0x89, 0x10, 0x89, 0x20, 0x89, 0x30, 0x89})
	for _, pc := range []uint16{0x8900, 0x8910, 0x8920, 0x8930} {
		put(0, pc, []byte{0xea, 0x60})
	}
	// Only index 12 contains a valid stream pointer. Unknown inputs must not
	// trigger a scan of neighboring records or reuse this value as a seed.
	for _, bank := range []byte{2, 3} {
		put(bank, 0x900c, []byte{0x20, 0xa0})
	}
	var results []shadowDecodeResult
	for _, pc := range []uint16{0x8000, 0x8100, 0x8200} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		s := collectShadowCommandStreams(g)
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: uint32(pc)}, commandStreams: s, commandRoots: collectShadowCommandRoots(g, s), streamInputs: collectShadowStreamInputs(g)})
	}
	return image, results
}

func TestCommandRootLiteralAcrossMirroredWrapper(t *testing.T) {
	for _, longTable := range []bool{false, true} {
		image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, longTable)
		got := resolveShadowCommandRoots(image, results)
		if len(got) != 1 {
			t.Fatalf("roots=%+v", got)
		}
		r := got[0]
		if len(r.References) != 1 || len(r.Unresolved) != 0 || r.Truncated {
			t.Fatalf("lost literal chain: %+v", r)
		}
		p := r.References[0]
		table := uint32(0x02900c)
		if longTable {
			table = 0x03900c
		}
		if p.Status != "literal_call_path_ROM_stream_reference" || p.LiteralValue == nil || *p.LiteralValue != 3 || p.TableIndex != 12 || p.TableReadPC != table || p.StreamPC == nil || *p.StreamPC != 0x02a020 || p.FirstFetchPC == nil || *p.FirstFetchPC != 0x02a01f || p.DefinitionPC != 0x8000 {
			t.Fatalf("reference=%+v", p)
		}
		if len(p.Calls) != 2 || p.Calls[0].Register != "X" || p.Calls[1].Register != "A" || p.Calls[1].TargetPC != 0x808200 {
			t.Fatalf("lost correlated argument path/mirrored target: %+v", p.Calls)
		}
		reverse := append([]shadowDecodeResult(nil), results...)
		slices.Reverse(reverse)
		if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, reverse)) {
			t.Fatal("worker order changes root evidence")
		}
	}
}

func TestCommandRootBankAtCursorUseAndNativeBarriers(t *testing.T) {
	for _, tt := range []struct {
		name          string
		before, after []byte
		status        string
	}{
		{"bank changes between load and cursor use", []byte{0xf4, 3, 3, 0xab, 0xab}, nil, "literal_call_path_ROM_stream_reference"},
		{"unknown restored bank", []byte{0xab}, nil, "unknown_stream_bank"},
		{"cursor clobber", nil, []byte{0xa0, 0, 0xa0}, "no_root"},
		{"unknown call", nil, []byte{0x20, 0, 0x89}, "no_root"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
			engine := []byte{0xf4, 2, 2, 0xab, 0xab, 0x0a, 0x0a, 0xa8, 0xb9, 0, 0x90}
			engine = append(engine, tt.before...)
			engine = append(engine, 0xa8)
			engine = append(engine, tt.after...)
			engine = append(engine, 0x88, 0xb9, 0, 0, 0x30, 1, 0x60, 0xc8, 0xc8, 0xeb, 0x29, 3, 0, 0x0a, 0xaa, 0x7c, 0, 0x88)
			copy(image[0x200:], engine)
			g, err := decoder.DecodeFunction(image, 0, 0x8200, 0, 0, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			s := collectShadowCommandStreams(g)
			results[2].commandRoots = collectShadowCommandRoots(g, s)
			got := resolveShadowCommandRoots(image, results)
			if tt.status == "no_root" {
				if len(got) != 0 {
					t.Fatalf("crossed native barrier: %+v", got)
				}
				return
			}
			if len(got) != 1 || len(got[0].References) != 1 || got[0].References[0].Status != tt.status {
				t.Fatalf("bank use: %+v", got)
			}
			if tt.status == "literal_call_path_ROM_stream_reference" && *got[0].References[0].StreamPC != 0x03a020 {
				t.Fatal("table-read bank leaked into stream-bank use")
			}
			if !reflect.DeepEqual(collectShadowStreamInputs(g), collectShadowStreamInputs(g, collectShadowDirectCallInputs(g))) {
				t.Fatal("cached argument reuse changed evidence")
			}
		})
	}
}

func TestCommandRootCyclesBudgetsAndExpressionTies(t *testing.T) {
	image, results := commandRootFixture(t, []byte{0x60}, []byte{0x60}, false)
	// Two separately owned native tail wrappers form the input cycle. Keep
	// their bodies separate; an intraprocedural backedge is instead a local
	// predecessor ambiguity and is already rejected by the expression query.
	copy(image[0x100:], []byte{0x4c, 0, 0x83})
	copy(image[0x300:], []byte{0xd0, 3, 0x4c, 0, 0x81, 0x4c, 0, 0x82})
	results = results[2:]
	for _, pc := range []uint16{0x8100, 0x8300} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{SiblingEntryPCs: map[uint16]struct{}{0x8100: {}, 0x8200: {}, 0x8300: {}}})
		if err != nil {
			t.Fatal(err)
		}
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: uint32(pc)}, streamInputs: collectShadowStreamInputs(g)})
	}
	got := resolveShadowCommandRoots(image, results)
	if len(got) != 1 || len(got[0].References) != 0 || !slices.ContainsFunc(got[0].Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == "recursive_input_cycle" }) {
		t.Fatalf("missed recursive input cycle: %+v", got)
	}
	image, results = commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	seed := results[0].streamInputs[0]
	results[0].streamInputs = nil
	for n := 0; n < shadowStreamInputBudget+8; n++ {
		v := seed
		// Same call identity, differing predecessor expressions: tie ordering
		// and budget truncation must remain deterministic.
		v.values[1].Source.Operand = uint32(n)
		results[0].streamInputs = append(results[0].streamInputs, v)
	}
	got = resolveShadowCommandRoots(image, results)
	if !got[0].Truncated || len(got[0].References) > shadowStreamReferenceLimit {
		t.Fatal("unbounded caller expansion")
	}
	slices.Reverse(results[0].streamInputs)
	if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
		t.Fatal("budget cutoff depends on equal-call expression ordering")
	}
}

func TestCommandRootUnprovenInputsAreNotSeeds(t *testing.T) {
	for _, tt := range []struct {
		name            string
		caller, wrapper []byte
	}{
		{"unknown scalar", []byte{0xa6, 0x20, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}},
		{"masked unknown is not a literal call", []byte{0xa5, 0x20, 0x29, 3, 0, 0xaa, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}},
		{"unknown callee clobber", []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x20, 0, 0x89, 0x22, 0, 0x82, 0x80, 0x60}},
		{"unsupported arithmetic", []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x69, 1, 0, 0x22, 0, 0x82, 0x80, 0x60}},
		{"recursive wrapper", []byte{0x60}, []byte{0x20, 0, 0x81, 0x22, 0, 0x82, 0x80, 0x60}},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := commandRootFixture(t, tt.caller, tt.wrapper, false)
			got := resolveShadowCommandRoots(image, results)
			if len(got) != 1 || len(got[0].References) != 0 || len(got[0].Unresolved) == 0 {
				t.Fatalf("unknown inputs became roots: %+v", got)
			}
		})
	}
	image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	results[1].streamInputs[0].mx.X = 1
	got := resolveShadowCommandRoots(image, results)
	if len(got[0].References) != 0 {
		t.Fatal("joined a wrong-width call")
	}
	got = resolveShadowCommandRoots(image, results[2:])
	if len(got[0].References) != 0 || got[0].Unresolved[0].Reason != "no_matching_decoded_direct_inputs" {
		t.Fatalf("no caller became a root: %+v", got)
	}
}

func TestCommandRootWrapperDepthIsBounded(t *testing.T) {
	image, results := commandRootFixture(t, []byte{0x60}, []byte{0x60}, false)
	results = results[2:]
	code := map[uint16][]byte{0x8000: {0xa9, 3, 0, 0x20, 0, 0x81, 0x60}, 0x8100: {0x4c, 0x80, 0x81}, 0x8180: {0x4c, 0xa0, 0x81}, 0x81a0: {0x4c, 0xc0, 0x81}, 0x81c0: {0x4c, 0, 0x82}}
	siblings := map[uint16]struct{}{0x8100: {}, 0x8180: {}, 0x81a0: {}, 0x81c0: {}, 0x8200: {}}
	for pc, b := range code {
		copy(image[int(pc)-0x8000:], b)
	}
	for _, pc := range []uint16{0x8000, 0x8100, 0x8180, 0x81a0, 0x81c0} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{SiblingEntryPCs: siblings})
		if err != nil {
			t.Fatal(err)
		}
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: uint32(pc)}, streamInputs: collectShadowStreamInputs(g)})
	}
	got := resolveShadowCommandRoots(image, results)
	if len(got) != 1 || !got[0].Truncated || len(got[0].References) != 0 || !slices.ContainsFunc(got[0].Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == "caller_depth_limit" }) {
		t.Fatalf("crossed wrapper budget: %+v", got)
	}
}

func TestCommandRootBankAndWrapBarriers(t *testing.T) {
	image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	base := resolveShadowCommandRoots(image, results)[0]
	for _, tt := range []struct {
		name   string
		edit   func(*ShadowCommandRoot)
		status string
	}{
		{"unknown table bank", func(r *ShadowCommandRoot) { r.TableRead.DataBank.UnknownPaths = true }, "unknown_pointer_table_bank"},
		{"unknown stream bank", func(r *ShadowCommandRoot) { r.StreamBank.UnknownPaths = true }, "unknown_stream_bank"},
		{"indexed carry", func(r *ShadowCommandRoot) { r.TableRead.Operand = 0xfff5 }, "pointer_table_bank_boundary"},
		{"WRAM table", func(r *ShadowCommandRoot) { r.TableRead.DataBank.Constants = []ShadowRegisterConstant{{Value: 0x7e}} }, "pointer_table_not_ROM_mapped"},
		{"WRAM stream", func(r *ShadowCommandRoot) { r.StreamBank.Constants = []ShadowRegisterConstant{{Value: 0x7e}} }, "stream_not_ROM_mapped"},
		{"cursor underflow", func(r *ShadowCommandRoot) { r.FetchCursorDelta = -0xffff }, "first_fetch_bank_boundary"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			r := base
			tt.edit(&r)
			ref := ShadowCommandRootReference{TableIndex: 12}
			resolveShadowStreamPointer(image, r, &ref)
			if ref.Status != tt.status || ref.FirstFetchPC != nil {
				t.Fatalf("crossed bank barrier: %+v", ref)
			}
		})
	}
}

func TestCommandRootHiROMFullBankStream(t *testing.T) {
	lo, _ := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	image := make(romimage.Image, 4*0x10000)
	copy(image[0x8000:0x10000], lo[:0x8000])
	copy(image[0xffc0:], []byte("SYNTHETIC STREAM ROM  "))
	image[0xffd5], image[0xffdc], image[0xffdd], image[0xfffd] = 0x31, 0xff, 0xff, 0x80
	image[0x8201], image[0x8202] = 0xc2, 0xc2 // data bank, independent of program-bank mirrors
	image[0x2900c], image[0x2900d] = 0x00, 0x40
	if image.Mapper() != romimage.HiROM {
		t.Fatal("fixture lost HiROM mapping")
	}
	var results []shadowDecodeResult
	for _, pc := range []uint16{0x8000, 0x8100, 0x8200} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		s := collectShadowCommandStreams(g)
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: uint32(pc)}, commandRoots: collectShadowCommandRoots(g, s), streamInputs: collectShadowStreamInputs(g)})
	}
	got := resolveShadowCommandRoots(image, results)
	if len(got) != 1 || len(got[0].References) != 1 {
		t.Fatalf("missing HiROM reference: %+v", got)
	}
	p := got[0].References[0]
	if p.Status != "literal_call_path_ROM_stream_reference" || p.StreamPC == nil || *p.StreamPC != 0xc24000 || p.FirstFetchPC == nil || *p.FirstFetchPC != 0xc23fff {
		t.Fatalf("LoROM assumptions leaked into stream query: %+v", p)
	}
}

func TestCommandRootDirectTailAndBoundaryValues(t *testing.T) {
	image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x5c, 0, 0x82, 0x80}, false)
	got := resolveShadowCommandRoots(image, results)
	if len(got[0].References) != 1 || got[0].References[0].Calls[1].Transfer != "JML" {
		t.Fatalf("lost tail argument: %+v", got)
	}
	// Literal assignment is the last instruction before a sibling boundary.
	copy(image[0x300:], []byte{0xa9, 3, 0, 0xea, 0x60})
	end := uint16(0x8303)
	g, err := decoder.DecodeFunction(image, 0, 0x8300, 0, 0, decoder.Options{End: &end})
	if err != nil {
		t.Fatal(err)
	}
	inputs := collectShadowStreamInputs(g)
	if len(inputs) != 1 || inputs[0].values[0].Source.Kind != "load" || inputs[0].values[0].Source.Operand != 3 {
		t.Fatalf("missed boundary definition: %+v", inputs)
	}
}

func TestCommandRootEvidenceCannotExportFunctions(t *testing.T) {
	image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	roots := resolveShadowCommandRoots(image, results)
	report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreamRoots: roots}
	facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
	if len(facts) != 0 {
		t.Fatal("stream data became a dispatch fact")
	}
	var out bytes.Buffer
	if err := WriteShadowReport(&out, report, "json", true); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), `"command_stream_roots"`) || !strings.Contains(out.String(), "no_callback_root_or_closed_target_promotion") {
		t.Fatal(out.String())
	}
	var roundTrip ShadowReport
	if err := json.Unmarshal(out.Bytes(), &roundTrip); err != nil {
		t.Fatal(err)
	}
	before, _ := json.Marshal(roots)
	after, _ := json.Marshal(roundTrip.CommandStreamRoots)
	if !bytes.Equal(before, after) {
		t.Fatal("lost root evidence in JSON")
	}
	out.Reset()
	if err := WriteShadowReport(&out, report, "text", true); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "[COMMAND-ROOT-INPUT]") {
		t.Fatal(out.String())
	}
}

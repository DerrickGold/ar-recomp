package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestCallInputEntryRegistersAndBarriers(t *testing.T) {
	for _, reg := range []string{"A", "X", "Y"} {
		w, k := initializerFixture(t, []byte{0x8b, 0x4b, 0xab, 0x0b, 0x2b, 0x08, 0x48, 0xda, 0x5a, 0xf4, 0, 0, 0x60})
		r := w.indexExpression(k, reg, true)
		if r.Source.Kind != "entry_register" || r.Source.Register != reg || r.Source.PC != 0x8000 || r.DomainSize != 65536 {
			t.Fatalf("passive stack/bank operations changed %s: %+v", reg, r)
		}
		if old := w.initializerIndex(k, reg); old.Source.Kind != "unknown" {
			t.Fatalf("old initializer query changed: %+v", old)
		}
	}
	for _, test := range []struct {
		name, reg, kind string
		code            []byte
	}{
		{"PLA", "A", "unknown", []byte{0x68, 0x60}},
		{"PLX", "X", "unknown", []byte{0xfa, 0x60}},
		{"PLY", "Y", "unknown", []byte{0x7a, 0x60}},
		{"PLP", "A", "unknown", []byte{0x28, 0x60}},
		{"byte accumulator", "A", "unknown", []byte{0xe2, 0x20, 0xa9, 3, 0x60}},
		{"byte index", "X", "unknown", []byte{0xe2, 0x10, 0xa2, 3, 0x60}},
		{"status transition", "A", "unknown", []byte{0xc2, 0x20, 0x60}},
		{"callee", "A", "register_after_call", []byte{0x20, 0, 0x81, 0x60}},
		{"arithmetic", "A", "unknown", []byte{0x69, 1, 0, 0x60}},
		{"budget", "A", "unknown", append(bytes.Repeat([]byte{0xea}, shadowPointerWalkLimit+1), 0x60)},
		{"join", "A", "unknown", []byte{0x24, 0x20, 0xd0, 3, 0xa9, 1, 0, 0x60}},
		{"entry backedge", "A", "unknown", []byte{0x24, 0x20, 0xd0, 0xfc, 0x60}},
	} {
		t.Run(test.name, func(t *testing.T) {
			w, k := initializerFixture(t, test.code)
			r := w.indexExpression(k, test.reg, true)
			if r.Source.Kind != test.kind || r.DomainSize != 65536 || len(r.DomainValues) != 0 {
				t.Fatalf("barrier invented an argument: %+v", r)
			}
		})
	}
}

func TestLocalSlotNearestWriterAndClobbers(t *testing.T) {
	// The different spellings might alias: preserve both in execution order.
	w, k := initializerFixture(t, []byte{0xa9, 3, 0, 0x85, 0x40, 0x9e, 0, 0x20, 0x85, 0x42, 0xa5, 0x40, 0x60})
	load := w.graph.KeysAtPC(k.PC - 2)[0]
	r := w.localSlotSource(load, ShadowStoredField{Mode: "dp", Operand: 0x40})
	if r == nil || r.StorePC != 0x8003 || r.LoadPC != 0x800a || r.Value.DomainSize != 1 || r.Value.KnownOne != 3 || len(r.PossibleClobbers) != 2 || r.PossibleClobbers[0].PC != 0x8005 || r.PossibleClobbers[1].PC != 0x8008 {
		t.Fatalf("local writer or aliases: %+v", r)
	}
	for _, test := range []struct {
		name string
		code []byte
	}{
		{"STZ overwrite", []byte{0x64, 0x40}},
		{"RMW overwrite", []byte{0xe6, 0x40}},
		{"byte overwrite", []byte{0xe2, 0x20, 0x85, 0x40, 0xc2, 0x20}},
		{"call", []byte{0x20, 0, 0x81}},
		{"D change", []byte{0x5b}},
		{"DB change", []byte{0xab}},
		{"stack write", []byte{0x48}},
		{"unknown indirect write", []byte{0x91, 0x44}},
		{"join", []byte{0x24, 0x20, 0xd0, 1, 0xea}},
		{"budget", bytes.Repeat([]byte{0xea}, shadowLocalSlotLimit+1)},
	} {
		t.Run(test.name, func(t *testing.T) {
			code := append([]byte{0xa9, 3, 0, 0x85, 0x40}, test.code...)
			code = append(code, 0xa5, 0x40, 0x60)
			w, k := initializerFixture(t, code)
			load := w.graph.KeysAtPC(k.PC - 2)[0]
			if r := w.localSlotSource(load, ShadowStoredField{Mode: "dp", Operand: 0x40}); r != nil {
				t.Fatalf("crossed barrier to older writer: %+v", r)
			}
		})
	}
	for _, code := range [][]byte{{0x86, 0x40}, {0x84, 0x40}} {
		w, k := initializerFixture(t, append(code, 0xa5, 0x40, 0x60))
		r := w.localSlotSource(w.graph.KeysAtPC(k.PC - 2)[0], ShadowStoredField{Mode: "dp", Operand: 0x40})
		if r == nil || r.Value.Source.Kind != "entry_register" || r.Value.Source.Register == "A" {
			t.Fatalf("index register store lost: %+v", r)
		}
	}
	if r := w.localSlotSource(load, ShadowStoredField{Mode: "dp,x", Operand: 0x40}); r != nil {
		t.Fatalf("indexed slot treated as scalar: %+v", r)
	}
	// Even with a nearer word writer, do not use the earlier literal.
	w, k = initializerFixture(t, []byte{0xa9, 3, 0, 0x85, 0x40, 0xad, 0, 0x21, 0x85, 0x40, 0xa5, 0x40, 0x60})
	r = w.localSlotSource(w.graph.KeysAtPC(k.PC - 2)[0], ShadowStoredField{Mode: "dp", Operand: 0x40})
	if r == nil || r.StorePC != 0x8008 || r.Value.DomainSize != 65536 {
		t.Fatalf("nearest word writer lost: %+v", r)
	}
}

func TestLocalSlotScalarAddressingModes(t *testing.T) {
	for _, test := range []struct {
		field       ShadowStoredField
		store, load []byte
	}{
		{ShadowStoredField{Mode: "dp", Operand: 0x40}, []byte{0x85, 0x40}, []byte{0xa5, 0x40}},
		{ShadowStoredField{Mode: "abs", Operand: 0x2040}, []byte{0x8d, 0x40, 0x20}, []byte{0xad, 0x40, 0x20}},
		{ShadowStoredField{Mode: "long", Operand: 0x7e2040}, []byte{0x8f, 0x40, 0x20, 0x7e}, []byte{0xaf, 0x40, 0x20, 0x7e}},
	} {
		t.Run(test.field.Mode, func(t *testing.T) {
			code := append([]byte{0xa9, 3, 0}, test.store...)
			pc := 0x8000 + uint32(len(code))
			code = append(append(code, test.load...), 0x60)
			w, _ := initializerFixture(t, code)
			r := w.localSlotSource(w.graph.KeysAtPC(pc)[0], test.field)
			if r == nil || r.StorePC != 0x8003 || r.Field != test.field || r.Value.KnownOne != 3 || len(r.Obligations) == 0 {
				t.Fatalf("scalar slot evidence: %+v", r)
			}
		})
	}
}

func TestDirectCallInputsTargetsAndWidths(t *testing.T) {
	image := make(romimage.Image, 0x10000)
	copy(image[0x8000:], []byte{0xa9, 7, 0, 0xa2, 8, 0, 0xa0, 9, 0, 0x20, 0, 0x90, 0x22, 0, 0x92, 0x84, 0xe2, 0x20, 0xa9, 5, 0x20, 0, 0x90, 0x60})
	g, err := decoder.DecodeFunction(image, 1, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	calls := collectShadowDirectCallInputs(g)
	if len(calls) != 3 || calls[0].call.TargetPC != 0x019000 || calls[0].call.InstructionBytes != "20 00 90" || calls[1].call.TargetPC != 0x849200 || calls[1].call.InstructionBytes != "22 00 92 84" {
		t.Fatalf("near/far call targets: %+v", calls)
	}
	for i, want := range []uint16{7, 8, 9} {
		if calls[0].values[i].DomainSize != 1 || calls[0].values[i].KnownOne != want {
			t.Fatalf("register %d literal: %+v", i, calls[0].values[i])
		}
	}
	if calls[1].values[0].Source.Kind != "register_after_call" || calls[2].values[0].DomainSize != 65536 {
		t.Fatal("call/byte barrier lost")
	}
	g.Instructions[g.KeysAtPC(0x018009)[0]].Instruction.DispatchKind = "rts"
	if got := collectShadowDirectCallInputs(g); len(got) != 2 {
		t.Fatalf("collapsed dispatch treated as direct call: %+v", got)
	}
	copy(image, []byte{0xfc, 0, 0x90, 0x60})
	indirect, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	if got := collectShadowDirectCallInputs(indirect); len(got) != 0 {
		t.Fatalf("indirect call invented a target: %+v", got)
	}
}

func TestCallInputAttachmentDedupAndMX(t *testing.T) {
	w, _ := initializerFixture(t, []byte{0xa9, 3, 0, 0x20, 0, 0x90, 0x60})
	calls := collectShadowDirectCallInputs(w.graph)
	source := &ShadowLocalSlotSource{EntryPC: 0x9000, EntryMX: analysis.MXState{}, Value: ShadowInitializerIndex{Source: ShadowStoredOrigin{Kind: "entry_register", PC: 0x9000, Register: "A"}, DomainSize: 65536}}
	e := &ShadowPointerIndexEvidence{Initializers: []ShadowTableInitializer{{Read: ShadowInitializerRead{Index: ShadowInitializerIndex{LocalSource: source, DomainSize: 32768}}}}}
	sites := map[uint32]*ShadowDispatchSite{0x8200: {PointerProducers: []ShadowPointerProducer{{IndexEvidence: e}}}}
	mismatch := calls[0]
	mismatch.call.CallMX.M = 1
	mismatch.values[0] = ShadowInitializerIndex{DomainSize: 65536}
	results := []shadowDecodeResult{{callInputs: append(append(calls, calls...), mismatch)}}
	attachShadowEntryCallInputs(results, sites)
	got := e.Initializers[0].Read.Index.LocalSource
	if len(got.Calls) != 2 || got.Calls[0].MatchesEntryMX == got.Calls[1].MatchesEntryMX || len(source.Calls) != 0 || got.Value.DomainSize != 65536 || e.Initializers[0].Read.Index.DomainSize != 32768 {
		t.Fatalf("dedup/MX/no-substitution contract: %+v", got)
	}
}

func TestCallInputsReportOnlyIntegration(t *testing.T) {
	root := t.TempDir()
	image := make(romimage.Image, 0x8000)
	copy(image[0x100:], []byte{0x4b, 0xab, 0xac, 0, 0x20, 0xb9, 0, 0, 0x85, 0x40, 0x20, 0, 0x82, 0x60})
	copy(image[0x200:], []byte{0x6c, 0x40, 0})
	// Entry A -> scalar scratch slot -> table index. The unrelated indexed
	// write is not presumed disjoint, and caller values must not be substituted.
	copy(image[0x300:], []byte{0x8b, 0x4b, 0xab, 0x85, 0x50, 0x9e, 0, 0x21, 0xa5, 0x50, 0x0a, 0xaa, 0xbd, 0, 0x90, 0x8d, 0, 0x20, 0x60})
	for i, operand := range []byte{0x60, 0x62, 0x64} {
		code := []byte{0xa5, operand}
		if i < 2 {
			code = append(code, 0x29, 0xff, 0)
		}
		code = append(code, 0x20, 0, 0x83, 0x60)
		copy(image[0x400+0x100*i:], code)
	}
	options := ShadowAnalysisOptions{ROMPath: filepath.Join(root, "fixture.sfc"), CFGDir: filepath.Join(root, "recomp"), Jobs: 1}
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), "bank = 00\nfunc Caller 8100 entry_mx:0,0\nfunc Init 8300 entry_mx:0,0\nfunc One 8400 entry_mx:0,0\nfunc Two 8500 entry_mx:0,0\nfunc Three 8600 entry_mx:0,0\nhle_dispatch 8200 HostDispatcher\nhle_func 8300 HostInit\n")
	r, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	site := inventorySite(t, r, 0x8200)
	e := site.PointerProducers[0].IndexEvidence
	if len(e.Initializers) != 1 {
		t.Fatal(e)
	}
	read := e.Initializers[0].Read
	local := read.Index.LocalSource
	if local == nil || local.StorePC != 0x8303 || len(local.PossibleClobbers) != 1 || len(local.Calls) != 3 {
		t.Fatalf("local/caller chain: %+v", local)
	}
	for i, size := range []uint32{256, 256, 65536} {
		if local.Calls[i].Value.DomainSize != size || !local.Calls[i].MatchesEntryMX {
			t.Fatalf("caller %d input: %+v", i, local.Calls[i])
		}
	}
	if local.Value.DomainSize != 65536 || read.Index.DomainSize != 32768 || len(read.Samples) != 0 || site.TargetSetStatus != "unproven" || len(site.StaticTargets) != 0 || site.Routing != "hle" || r.EntryAblation.Summary.AuthoredHLEObligations != 1 {
		t.Fatal("caller evidence narrowed initializer, promoted root, or lost HLE")
	}
	db, err := BuildStaticAnalysisDatabase(r)
	if err != nil {
		t.Fatal(err)
	}
	without := r
	without.DispatchSites = nil
	without.DispatchSummary = ShadowDispatchSummary{}
	dbWithout, err := BuildStaticAnalysisDatabase(without)
	if err != nil || !reflect.DeepEqual(db, dbWithout) {
		t.Fatalf("report-only facts entered database: %v", err)
	}
	options.Jobs = 8
	parallel, err := AnalyzeAuthoredShadow(options)
	if err != nil || !reflect.DeepEqual(r, parallel) {
		t.Fatalf("non-deterministic report: %v", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, r, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"local-slot candidate", "possible-alias=", "caller-input:", "entry-mx-match=true", "decoded_direct_callers_only_not_all_entries", "no_value_substitution_or_root_promotion"} {
		if !strings.Contains(text.String(), want) {
			t.Fatalf("missing %q", want)
		}
	}
}

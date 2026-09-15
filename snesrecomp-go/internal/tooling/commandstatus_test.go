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

func nativeStatusFixture(t *testing.T, programs map[uint16][]byte) (romimage.Image, []shadowDecodeResult) {
	t.Helper()
	image := make(romimage.Image, 0x8000)
	var entries []uint16
	for pc, code := range programs {
		copy(image[int(pc)-0x8000:], code)
		entries = append(entries, pc)
	}
	slices.Sort(entries)
	var results []shadowDecodeResult
	for _, pc := range entries {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: uint32(pc)}, statusBody: collectShadowStatusBody(g, nil)})
	}
	return image, results
}

func TestNativeStatusSummaryPathsAndReturns(t *testing.T) {
	for _, tt := range []struct {
		name         string
		code         []byte
		c, d, reason string
	}{
		{"leaf", []byte{0xea, 0x60}, "preserve", "preserve", ""},
		{"comparison only changes C", []byte{0xc9, 1, 0, 0x60}, "unknown", "preserve", ""},
		{"local constants", []byte{0x38, 0xd8, 0x60}, "set", "clear", ""},
		{"REP constants", []byte{0xc2, 9, 0x60}, "clear", "clear", ""},
		{"divergent branches", []byte{0xd0, 3, 0xd8, 0x80, 1, 0xf8, 0x60}, "preserve", "unknown", ""},
		{"common definition after merge", []byte{0xd0, 3, 0xd8, 0x80, 1, 0xf8, 0xd8, 0x60}, "preserve", "clear", ""},
		{"loop fixed point", []byte{0xc9, 1, 0, 0xd0, 0xfb, 0x60}, "unknown", "preserve", ""},
		{"balanced stack", []byte{0xda, 0x48, 0x68, 0xfa, 0x60}, "preserve", "preserve", ""},
		{"byte index stack", []byte{0xe2, 0x10, 0xda, 0xfa, 0xc2, 0x10, 0x60}, "preserve", "preserve", ""},
		{"push pull widths differ", []byte{0xe2, 0x10, 0xda, 0xc2, 0x10, 0xfa, 0x60}, "", "", "native_status_stack_boundary"},
		{"pushed return trick", []byte{0xf4, 0, 0x90, 0x60}, "", "", "native_status_unbalanced_return"},
		{"popped caller frame", []byte{0x68, 0x60}, "", "", "native_status_stack_boundary"},
		{"incompatible stack join", []byte{0xd0, 1, 0x48, 0x60}, "", "", "native_status_"},
		{"status restore", []byte{0x28, 0x60}, "", "", "native_status_barrier_PLP"},
		{"PHP not yet a summary", []byte{0x08, 0x28, 0x60}, "", "", "native_status_barrier_PHP"},
		{"SP assignment", []byte{0x1b, 0x60}, "", "", "native_status_barrier_TCS"},
		{"wrong frame kind", []byte{0x6b}, "", "", "native_status_return_kind_mismatch"},
		{"interrupt return", []byte{0x40}, "", "", "native_status_barrier_RTI"},
		{"scheduler wait", []byte{0xcb}, "", "", "native_status_barrier_WAI"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := nativeStatusFixture(t, map[uint16][]byte{0x8000: tt.code})
			before := commandPathSnapshot(t, results[0].statusBody.graph)
			s := newShadowStatusAnalyzer(image, results).summarize(0x808000, analysis.MXState{}, "RTS")
			if !bytes.Equal(before, commandPathSnapshot(t, results[0].statusBody.graph)) {
				t.Fatal("summary mutated decoder")
			}
			if tt.reason != "" {
				if !strings.HasPrefix(s.Reason, tt.reason) {
					t.Fatalf("%s", s)
				}
				return
			}
			if s.Reason != "" || s.Carry != tt.c || s.Decimal != tt.d || len(s.Returns) == 0 {
				t.Fatalf("%s", s)
			}
		})
	}
}

func TestNativeStatusDirectCallsAndBarriers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		callee []byte
		want   string
	}{
		{"preserve", []byte{0xc9, 1, 0, 0x60}, ""},
		{"clear decimal", []byte{0xd8, 0x60}, ""},
		{"changed return width", []byte{0xe2, 0x20, 0x60}, "native_status_return_MX_mismatch"},
		{"wrong return", []byte{0x6b}, "native_status_return_kind_mismatch"},
		{"recursive call", []byte{0x20, 0, 0x80, 0x60}, "native_status_recursive_group"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := nativeStatusFixture(t, map[uint16][]byte{0x8000: {0x20, 0, 0x81, 0x60}, 0x8100: tt.callee})
			s := newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTS")
			if s.Reason != tt.want {
				t.Fatalf("%s", s)
			}
			if tt.want == "" && (len(s.Calls) != 1 || s.Calls[0].Summary.Context.PC != 0x8100) {
				t.Fatal("lost nested summary")
			}
			slices.Reverse(results)
			if !reflect.DeepEqual(s, newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTS")) {
				t.Fatal("worker order changed summaries")
			}
		})
	}
	image, results := nativeStatusFixture(t, map[uint16][]byte{0x8000: {0x20, 0, 0x81, 0x60}, 0x8100: {0x60}})
	if s := newShadowStatusAnalyzer(image, results[:1]).summarize(0x8000, analysis.MXState{}, "RTS"); s.Reason != "native_status_body_missing" {
		t.Fatalf("%s", s)
	}
	for _, cfg := range []*config.Config{
		{HLEFunctions: map[uint16]string{0x8100: "replacement"}},
		{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8100: {Function: "replacement", Predicate: "enabled"}}},
		{HLEDispatch: map[uint16]string{0x8100: "replacement"}},
		{HLESPCUpload: []uint16{0x8100}},
	} {
		copyResults := slices.Clone(results)
		copyResults[1].statusBody = collectShadowStatusBody(results[1].statusBody.graph, cfg)
		if s := newShadowStatusAnalyzer(image, copyResults).summarize(0x8000, analysis.MXState{}, "RTS"); s.Reason != "native_status_HLE_boundary" {
			t.Fatalf("native summary crossed HLE: %s", s)
		}
	}
	alias := results[1]
	alias.entry.Address |= 0x800000
	if s := newShadowStatusAnalyzer(image, append(results, alias)).summarize(0x8000, analysis.MXState{}, "RTS"); s.Reason != "native_status_ambiguous_body" {
		t.Fatalf("selected alias contract: %s", s)
	}
}

func TestNativeStatusCompletenessAndBudgets(t *testing.T) {
	image, results := nativeStatusFixture(t, map[uint16][]byte{0x8000: {0xea, 0x60}})
	g := results[0].statusBody.graph
	delete(g.Instructions, decoder.DecodeKey{PC: 0x8001})
	if s := newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTS"); s.Reason != "native_status_body_missing" {
		t.Fatalf("partial body accepted: %s", s)
	}
	code := append(bytes.Repeat([]byte{0xea}, shadowStatusBodyLimit), 0x60)
	image, results = nativeStatusFixture(t, map[uint16][]byte{0x8000: code})
	if s := newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTS"); s.Reason != "native_status_body_budget" {
		t.Fatalf("oversized body accepted: %s", s)
	}
	programs := make(map[uint16][]byte)
	for n := 0; n <= shadowStatusCallDepth; n++ {
		pc := uint16(0x8000 + n*0x100)
		programs[pc] = []byte{0x60}
		if n < shadowStatusCallDepth {
			programs[pc] = []byte{0x20, 0, byte((pc + 0x100) >> 8), 0x60}
		}
	}
	image, results = nativeStatusFixture(t, programs)
	if s := newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTS"); s.Reason != "native_status_call_depth" {
		t.Fatalf("call depth unbounded: %s", s)
	}
	image, results = nativeStatusFixture(t, map[uint16][]byte{0x8000: {0x60}})
	a := newShadowStatusAnalyzer(image, results)
	k, _ := a.entryKey(0x8000, analysis.MXState{})
	budget := 0
	if s := a.summarizeKey(shadowStatusKey{k, "RTS"}, make(map[shadowStatusKey]bool), 0, &budget); s.Reason != "native_status_work_budget" {
		t.Fatalf("work limit ignored: %s", s)
	}
	graph := results[0].statusBody.graph
	graph.DispatchTargetsSuppressed = []decoder.DispatchTargetSuppressed{{SitePC: 0x8000, TargetPC: 0x8100}}
	results[0].statusBody = collectShadowStatusBody(graph, nil)
	if s := newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTS"); s.Reason != "native_status_pruned_or_dynamic_graph" {
		t.Fatalf("pruned path ignored: %s", s)
	}
}

func TestCommandInputUsesNativeCalleeSummary(t *testing.T) {
	for _, callee := range [][]byte{{0xc9, 1, 0, 0x60}, {0xd8, 0x60}} {
		image, results, _ := commandPathFixture(t, []byte{0x18, 0x69, 1, 0})
		replaceCommandInputFixture(t, image, results, 0, 0x8000, []byte{0xd8, 0x20, 0, 0x89, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60})
		copy(image[0x900:], callee)
		g, err := decoder.DecodeFunction(image, 0, 0x8900, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		results = append(results, shadowDecodeResult{entry: decoder.Variant{Address: 0x8900}, statusBody: collectShadowStatusBody(g, nil)})
		got := resolveShadowCommandRoots(image, results)
		if len(got) != 1 || len(got[0].References) != 1 || got[0].References[0].TableIndex != 16 {
			t.Fatalf("summary did not unblock input: %+v", got)
		}
		flag := got[0].References[0].Calls[0].Flags.Decimal
		if flag.Value == nil || *flag.Value != 0 || flag.NativeCall == nil || flag.NativeCall.Call.Summary == nil {
			t.Fatalf("missing native dependency: %+v", flag)
		}
		var out bytes.Buffer
		writeShadowCommandRoots(&out, got)
		if !strings.Contains(out.String(), "native-status D") || !strings.Contains(out.String(), "conditional native return") {
			t.Fatal(out.String())
		}
		report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreamRoots: got}
		if facts, _ := SelectStaticProvenDatabaseDispatchFacts(report); len(facts) != 0 {
			t.Fatal("native conditional summary became generation fact")
		}
		b, err := json.Marshal(report)
		if err != nil {
			t.Fatal(err)
		}
		var round ShadowReport
		if err := json.Unmarshal(b, &round); err != nil {
			t.Fatal(err)
		}
		b2, _ := json.Marshal(round)
		if !bytes.Equal(b, b2) {
			t.Fatal("native summary evidence lost")
		}
		slices.Reverse(results)
		if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
			t.Fatal("summary consumption order changed input")
		}
	}
}

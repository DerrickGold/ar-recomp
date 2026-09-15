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

func replaceCommandInputFixture(t *testing.T, image romimage.Image, results []shadowDecodeResult, index int, pc uint16, code []byte) {
	t.Helper()
	copy(image[int(pc)-0x8000:], code)
	g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	results[index].streamInputs = collectShadowStreamInputs(g)
}

func TestCommandInputCorrelatedFlags(t *testing.T) {
	for _, tt := range []struct {
		name                        string
		caller, wrapper, adjustment []byte
		want                        int
	}{
		{"decimal through mirrored wrapper", []byte{0xd8, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, nil, []byte{0x18, 0x69, 1, 0}, 16},
		{"carry and decimal from caller", []byte{0xd8, 0x18, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, nil, []byte{0x69, 1, 0}, 16},
		{"borrow from caller", []byte{0xd8, 0x38, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, nil, []byte{0xe9, 1, 0}, 8},
		{"decimal caller rejected", []byte{0xf8, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, nil, []byte{0x18, 0x69, 1, 0}, -1},
		{"unknown caller rejected", []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, nil, []byte{0x18, 0x69, 1, 0}, -1},
		{"callee may clobber decimal", []byte{0xd8, 0x20, 0, 0x89, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, nil, []byte{0x18, 0x69, 1, 0}, -1},
		{"wrapper overrides outer decimal", []byte{0xd8, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0xf8, 0x22, 0, 0x82, 0x80, 0x60}, []byte{0x18, 0x69, 1, 0}, -1},
		{"wrapper comparison clobbers carry", []byte{0xd8, 0x18, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0xc9, 1, 0, 0x22, 0, 0x82, 0x80, 0x60}, []byte{0x69, 1, 0}, -1},
		{"status restore barrier", []byte{0xd8, 0xa2, 3, 0, 0x28, 0x20, 0, 0x81, 0x60}, nil, []byte{0x18, 0x69, 1, 0}, -1},
		{"caller arithmetic", []byte{0xd8, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x18, 0x69, 1, 0, 0x22, 0, 0x82, 0x80, 0x60}, nil, 16},
		{"caller flag-free increment", []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x1a, 0x22, 0, 0x82, 0x80, 0x60}, nil, 16},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results, _ := commandPathFixture(t, tt.adjustment)
			replaceCommandInputFixture(t, image, results, 0, 0x8000, tt.caller)
			if tt.wrapper != nil {
				replaceCommandInputFixture(t, image, results, 1, 0x8100, tt.wrapper)
			}
			got := resolveShadowCommandRoots(image, results)
			if len(got) != 1 {
				t.Fatalf("roots=%+v", got)
			}
			r := got[0]
			if tt.want < 0 {
				if len(r.References) != 0 || len(r.Unresolved) == 0 {
					t.Fatalf("unproven status supplied a value: %+v", r)
				}
				return
			}
			if len(r.References) != 1 || int(r.References[0].TableIndex) != tt.want || len(r.Unresolved) != 0 {
				t.Fatalf("lost native flag chain: refs=%+v issues=%+v", r.References, r.Unresolved)
			}
			p := r.References[0]
			if len(p.Calls) != 2 || p.Calls[0].Flags == nil || p.Calls[1].Flags == nil {
				t.Fatal("missing flag witnesses")
			}
			if tt.name != "caller flag-free increment" && (len(p.Arithmetic) != 1 || p.Arithmetic[0].Operation.Decimal.Value == nil || *p.Arithmetic[0].Operation.Decimal.Value != 0) {
				t.Fatalf("lost arithmetic evidence: %+v", p)
			}
			slices.Reverse(results)
			if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
				t.Fatal("worker order changed evidence")
			}
		})
	}
}

func TestCommandInputCallerBranchesKeepFlagsAndValuesTogether(t *testing.T) {
	image, results, _ := commandPathFixture(t, []byte{0x18, 0x69, 1, 0})
	// Two predecessors to the SAME call: binary X=3 or decimal X=4. Joining
	// decimal=0 from the first path with X=4 from the second would invent index20.
	caller := []byte{0xc5, 0x20, 0xf0, 6, 0xd8, 0xa2, 3, 0, 0x80, 4, 0xf8, 0xa2, 4, 0, 0x20, 0, 0x81, 0x60}
	replaceCommandInputFixture(t, image, results, 0, 0x8000, caller)
	got := resolveShadowCommandRoots(image, results)
	if len(got) != 1 || len(got[0].References) != 1 || got[0].References[0].TableIndex != 16 || len(got[0].Unresolved) == 0 {
		t.Fatalf("correlation=%+v", got)
	}
	p := got[0].References[0]
	if len(p.Calls[0].InputEdges) != 1 || p.Calls[0].Flags.Decimal.Value == nil || *p.Calls[0].Flags.Decimal.Value != 0 {
		t.Fatalf("lost branch flag proof: %+v", p)
	}
	var text bytes.Buffer
	writeShadowCommandRoots(&text, got)
	if !strings.Contains(text.String(), "caller-requires-edge") || !strings.Contains(text.String(), "same native call path") {
		t.Fatal(text.String())
	}
	report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreamRoots: got}
	facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
	if len(facts) != 0 {
		t.Fatal("conditional call path promoted to fact")
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
		t.Fatal("lost flag/call/path evidence")
	}
	slices.Reverse(results)
	if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
		t.Fatal("worker order changes correlated flags")
	}
}

func TestCommandInputFlagEntryBoundary(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	// The first invocation does not inherit the CLD on the loop's later lap.
	copy(image, []byte{0x18, 0x69, 1, 0, 0xd8, 0x80, 0xf9})
	g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	w := shadowPointerWalk{graph: g, preds: shadowPredecessors(g)}
	flag := shadowCommandFlag(w, decoder.DecodeKey{PC: 0x8001}, 8)
	if flag.Value != nil || flag.Reason != "preserved_entry_flag" {
		t.Fatalf("borrowed future flag: %+v", flag)
	}
}

func TestCommandInputCallerPathBudgetAndGraphIsolation(t *testing.T) {
	image, results, _ := commandPathFixture(t, nil)
	caller := []byte{0xa9, 3, 0}
	for range 4 {
		caller = append(caller, 0xc5, 0x20, 0xf0, 1, 0x1a)
	}
	caller = append(caller, 0xaa, 0x20, 0, 0x81, 0x60)
	copy(image, caller)
	g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	before := commandPathSnapshot(t, g)
	results[0].streamInputs = collectShadowStreamInputs(g)
	if !bytes.Equal(before, commandPathSnapshot(t, g)) {
		t.Fatal("caller path query mutated graph")
	}
	got := resolveShadowCommandRoots(image, results)
	if len(got) != 1 || !got[0].Truncated || len(got[0].References) == 0 || len(got[0].References) > shadowCommandInputPathLimit ||
		!slices.ContainsFunc(got[0].Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == "caller_input_path_budget" }) {
		t.Fatalf("caller path limit lost: %+v", got)
	}
	slices.Reverse(results)
	if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
		t.Fatal("bounded caller order changed evidence")
	}
}

func TestCommandInputFlagContextMismatch(t *testing.T) {
	image, results, _ := commandPathFixture(t, []byte{0x18, 0x69, 1, 0})
	replaceCommandInputFixture(t, image, results, 0, 0x8000, []byte{0xd8, 0xa2, 3, 0, 0x20, 0, 0x81, 0x60})
	for _, edit := range []func(*ShadowCommandRootCall){
		func(c *ShadowCommandRootCall) { c.MX.M = 1 },
		func(c *ShadowCommandRootCall) { c.Caller.PC++ },
	} {
		copyResults := slices.Clone(results)
		copyResults[1].streamInputs = slices.Clone(results[1].streamInputs)
		edit(&copyResults[1].streamInputs[0].call)
		got := resolveShadowCommandRoots(image, copyResults)
		if len(got) != 1 || len(got[0].References) != 0 {
			t.Fatalf("flags crossed context mismatch: %+v", got)
		}
	}
}

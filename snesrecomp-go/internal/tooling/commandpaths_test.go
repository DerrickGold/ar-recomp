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

func commandPathFixture(t *testing.T, prefix []byte) (romimage.Image, []shadowDecodeResult, *decoder.Graph) {
	t.Helper()
	image, results := commandRootFixture(t, []byte{0xa2, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	body := slices.Clone(image[0x200:0x240])
	copy(image[0x200:], append(slices.Clone(prefix), body...))
	copy(image[2*0x8000+0x1010:], []byte{0x40, 0xa0})
	g, err := decoder.DecodeFunction(image, 0, 0x8200, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	before := commandPathSnapshot(t, g)
	s := collectShadowCommandStreams(g)
	results[2].commandRoots = collectShadowCommandRoots(g, s)
	if !bytes.Equal(before, commandPathSnapshot(t, g)) {
		t.Fatal("path query mutated decoder graph")
	}
	return image, results, g
}

func commandPathSnapshot(t *testing.T, g *decoder.Graph) []byte {
	t.Helper()
	var instructions []*decoder.DecodedInstruction
	for _, key := range g.Order {
		instructions = append(instructions, g.Instructions[key])
	}
	b, err := json.Marshal(instructions)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestCommandInputConditionalPaths(t *testing.T) {
	for _, tt := range []struct {
		name   string
		adjust []byte
		both   bool
	}{
		{"known binary add", []byte{0xd8, 0x18, 0x69, 1, 0}, true},
		{"flag free increment", []byte{0x1a}, true},
		{"unknown decimal stays unknown", []byte{0x18, 0x69, 1, 0}, false},
		{"decimal is not binary", []byte{0xf8, 0x18, 0x69, 1, 0}, false},
		{"unknown carry stays unknown", []byte{0xd8, 0x69, 1, 0}, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			prefix := append([]byte{0xc5, 0x20, 0xf0, byte(len(tt.adjust))}, tt.adjust...)
			image, results, g := commandPathFixture(t, prefix)
			before := commandPathSnapshot(t, g)
			roots := collectShadowCommandRoots(g, collectShadowCommandStreams(g))
			after := commandPathSnapshot(t, g)
			if !bytes.Equal(before, after) {
				t.Fatal("path query mutated decoder graph")
			}
			results[2].commandRoots = roots
			got := resolveShadowCommandRoots(image, results)
			if len(got) != 1 || len(got[0].InputPaths) != 2 || got[0].Truncated {
				t.Fatalf("lost alternatives: %+v", got)
			}
			r := got[0]
			want := 1
			if tt.both {
				want = 2
			}
			if len(r.References) != want {
				t.Fatalf("references=%+v issues=%+v", r.References, r.Unresolved)
			}
			var indices []uint16
			for _, ref := range r.References {
				indices = append(indices, ref.TableIndex)
				if len(ref.InputEdges) != 1 || ref.LiteralValue == nil || *ref.LiteralValue != 3 || len(ref.Calls) != 2 || ref.StreamPC == nil {
					t.Fatalf("lost conditional evidence: %+v", ref)
				}
			}
			slices.Sort(indices)
			if indices[0] != 12 || tt.both && indices[1] != 16 {
				t.Fatalf("wrong path arithmetic: %v", indices)
			}
			if tt.both && len(r.Unresolved) != 0 {
				t.Fatalf("known flags rejected: %+v", r.Unresolved)
			}
			if !tt.both && !slices.ContainsFunc(r.Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == "input_operation_or_arithmetic_flags_unproven" }) {
				t.Fatal("unproven path silently disappeared")
			}
			slices.Reverse(results)
			if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
				t.Fatal("worker order changes conditional values")
			}
			report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreamRoots: got}
			facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
			if len(facts) != 0 {
				t.Fatal("branch feasibility promoted to static fact")
			}
			b, _ := json.Marshal(report)
			var round ShadowReport
			if err := json.Unmarshal(b, &round); err != nil {
				t.Fatal(err)
			}
			b2, _ := json.Marshal(round)
			if !bytes.Equal(b, b2) {
				t.Fatal("path/flag evidence lost in JSON")
			}
			var out bytes.Buffer
			writeShadowCommandRoots(&out, got)
			if !strings.Contains(out.String(), "requires-edge") || !strings.Contains(out.String(), "branch feasibility unproven") {
				t.Fatal(out.String())
			}
		})
	}
}

func TestCommandInputPathBounds(t *testing.T) {
	// Four independent conditional increments generate 16 possible histories.
	var prefix []byte
	for range 4 {
		prefix = append(prefix, 0xc5, 0x20, 0xf0, 1, 0x1a)
	}
	image, results, g := commandPathFixture(t, prefix)
	got := resolveShadowCommandRoots(image, results)
	if len(got) != 1 || !got[0].Truncated || len(got[0].InputPaths) > shadowCommandInputPathLimit || len(got[0].InputPaths) == 0 {
		t.Fatalf("path enumeration unbounded: %+v", got)
	}
	// The unordered predecessor inventory must not change budget-cutoff choices.
	w := shadowPointerWalk{graph: g, preds: shadowPredecessors(g)}
	raw := results[2].commandRoots[0]
	at := decoder.DecodeKey{PC: raw.TableRead.LoadPC}
	a, ta := collectShadowCommandInputPaths(w, at, raw.TableRead.IndexRegister)
	for _, preds := range w.preds {
		slices.Reverse(preds)
	}
	b, tb := collectShadowCommandInputPaths(w, at, raw.TableRead.IndexRegister)
	if ta != tb || !reflect.DeepEqual(a, b) {
		t.Fatal("predecessor ordering changes bounded paths")
	}
}

func TestCommandInputNativeArithmetic(t *testing.T) {
	zero, one := uint8(0), uint8(1)
	for _, tt := range []struct {
		op         string
		a, b, want uint16
		c          *uint8
	}{
		{"ADC", 0xffff, 0, 0, &one}, {"SBC", 0, 0, 0xffff, &zero}, {"SBC", 3, 1, 2, &one},
		{"INC", 0xffff, 0, 0, nil}, {"DEC", 0, 0, 0xffff, nil}, {"INX", 3, 0, 4, nil}, {"DEY", 3, 0, 2, nil},
	} {
		op := ShadowStoredOperation{Mnemonic: tt.op, Operand: tt.b, Carry: &ShadowStoredFlag{Value: tt.c}, Decimal: &ShadowStoredFlag{Value: &zero}}
		v, ok := shadowCommandInputWord(tt.a, op)
		if !ok || v != tt.want {
			t.Fatalf("%s=%04X %t want=%04X", tt.op, v, ok, tt.want)
		}
	}
}

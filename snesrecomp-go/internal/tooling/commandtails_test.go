package tooling

import (
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func nativeStatusOwnedFixture(t *testing.T, programs map[uint16][]byte) (romimage.Image, []shadowDecodeResult) {
	t.Helper()
	image, results := nativeStatusFixture(t, programs)
	for n, r := range results {
		siblings := make(map[uint16]struct{})
		for pc := range programs {
			if pc != uint16(r.entry.Address) {
				siblings[pc] = struct{}{}
			}
		}
		g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{SiblingEntryPCs: siblings})
		if err != nil {
			t.Fatal(err)
		}
		results[n].statusBody = collectShadowStatusBody(g, nil)
	}
	return image, results
}

func TestNativeStatusOwnedTails(t *testing.T) {
	for _, tt := range []struct {
		name            string
		caller, tail    []byte
		kind, d, reason string
	}{
		{"same-frame JMP", []byte{0x4c, 0, 0x81}, []byte{0xd8, 0x60}, "RTS", "clear", ""},
		{"same-frame BRL", []byte{0x82, 0xfd, 0}, []byte{0xd8, 0x60}, "RTS", "clear", ""},
		{"long tail preserves RTL frame", []byte{0x5c, 0, 0x81, 0x80}, []byte{0xd8, 0x6b}, "RTL", "clear", ""},
		{"long jump cannot change short return bank", []byte{0x5c, 0, 0x81, 0x80}, []byte{0xd8, 0x60}, "RTS", "", "native_status_short_return_bank_change"},
		{"live saved locals are not a new frame", []byte{0xda, 0x4c, 0, 0x81}, []byte{0xfa, 0x60}, "RTS", "", "native_status_live_stack_tail"},
		{"tail return kind mismatch", []byte{0x4c, 0, 0x81}, []byte{0xd8, 0x6b}, "RTS", "", "native_status_return_kind_mismatch"},
		{"tail ownership cycle", []byte{0x4c, 0, 0x81}, []byte{0x4c, 0, 0x80}, "RTS", "", "native_status_recursive_group"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := nativeStatusOwnedFixture(t, map[uint16][]byte{0x8000: tt.caller, 0x8100: tt.tail})
			s := newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, tt.kind)
			if s.Reason != tt.reason || tt.reason == "" && (s.Decimal != tt.d || len(s.Calls) != 1 || s.Calls[0].Transfer == "" || len(s.Returns) != 1 || s.Returns[0].PC != 0x8101) {
				t.Fatalf("tail: %s %+v", s, s.Calls)
			}
			slices.Reverse(results)
			if !reflect.DeepEqual(s, newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, tt.kind)) {
				t.Fatal("tail evidence depends on worker order")
			}
		})
	}
}

func TestNativeStatusTailBranchAndContinuation(t *testing.T) {
	image, results := nativeStatusOwnedFixture(t, map[uint16][]byte{0x8000: {0xd0, 6, 0x60}, 0x8008: {0xd8, 0x60}})
	s := newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTS")
	if s.Reason != "" || s.Decimal != "unknown" || len(s.Returns) != 2 {
		t.Fatalf("discarded local or shared exit: %s", s)
	}
	image, results = nativeStatusOwnedFixture(t, map[uint16][]byte{0x8000: {0x20, 0, 0x81, 0xd8, 0x6b}, 0x8003: {0xd8, 0x6b}, 0x8100: {0x60}})
	end := uint16(0x8003)
	g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{End: &end})
	if err != nil {
		t.Fatal(err)
	}
	results[0].statusBody = collectShadowStatusBody(g, nil)
	s = newShadowStatusAnalyzer(image, results).summarize(0x8000, analysis.MXState{}, "RTL")
	if s.Reason != "" || s.Decimal != "clear" || len(s.Calls) != 2 {
		t.Fatalf("continuation ownership invented a frame: %s %+v", s, s.Calls)
	}
}

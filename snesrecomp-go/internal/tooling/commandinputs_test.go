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

func TestCommandInputROMReads(t *testing.T) {
	for _, tt := range []struct {
		name            string
		caller, wrapper []byte
		reads           []uint32
		literal         *uint16
	}{
		{"absolute word", []byte{0xf4, 3, 3, 0xab, 0xab, 0xad, 0, 0xb0, 0xaa, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, []uint32{0x03b000}, nil},
		{"long word", []byte{0xaf, 0, 0xb0, 3, 0xaa, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, []uint32{0x03b000}, nil},
		{"indexed absolute", []byte{0xf4, 3, 3, 0xab, 0xab, 0xa0, 2, 0, 0xbe, 0xfe, 0xaf, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, []uint32{0x03b000}, newWord(2)},
		{"index passed through wrapper", []byte{0xa2, 2, 0, 0x20, 0, 0x81, 0x60}, []byte{0xbf, 0xfe, 0xaf, 3, 0x22, 0, 0x82, 0x80, 0x60}, []uint32{0x03b000}, newWord(2)},
		{"nested ROM lookups", []byte{0xa2, 2, 0, 0xbf, 0xfe, 0xaf, 3, 0xaa, 0xbf, 0, 0xb1, 3, 0xaa, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, []uint32{0x03b000, 0x03b103}, newWord(2)},
		{"post-load mask", []byte{0xaf, 0, 0xb0, 3, 0x29, 3, 0, 0xaa, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, []uint32{0x03b000}, nil},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := commandRootFixture(t, tt.caller, tt.wrapper, false)
			copy(image[3*0x8000+0x3000:], []byte{3, 0})
			copy(image[3*0x8000+0x3103:], []byte{3, 0})
			got := resolveShadowCommandRoots(image, results)
			if len(got) != 1 || len(got[0].References) != 1 || len(got[0].Unresolved) != 0 || got[0].Truncated {
				t.Fatalf("lost ROM input: %+v", got)
			}
			p := got[0].References[0]
			if p.Status != "ROM_data_call_path_stream_reference" || p.TableIndex != 12 || p.StreamPC == nil || *p.StreamPC != 0x02a020 || !reflect.DeepEqual(p.LiteralValue, tt.literal) {
				t.Fatalf("reference=%+v", p)
			}
			var addresses []uint32
			for _, r := range p.ROMReads {
				addresses = append(addresses, r.ReadPC)
				if r.Word != 3 || r.LoadMX.M != 0 || r.LoadMX.X != 0 {
					t.Fatalf("lost read width/value: %+v", r)
				}
			}
			if !slices.Equal(addresses, tt.reads) {
				t.Fatalf("reads=%X want=%X", addresses, tt.reads)
			}
			if tt.literal == nil && p.DefinitionPC != 0 {
				t.Fatal("ROM word falsely labeled literal")
			}
			if len(p.Calls) != 2 || p.Calls[1].Value != 3 {
				t.Fatalf("lost actual wrapper argument: %+v", p.Calls)
			}
			slices.Reverse(results)
			if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
				t.Fatal("input order changes ROM references")
			}
			report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreamRoots: got}
			facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
			if len(facts) != 0 {
				t.Fatal("ROM input promoted to static code fact")
			}
			var text bytes.Buffer
			writeShadowCommandRoots(&text, got)
			if !strings.Contains(text.String(), "ROM-input load=") {
				t.Fatal(text.String())
			}
			b, err := json.Marshal(report)
			if err != nil {
				t.Fatal(err)
			}
			var roundTrip ShadowReport
			if err = json.Unmarshal(b, &roundTrip); err != nil {
				t.Fatal(err)
			}
			b2, _ := json.Marshal(roundTrip)
			if !bytes.Equal(b, b2) {
				t.Fatal("ROM provenance lost in serialization")
			}
		})
	}
}

func newWord(v uint16) *uint16 { return &v }

func TestCommandInputROMReadBarriers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		code   []byte
		reason string
	}{
		{"unknown DB", []byte{0xad, 0, 0xb0}, "ROM_input_bank_unproven"},
		{"live PB is not literal DB", []byte{0x4b, 0xab, 0xad, 0, 0xb0}, "ROM_input_bank_unproven"},
		{"RAM without local writer", []byte{0xaf, 0, 0xb0, 0x7e}, "local_input_entry_or_ambiguous_predecessor"},
		{"hardware", []byte{0xaf, 0x10, 0x42, 0}, "input_not_ROM_mapped"},
		{"outside image", []byte{0xaf, 0, 0xb0, 0x20}, "input_not_ROM_mapped"},
		{"unmapped low ROM", []byte{0xaf, 0, 0x60, 3}, "input_not_ROM_mapped"},
		{"word crosses bank", []byte{0xaf, 0xff, 0xff, 3}, "ROM_input_bank_boundary"},
		{"index crosses bank", []byte{0xa2, 2, 0, 0xbf, 0xfe, 0xff, 2}, "ROM_input_bank_boundary"},
		{"unknown index", []byte{0xa6, 0x20, 0xbf, 0, 0xb0, 3}, "local_input_D_unproven"},
		{"masked unknown index", []byte{0xa5, 0x20, 0x29, 3, 0, 0xaa, 0xbf, 0, 0xb0, 3}, "local_input_D_unproven"},
		{"callee clobbers index", []byte{0xa2, 2, 0, 0x20, 0, 0x89, 0xbf, 0xfe, 0xaf, 3}, "nonliteral_input:register_after_call:callee_value_unproven"},
		{"mixed predecessor", []byte{0xa2, 2, 0, 0xc5, 0x20, 0xd0, 3, 0xa2, 4, 0, 0xbf, 0xfe, 0xaf, 3}, "nonliteral_input:unknown:entry_or_ambiguous_predecessor"},
		{"byte read", []byte{0xe2, 0x20, 0xaf, 0, 0xb0, 3, 0xc2, 0x20}, "nonliteral_input:unknown:byte_or_truncated_origin"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			caller := append(slices.Clone(tt.code), 0xaa, 0x20, 0, 0x81, 0x60)
			image, results := commandRootFixture(t, caller, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
			copy(image[3*0x8000+0x3000:], []byte{3, 0})
			got := resolveShadowCommandRoots(image, results)
			if len(got) != 1 || len(got[0].References) != 0 || !slices.ContainsFunc(got[0].Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == tt.reason }) {
				t.Fatalf("barrier lost (%s): %+v", tt.reason, got)
			}
		})
	}
}

func TestCommandInputROMReadDepth(t *testing.T) {
	for _, count := range []int{shadowStreamInputReadDepth, shadowStreamInputReadDepth + 1} {
		code := []byte{0xa2, 0, 0}
		for range count {
			code = append(code, 0xbf, 0, 0xb0, 3, 0xaa)
		}
		code = append(code, 0x20, 0, 0x81, 0x60)
		image, results := commandRootFixture(t, code, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
		got := resolveShadowCommandRoots(image, results)
		if count == shadowStreamInputReadDepth {
			if len(got[0].References) != 1 || len(got[0].References[0].ROMReads) != count || got[0].Truncated {
				t.Fatalf("lost bounded nested reads: %+v", got)
			}
		} else if len(got[0].References) != 0 || !got[0].Truncated || !slices.ContainsFunc(got[0].Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == "ROM_input_read_depth_limit" }) {
			t.Fatalf("unbounded nested reads: %+v", got)
		}
	}
}

func TestCommandInputROMReadsAtBoundary(t *testing.T) {
	image, _ := commandRootFixture(t, []byte{0x60}, []byte{0x60}, false)
	copy(image[0x300:], []byte{0xaf, 0, 0xb0, 3, 0x60})
	end := uint16(0x8304)
	g, err := decoder.DecodeFunction(image, 0, 0x8300, 0, 0, decoder.Options{End: &end})
	if err != nil {
		t.Fatal(err)
	}
	in := collectShadowStreamInputs(g)
	if len(in) != 1 || in[0].reads[0] == nil || in[0].reads[0].LoadPC != 0x8300 {
		t.Fatalf("lost ROM load at boundary: %+v", in)
	}
	// Bank/operand provenance participates in tie ordering, not only call PCs.
	other := in[0]
	r := *other.reads[0]
	r.Operand++
	other.reads[0] = &r
	if shadowStreamInputKey(in[0]) == shadowStreamInputKey(other) {
		t.Fatal("distinct ROM histories collapsed")
	}
}

func TestCommandInputAtInitializerWithoutCallers(t *testing.T) {
	image, _ := commandRootFixture(t, []byte{0x60}, []byte{0x60}, false)
	body := slices.Clone(image[0x200:0x240])
	copy(image[0x200:], append([]byte{0xaf, 0, 0xb0, 3}, body...))
	copy(image[3*0x8000+0x3000:], []byte{3, 0})
	g, err := decoder.DecodeFunction(image, 0, 0x8200, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	s := collectShadowCommandStreams(g)
	got := resolveShadowCommandRoots(image, []shadowDecodeResult{{commandRoots: collectShadowCommandRoots(g, s)}})
	if len(got) != 1 || len(got[0].References) != 1 {
		t.Fatalf("direct ROM input lost: %+v", got)
	}
	p := got[0].References[0]
	if len(p.ROMReads) != 1 || p.ROMReads[0].LoadPC != 0x8200 || len(p.Calls) != 0 || p.LiteralValue != nil || p.TableIndex != 12 {
		t.Fatalf("invented caller/literal: %+v", p)
	}
}

func TestCommandInputHiROMDataBelow8000(t *testing.T) {
	lo, _ := commandRootFixture(t, []byte{0xaf, 0, 0x30, 0xc3, 0xaa, 0x20, 0, 0x81, 0x60}, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
	image := make(romimage.Image, 4*0x10000)
	copy(image[0x8000:0x10000], lo[:0x8000])
	copy(image[0xffc0:], []byte("SYNTHETIC INPUT ROM   "))
	image[0xffd5], image[0xffdc], image[0xffdd], image[0xfffd] = 0x31, 0xff, 0xff, 0x80
	image[0x8201], image[0x8202] = 0xc2, 0xc2
	image[0x2900c], image[0x2900d], image[0x33000] = 0, 0x40, 3
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
		t.Fatalf("HiROM input missing: %+v", got)
	}
	p := got[0].References[0]
	if p.StreamPC == nil || *p.StreamPC != 0xc24000 || len(p.ROMReads) != 1 || p.ROMReads[0].ReadPC != 0xc33000 || p.ROMReads[0].Word != 3 {
		t.Fatalf("LoROM assumption in input query: %+v", p)
	}
}

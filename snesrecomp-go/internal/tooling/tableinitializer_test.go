package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func initializerFixture(t *testing.T, code []byte) (shadowPointerWalk, decoder.DecodeKey) {
	t.Helper()
	image := make(romimage.Image, 0x8000)
	copy(image, code)
	g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	keys := g.KeysAtPC(0x8000 + uint32(len(code)) - 1)
	if len(keys) != 1 {
		t.Fatalf("fixture has no unique final RTS: %+v", keys)
	}
	return shadowPointerWalk{graph: g, preds: shadowPredecessors(g)}, keys[0]
}

func TestInitializerWordDomains(t *testing.T) {
	for _, test := range []struct {
		name      string
		code      []byte
		values    []uint16
		zero, one uint16
		size      uint32
	}{
		{"mask and stride", []byte{0xad, 0, 0x20, 0x29, 3, 0, 0x0a, 0xaa, 0x60}, []uint16{0, 2, 4, 6}, 0xfff9, 0, 4},
		{"sparse mask", []byte{0xad, 0, 0x20, 0x29, 5, 0, 0xaa, 0x60}, []uint16{0, 1, 4, 5}, 0xfffa, 0, 4},
		{"shift without bound", []byte{0xad, 0, 0x20, 0x0a, 0xaa, 0x60}, nil, 1, 0, 32768},
		{"literal wrapping", []byte{0xa9, 0x01, 0x80, 0x0a, 0xaa, 0x60}, []uint16{2}, 0xfffd, 2, 1},
		{"shift mask OR XOR", []byte{0xad, 0, 0x20, 0x4a, 0x29, 3, 0, 0x09, 0x10, 0, 0x49, 1, 0, 0xaa, 0x60}, []uint16{0x10, 0x11, 0x12, 0x13}, 0xffec, 0x10, 4},
		{"unrelated comparison is not a bound", []byte{0xad, 0, 0x20, 0xe0, 4, 0, 0xaa, 0x60}, nil, 0, 0, 65536},
		{"mask after call", []byte{0x20, 0, 0x81, 0x29, 1, 0, 0xaa, 0x60}, []uint16{0, 1}, 0xfffe, 0, 2},
		{"byte origin never becomes literal", []byte{0xe2, 0x20, 0xa9, 1, 0xaa, 0x60}, nil, 0, 0, 65536},
		{"unsupported arithmetic before mask", []byte{0x69, 1, 0, 0x29, 1, 0, 0xaa, 0x60}, []uint16{0, 1}, 0xfffe, 0, 2},
	} {
		t.Run(test.name, func(t *testing.T) {
			w, k := initializerFixture(t, test.code)
			r := w.initializerIndex(k, "X")
			if r.KnownZero != test.zero || r.KnownOne != test.one || r.DomainSize != test.size || !reflect.DeepEqual(r.DomainValues, test.values) {
				t.Fatalf("domain = %+v", r)
			}
			if test.name == "mask and stride" && (r.Source.PC != 0x8000 || r.Field == nil || r.Field.Operand != 0x2000) {
				t.Fatal(r)
			}
		})
	}
}

func TestInitializerBitsExhaustive(t *testing.T) {
	// Compare the domain abstraction with every concrete 16-bit input. This
	// exercises sparse masks, high-bit shifts, wrapping, and sequential mixing.
	sequences := [][]ShadowStoredOperation{
		{{Mnemonic: "ASL"}}, {{Mnemonic: "LSR"}},
		{{Mnemonic: "AND", Operand: 5}, {Mnemonic: "ASL"}, {Mnemonic: "ORA", Operand: 0x8000}},
		{{Mnemonic: "AND", Operand: 0xf03f}, {Mnemonic: "EOR", Operand: 0xfedc}, {Mnemonic: "LSR"}, {Mnemonic: "AND", Operand: 0x55}},
	}
	for _, ops := range sequences {
		zero, one := uint16(0), uint16(0)
		for _, op := range ops {
			zero, one = initializerBits(zero, one, op)
		}
		for v := uint32(0); v <= 0xffff; v++ {
			out := uint16(v)
			for _, op := range ops {
				switch op.Mnemonic {
				case "ASL":
					out <<= 1
				case "LSR":
					out >>= 1
				case "AND":
					out &= op.Operand
				case "ORA":
					out |= op.Operand
				case "EOR":
					out ^= op.Operand
				}
			}
			if out&zero != 0 || out&one != one {
				t.Fatalf("excluded concrete word %04X -> %04X", v, out)
			}
		}
	}
}

func TestInitializerBankStackAndBarriers(t *testing.T) {
	for _, test := range []struct {
		name string
		code []byte
		bank int
	}{
		{"word PHA first pull", []byte{0xa9, 0x12, 0x34, 0x48, 0xab, 0x60}, 0x12},
		{"word PHA second pull", []byte{0xa9, 0x12, 0x34, 0x48, 0xab, 0xab, 0x60}, 0x34},
		{"byte PHA", []byte{0xe2, 0x20, 0xa9, 5, 0x48, 0xab, 0x60}, 5},
		{"byte PHA overpull", []byte{0xe2, 0x20, 0xa9, 5, 0x48, 0xab, 0xab, 0x60}, -1},
		{"PEA high", []byte{0xf4, 0x12, 0x34, 0xab, 0xab, 0x60}, 0x34},
		{"PHK", []byte{0x4b, 0xab, 0x60}, 0},
		{"unknown PHA", []byte{0xad, 0, 0x20, 0x48, 0xab, 0x60}, -1},
		{"call barrier", []byte{0x4b, 0xab, 0x20, 0, 0x81, 0x60}, -1},
		{"unknown replacement", []byte{0x4b, 0xab, 0xab, 0x60}, -1},
		{"long straight path", append(append([]byte{0xa9, 0, 5, 0x48, 0xab, 0xab}, bytes.Repeat([]byte{0xea}, 80)...), 0x60), 5},
		{"walk cap", append(append([]byte{0x4b, 0xab}, bytes.Repeat([]byte{0xea}, 256)...), 0x60), -1},
	} {
		t.Run(test.name, func(t *testing.T) {
			w, k := initializerFixture(t, test.code)
			r := w.initializerBank(k)
			if test.bank < 0 {
				if !r.UnknownPaths || len(r.Constants) != 0 {
					t.Fatal(r)
				}
			} else if r.UnknownPaths || len(r.Constants) != 1 || int(r.Constants[0].Value) != test.bank {
				t.Fatal(r)
			}
		})
	}
	// A decoded join cannot inherit a bank from only one predecessor. BIT
	// prevents the decoder's literal-Z branch fold from making this vacuous.
	w, k := initializerFixture(t, []byte{0x24, 0x20, 0xd0, 4, 0x4b, 0xab, 0x80, 4, 0xf4, 1, 1, 0xab, 0x60})
	if r := w.initializerBank(k); !r.UnknownPaths || len(r.Constants) != 0 {
		t.Fatalf("join became bank proof: %+v", r)
	}
}

func TestInitializerTwoLevelPointerAndNoSubstitution(t *testing.T) {
	for _, clobber := range [][]byte{nil, {0x64, 0x98}, {0x5b}, {0x20, 0, 0x83}} {
		code := []byte{0x4b, 0xab, 0xad, 0, 0x20, 0x29, 1, 0, 0x0a, 0xa8, 0xb9, 0, 0x90, 0x85, 0x98}
		code = append(code, clobber...)
		code = append(code, 0xad, 2, 0x20, 0x29, 3, 0, 0x0a, 0xa8, 0xb1, 0x98, 0x8d, 0, 0x21, 0x60)
		w, _ := initializerFixture(t, code)
		_, writes := collectShadowStoredTargets(w.graph)
		var init *ShadowTableInitializer
		for _, write := range writes {
			if write.field.Operand == 0x2100 {
				init = write.initializer
			}
		}
		if init == nil || init.Read.Mode != "(dp),y" || init.Read.Index.DomainSize != 4 {
			t.Fatalf("initializer = %+v", init)
		}
		if (init.Read.PointerSource != nil) != (len(clobber) == 0) {
			t.Fatalf("pointer clobber contract: %+v", init.Read)
		}
		if len(clobber) != 0 {
			continue
		}
		if init.Read.PointerStorePC != 0x800d || init.Read.PointerSource.Operand != 0x9000 || init.Read.PointerSource.Index.DomainSize != 2 {
			t.Fatal(init.Read)
		}
		e := &ShadowPointerIndexEvidence{Initializers: []ShadowTableInitializer{*init}}
		site := &ShadowDispatchSite{PointerProducers: []ShadowPointerProducer{{IndexEvidence: e}}}
		image := make(romimage.Image, 0x8000)
		copy(image[0x1000:], []byte{0, 0xa0, 2, 0xa0})
		attachInitializerSamples(image, nil, map[uint32]*ShadowDispatchSite{0: site})
		r := e.Initializers[0].Read
		if len(r.Samples) != 0 || len(r.PointerSource.Samples) != 2 || r.PointerSource.Samples[0].Word == nil || *r.PointerSource.Samples[0].Word != 0xa000 {
			t.Fatalf("unproven pointer substituted or source samples lost: %+v", r)
		}
	}
}

func TestInitializerInventoryIsReportOnly(t *testing.T) {
	root := t.TempDir()
	image := make(romimage.Image, 0x8000)
	// Caller uses a script pointer in $2000. A separate writer initializes it
	// from a table whose local index mask/shift yields four possible words.
	copy(image[0x100:], []byte{0x4b, 0xab, 0xac, 0, 0x20, 0xb9, 0, 0, 0x85, 0x40, 0x20, 0, 0x82, 0x60})
	copy(image[0x200:], []byte{0x6c, 0x40, 0})
	copy(image[0x300:], []byte{0xa9, 0, 0, 0x48, 0xab, 0xab, 0xad, 0, 0x21, 0x29, 3, 0, 0x0a, 0xaa, 0xbd, 0, 0x90, 0x8d, 0, 0x20, 0x60})
	copy(image[0x1000:], []byte{0, 0xa0, 2, 0xa0, 4, 0xa0, 6, 0xa0})
	options := ShadowAnalysisOptions{ROMPath: filepath.Join(root, "fixture.sfc"), CFGDir: filepath.Join(root, "recomp"), Jobs: 1}
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), "bank = 00\nfunc Caller 8100 entry_mx:0,0\nfunc Init 8300 entry_mx:0,0\nhle_dispatch 8200 HostDispatcher\nhle_func 8300 HostInit\n")
	r, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	site := inventorySite(t, r, 0x8200)
	e := site.PointerProducers[0].IndexEvidence
	if len(e.Initializers) != 1 || len(e.Initializers[0].Read.Samples) != 4 || len(e.Values) != 0 || len(e.Reads) != 0 || site.TargetSetStatus != "unproven" || len(site.StaticTargets) != 0 || site.Routing != "hle" {
		t.Fatalf("initializer became dispatch fact: %+v", e)
	}
	if !slices.Contains(e.Initializers[0].Read.Obligations, "stored_word_not_handler_or_stream_root") {
		t.Fatal("missing obligation")
	}
	before, err := BuildStaticAnalysisDatabase(r)
	if err != nil {
		t.Fatal(err)
	}
	without := r
	without.DispatchSites = nil
	without.DispatchSummary = ShadowDispatchSummary{}
	after, err := BuildStaticAnalysisDatabase(without)
	if err != nil || !reflect.DeepEqual(before, after) {
		t.Fatalf("evidence changed database: %v", err)
	}
	if r.EntryAblation.Summary.AuthoredHLEObligations != 1 {
		t.Fatal("HLE obligation lost")
	}
	options.Jobs = 8
	second, err := AnalyzeAuthoredShadow(options)
	if err != nil || !reflect.DeepEqual(r, second) {
		t.Fatalf("non-deterministic report: %v", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, r, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"table-initializer (report-only)", "domain-superset=4", "conditional_rom_word_not_a_root"} {
		if !strings.Contains(text.String(), want) {
			t.Fatalf("missing %q", want)
		}
	}
}

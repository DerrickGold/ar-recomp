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

func pointerProducerFixture(t *testing.T, code []byte, slot uint16, regions []decoder.DataRegion) (romimage.Image, *decoder.Graph, []ShadowPointerProducer) {
	t.Helper()
	image := make(romimage.Image, 0x8000)
	copy(image[0x100:], code)
	copy(image[0x200:], []byte{0x6c, byte(slot), byte(slot >> 8)})
	graph, err := decoder.DecodeFunction(image, 0, 0x8100, 0, 0, decoder.Options{DataRegions: regions})
	if err != nil {
		t.Fatal(err)
	}
	return image, graph, collectShadowPointerProducers(image, graph, regions)
}

func pointerProducerCode(parts ...[]byte) []byte {
	var result []byte
	for _, part := range parts {
		result = append(result, part...)
	}
	return result
}

func TestShadowPointerProducerDefUse(t *testing.T) {
	setup := []byte{0xa9, 0, 0, 0x5b, 0xf4, 0x80, 0x80, 0xab} // D=0, DB=$80
	index := []byte{0xbd, 0x00, 0x20, 0xa8}                   // LDA $2000,X; TAY
	load := []byte{0xb9, 0x00, 0x90}                          // LDA $9000,Y
	storeCall := []byte{0x85, 0x40, 0x20, 0x00, 0x82, 0x60}   // STA $40; JSR $8200; RTS
	for _, test := range []struct {
		name string
		code []byte
	}{
		{"accumulator", pointerProducerCode(setup, index, load, storeCall)},
		{"transfer to X", pointerProducerCode(setup, index, load, []byte{0xaa, 0x86, 0x40, 0x20, 0, 0x82, 0x60})},
		{"transfer to Y", pointerProducerCode(setup, index, load, []byte{0xa8, 0x84, 0x40, 0x20, 0, 0x82, 0x60})},
		{"decoded branch over noncode", pointerProducerCode(setup, index, load, []byte{0x80, 1, 0xff}, storeCall)},
		{"X to Y to A", pointerProducerCode(setup, []byte{0xbe, 0, 0x90, 0x9b, 0x98}, storeCall)},
	} {
		t.Run(test.name, func(t *testing.T) {
			_, _, findings := pointerProducerFixture(t, test.code, 0x40, nil)
			if len(findings) != 1 {
				t.Fatalf("findings = %+v", findings)
			}
			p := findings[0]
			if p.TrampolinePC != 0x8200 || p.CallerEntryPC != 0x8100 || p.TableOffset != 0x9000 || p.IndexRegister != "Y" ||
				p.PointerAlias != "local_constant" || p.DirectPage.UnknownPaths || p.DataBank.UnknownPaths ||
				!reflect.DeepEqual(p.ROMBaseCandidates, []uint32{0x809000}) || !slices.Contains(p.ProofObligations, "index_domain_and_stride") {
				t.Fatalf("producer = %+v", p)
			}
			if test.name != "X to Y to A" && (p.IndexOrigin == nil || p.IndexOrigin.PC != 0x8108 || p.IndexOrigin.Mnemonic != "LDA" || p.IndexOrigin.Operand != 0x2000) {
				t.Fatalf("lost index provenance: %+v", p.IndexOrigin)
			}
		})
	}
	for _, test := range []struct {
		name string
		code []byte
	}{
		{"arithmetic clobber", pointerProducerCode(setup, load, []byte{0x69, 1, 0}, storeCall)},
		{"register clobber", pointerProducerCode(setup, load, []byte{0x8a}, storeCall)},
		{"call clobber", pointerProducerCode(setup, load, []byte{0x20, 0, 0x83}, storeCall)},
		{"unsupported stack shuffle", pointerProducerCode(setup, load, []byte{0xda, 0xfa}, storeCall)},
		{"aliasing store", pointerProducerCode(setup, load, []byte{0x85, 0x40, 0x64, 0x40, 0x20, 0, 0x82, 0x60})},
		{"unreachable store", pointerProducerCode(setup, load, []byte{0x80, 2}, storeCall)},
		{"different slot", pointerProducerCode(setup, load, []byte{0x85, 0x42, 0x20, 0, 0x82, 0x60})},
		{"byte pointer", pointerProducerCode(setup, []byte{0xe2, 0x20}, load, storeCall)},
		{"truncated transfer", pointerProducerCode(setup, []byte{0xe2, 0x10}, load, []byte{0xaa, 0x8a}, storeCall)},
		{"competing loads", pointerProducerCode(setup, []byte{0xd0, 5}, load, []byte{0x80, 3, 0xb9, 0, 0x91}, storeCall)},
		{"absolute store unsupported", pointerProducerCode(setup, load, []byte{0x8d, 0x40, 0, 0x20, 0, 0x82, 0x60})},
		{"nonzero D mismatch", pointerProducerCode([]byte{0xa9, 0, 1, 0x5b}, load, storeCall)},
	} {
		t.Run(test.name, func(t *testing.T) {
			_, _, findings := pointerProducerFixture(t, test.code, 0x40, nil)
			if len(findings) != 0 {
				t.Fatalf("invented def-use chain: %+v", findings)
			}
		})
	}
	_, _, findings := pointerProducerFixture(t, pointerProducerCode([]byte{0xa9, 0, 1, 0x5b}, load, storeCall), 0x140, nil)
	if len(findings) != 1 || findings[0].RequiredD != 0x100 || findings[0].PointerAlias != "local_constant" {
		t.Fatalf("missed proven nonzero D alias: %+v", findings)
	}
	_, _, findings = pointerProducerFixture(t, pointerProducerCode(load, storeCall), 0x40, nil)
	if len(findings) != 1 || findings[0].PointerAlias != "conditional" || !findings[0].DirectPage.UnknownPaths || !findings[0].DataBank.UnknownPaths || len(findings[0].ROMBaseCandidates) != 0 {
		t.Fatalf("unknown D/DB became proof: %+v", findings)
	}
	_, _, findings = pointerProducerFixture(t, pointerProducerCode(setup, []byte{0x20, 0, 0x83}, load, storeCall), 0x40, nil)
	if len(findings) != 1 || !findings[0].DirectPage.UnknownPaths || !findings[0].DataBank.UnknownPaths || len(findings[0].DataBank.Constants) != 0 {
		t.Fatalf("callee incorrectly preserves D/DB: %+v", findings)
	}
	_, _, findings = pointerProducerFixture(t, pointerProducerCode(setup, load, storeCall), 0x40, []decoder.DataRegion{{Bank: 0, Start: 0x8201, End: 0x8202}})
	if len(findings) != 0 {
		t.Fatalf("decoded trampoline across data ownership: %+v", findings)
	}
}

func TestShadowPointerProducerLoopAndBankEvidence(t *testing.T) {
	// The loop's CPX bounds object iteration, not the Y loaded from an object.
	// The backedge crosses a callee: neither D nor DB has an all-path constant.
	code := []byte{
		0xa9, 0, 0, 0x5b, 0xf4, 0x80, 0x80, 0xab,
		0xa2, 0, 0, // 8108 LDX #0
		0x86, 0x44, // 810B STX $44 (loop join)
		0xbd, 0x00, 0x20, // 810D LDA $2000,X
		0xf0, 0x09, // 8110 BEQ $811B
		0xa8, 0xb9, 0x00, 0x90, 0x85, 0x40, 0x20, 0x00, 0x82,
		0xa6, 0x44, 0xe8, 0xe8, 0xe0, 0x50, 0x00,
		0xd0, 0xe7, // 8122 BNE $810B
		0x60,
	}
	_, _, findings := pointerProducerFixture(t, code, 0x40, nil)
	if len(findings) != 1 {
		t.Fatalf("findings = %+v", findings)
	}
	p := findings[0]
	if !p.DataBank.UnknownPaths || !p.DirectPage.UnknownPaths || len(p.DataBank.Constants) != 1 || p.DataBank.Constants[0].Value != 0x80 ||
		p.IndexOrigin == nil || p.IndexOrigin.Operand != 0x2000 || !slices.Contains(p.ProofObligations, "data_bank_at_load") || !slices.Contains(p.ProofObligations, "index_domain_and_stride") {
		t.Fatalf("loop evidence overclaimed: %+v", p)
	}
	for _, test := range []struct {
		name string
		code []byte
		base uint32
	}{
		{"PHK PLB", []byte{0x4b, 0xab, 0xb9, 0, 0x90}, 0x9000},
		{"explicit long bank", []byte{0xbf, 0, 0x90, 0x80}, 0x809000},
		{"WRAM long is not ROM", []byte{0xbf, 0, 0x90, 0x7e}, 0},
	} {
		t.Run(test.name, func(t *testing.T) {
			_, _, findings := pointerProducerFixture(t, pointerProducerCode(test.code, []byte{0x85, 0x40, 0x20, 0, 0x82, 0x60}), 0x40, nil)
			if len(findings) != 1 || findings[0].DataBank.UnknownPaths {
				t.Fatalf("bank evidence = %+v", findings)
			}
			if test.base == 0 && len(findings[0].ROMBaseCandidates) != 0 || test.base != 0 && !reflect.DeepEqual(findings[0].ROMBaseCandidates, []uint32{test.base}) {
				t.Fatalf("ROM base = %+v", findings[0].ROMBaseCandidates)
			}
		})
	}
}

func TestShadowPointerProducerInventoryIsReportOnlyAndDeterministic(t *testing.T) {
	image, graph, findings := pointerProducerFixture(t, []byte{0xbd, 0, 0x90, 0x85, 0x40, 0x20, 0, 0x82, 0x60}, 0x40, nil)
	if len(findings) != 1 {
		t.Fatal(findings)
	}
	root := t.TempDir()
	options := ShadowAnalysisOptions{ROMPath: filepath.Join(root, "fixture.sfc"), CFGDir: filepath.Join(root, "recomp"), Jobs: 1}
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), "bank = 00\nfunc Caller 8100 entry_mx:0,0\nhle_dispatch 8200 HostDispatcher\n")
	first, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	site := inventorySite(t, first, 0x8200)
	if len(site.PointerProducers) != 1 || site.Routing != "hle" || site.TargetSetStatus != "unproven" || len(site.StaticTargets) != 0 {
		t.Fatalf("inventory = %+v", site)
	}
	before, err := BuildStaticAnalysisDatabase(first)
	if err != nil {
		t.Fatal(err)
	}
	withoutProducers := first
	withoutProducers.DispatchSites = nil
	after, err := BuildStaticAnalysisDatabase(withoutProducers)
	if err != nil || !reflect.DeepEqual(before, after) {
		t.Fatalf("pointer provenance affects static facts: %v", err)
	}
	options.Jobs = 8
	second, err := AnalyzeAuthoredShadow(options)
	if err != nil || !reflect.DeepEqual(first, second) {
		t.Fatalf("worker-count dependent report: %v", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, first, "text", true); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(text.String(), "pointer-producer (report-only)") || !strings.Contains(text.String(), "index_domain_and_stride") {
		t.Fatal("missing actionable text finding")
	}
	duplicated := append(collectShadowPointerProducers(image, graph, nil), findings...)
	if len(normalizeShadowPointerProducers(duplicated)) != 1 {
		t.Fatal("duplicated abstract paths were not deduplicated")
	}
}

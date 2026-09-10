package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestCallerSourceTableIndexNotArgumentDomain(t *testing.T) {
	for _, setup := range [][]byte{nil, {0x4b, 0xab}} {
		// The table has two possible word offsets; a mask AFTER the load
		// constrains the argument, not the table index or the callee's index.
		code := append([]byte{}, setup...)
		code = append(code, 0xad, 0, 0x20, 0x29, 1, 0, 0x0a, 0xa8, 0xb9, 0, 0x90, 0x29, 0xff, 0, 0x20, 0, 0x83, 0x60)
		w, _ := initializerFixture(t, code)
		calls := collectShadowDirectCallInputs(w.graph)
		if len(calls) != 1 || calls[0].values[0].DomainSize != 256 {
			t.Fatalf("argument domain: %+v", calls)
		}
		prototype := calls[0].sources[0]
		source := cloneShadowCallerSource(prototype)
		if source == nil || source.Table == nil || source.Table.Index.DomainSize != 2 || !reflect.DeepEqual(source.Table.Index.DomainValues, []uint16{0, 2}) || source.Table.PointerSource != nil {
			t.Fatalf("caller index confused with argument: %+v", source)
		}
		image := make(romimage.Image, 0x8000)
		copy(image[0x1000:], []byte{0x34, 0x12, 0x78, 0x56})
		attachInitializerReadSamples(image, nil, []*ShadowInitializerRead{source.Table})
		if len(prototype.Table.Samples) != 0 || len(prototype.Table.Index.Writers) != 0 {
			t.Fatal("mutated raw graph source")
		}
		if len(setup) == 0 {
			if !source.Table.DataBank.UnknownPaths || len(source.Table.Samples) != 0 {
				t.Fatal("guessed DB=PB")
			}
		} else if len(source.Table.Samples) != 2 || *source.Table.Samples[0].Word != 0x1234 || *source.Table.Samples[1].Word != 0x5678 {
			t.Fatalf("ROM words: %+v", source.Table.Samples)
		}
		if calls[0].values[0].DomainSize != 256 || source.Table.Index.DomainSize != 2 {
			t.Fatal("samples substituted into domains")
		}
	}
}

func TestCallerSourceStopsWithoutMemoryLoad(t *testing.T) {
	for _, code := range [][]byte{
		{0xa9, 2, 0, 0x20, 0, 0x83, 0x60},             // immediate
		{0x68, 0x20, 0, 0x83, 0x60},                   // pull, not a proven source load
		{0xe2, 0x20, 0xa5, 0x40, 0x20, 0, 0x83, 0x60}, // byte input
		{0x20, 0, 0x82, 0x20, 0, 0x83, 0x60},          // callee effect
	} {
		w, _ := initializerFixture(t, code)
		calls := collectShadowDirectCallInputs(w.graph)
		if source := calls[len(calls)-1].sources[0]; source != nil {
			t.Fatalf("invented source beyond barrier: %+v", source)
		}
	}
	// The indirect table read is documented, but its pointer source is not
	// expanded: one caller-source hop cannot initiate recursive chasing.
	w, _ := initializerFixture(t, []byte{0x4b, 0xab, 0xa0, 0, 0, 0xb9, 0, 0x90, 0x85, 0x40, 0xa0, 0, 0, 0xb1, 0x40, 0x20, 0, 0x83, 0x60})
	source := collectShadowDirectCallInputs(w.graph)[0].sources[0]
	if source == nil || source.Table == nil || source.Table.PointerSource != nil {
		t.Fatalf("indirect source expansion: %+v", source)
	}
	image := make(romimage.Image, 0x8000)
	attachInitializerReadSamples(image, nil, []*ShadowInitializerRead{source.Table})
	if len(source.Table.Samples) != 0 {
		t.Fatal("unproven pointer was sampled")
	}
}

func TestCallerSourceExactWriterIndexAndContextMerge(t *testing.T) {
	field := ShadowStoredField{Mode: "abs", Operand: 0x2040}
	value := uint16(3)
	a := ShadowStoredTargetWriter{Contexts: []analysis.EntryVariant{{PC: 0x8000}}, StorePC: 0x8003, Width: 2, StoredValue: &value}
	b := a
	b.Contexts = []analysis.EntryVariant{{PC: 0x8001}}
	results := []shadowDecodeResult{{storedWrites: []shadowStoredWrite{{field: field, writer: a}, {field: field, writer: b}, {field: field, writer: a},
		{field: ShadowStoredField{Mode: "abs,x", Operand: field.Operand}, writer: a}}}}
	writers := shadowFieldWriterIndex(results, map[ShadowStoredField]bool{field: true})
	if len(writers) != 1 || len(writers[field]) != 1 || len(writers[field][0].Contexts) != 2 || len(a.Contexts) != 1 {
		t.Fatalf("exact-field/context contract: %+v", writers)
	}
}

func TestCallerSourceReportOnlyIntegration(t *testing.T) {
	root := t.TempDir()
	image := make(romimage.Image, 0x8000)
	copy(image[0x100:], []byte{0x4b, 0xab, 0xac, 0, 0x20, 0xb9, 0, 0, 0x85, 0x40, 0x20, 0, 0x82, 0x60})
	copy(image[0x200:], []byte{0x6c, 0x40, 0})
	copy(image[0x300:], []byte{0x8b, 0x4b, 0xab, 0x85, 0x50, 0xa5, 0x50, 0x0a, 0xaa, 0xbd, 0, 0x90, 0x8d, 0, 0x20, 0x60})
	// Direct caller one: bounded table index, independent argument mask.
	copy(image[0x400:], []byte{0x4b, 0xab, 0xad, 0, 0x21, 0x29, 1, 0, 0x0a, 0xa8, 0xb9, 0, 0x91, 0x29, 0xff, 0, 0x20, 0, 0x83, 0x60})
	// Direct caller two: unbounded scalar source, with two candidate writers.
	copy(image[0x500:], []byte{0xad, 0x40, 0x21, 0x20, 0, 0x83, 0x60})
	copy(image[0x600:], []byte{0xa9, 3, 0, 0x8d, 0x40, 0x21, 0x60})
	copy(image[0x700:], []byte{0xad, 0x42, 0x21, 0x8d, 0x40, 0x21, 0x60})
	copy(image[0x1100:], []byte{0x34, 0x12, 0x78, 0x56})
	options := ShadowAnalysisOptions{ROMPath: filepath.Join(root, "fixture.sfc"), CFGDir: filepath.Join(root, "recomp"), Jobs: 1}
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(options.CFGDir, "bank00.cfg"), "bank = 00\nfunc Caller 8100 entry_mx:0,0\nfunc Init 8300 entry_mx:0,0\nfunc TableCaller 8400 entry_mx:0,0\nfunc ScalarCaller 8500 entry_mx:0,0\nfunc LiteralWriter 8600 entry_mx:0,0\nfunc UnknownWriter 8700 entry_mx:0,0\nhle_dispatch 8200 HostDispatcher\nhle_func 8300 HostInit\n")
	r, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	site := inventorySite(t, r, 0x8200)
	init := site.PointerProducers[0].IndexEvidence.Initializers[0].Read
	calls := init.Index.LocalSource.Calls
	if len(calls) != 2 || calls[0].SourceEvidence == nil || calls[1].SourceEvidence == nil {
		t.Fatalf("caller source attachment: %+v", calls)
	}
	table, scalar := calls[0].SourceEvidence.Table, calls[1].SourceEvidence
	bank := calls[0].SourceEvidence.BankContext
	if bank == nil || bank.Local.Status != "local_constant_set" || len(bank.Local.Banks) != 1 || bank.Local.Banks[0] != 0 || bank.ProgramBudgetHit {
		t.Fatalf("report-only bank context: %+v", bank)
	}
	if table == nil || table.Index.DomainSize != 2 || len(table.Samples) != 2 || len(scalar.Writers) != 2 || scalar.Writers[0].StoredValue == nil || scalar.Writers[1].StoredValue != nil {
		t.Fatalf("table/scalar sources: %+v / %+v", table, scalar)
	}
	if calls[0].Value.DomainSize != 256 || calls[1].Value.DomainSize != 65536 || init.Index.DomainSize != 32768 || len(init.Samples) != 0 || site.Routing != "hle" || site.TargetSetStatus != "unproven" || len(site.StaticTargets) != 0 || r.EntryAblation.Summary.AuthoredHLEObligations != 1 {
		t.Fatal("caller-source facts changed domains, roots, routing, or HLE obligations")
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
		t.Fatalf("caller sources entered proven database: %v", err)
	}
	options.Jobs = 8
	parallel, err := AnalyzeAuthoredShadow(options)
	if err != nil || !reflect.DeepEqual(r, parallel) {
		t.Fatalf("worker-dependent report: %v", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, r, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"caller-source (report-only)", "caller table index (distinct from argument/callee index)", "one_caller_source_hop_no_recursive_expansion", "no_sample_or_writer_value_substitution"} {
		if !strings.Contains(text.String(), want) {
			t.Fatalf("missing %q", want)
		}
	}
}

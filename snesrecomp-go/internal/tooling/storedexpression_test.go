package tooling

import (
	"bytes"
	"os"
	"reflect"
	"strings"
	"testing"
)

func storedExpressionWriter(t *testing.T, code []byte) ShadowStoredTargetWriter {
	t.Helper()
	code = append(append([]byte(nil), code...), 0x9d, 0x40, 0, 0x60)
	_, writes := storedTargetGraph(t, code)
	for _, write := range writes {
		if write.field.Operand == 0x40 && write.writer.StoreMX.M == 0 && write.writer.StoreMX.X == 0 {
			return write.writer
		}
	}
	t.Fatal("word writer not decoded")
	return ShadowStoredTargetWriter{}
}

func TestStoredExpressionBinaryArithmetic(t *testing.T) {
	for _, test := range []struct {
		name   string
		code   []byte
		want   uint16
		addend int
	}{
		{"ADC clear carry", []byte{0xd8, 0x18, 0xa9, 0, 0x85, 0x69, 0x0c, 0}, 0x850c, 12},
		{"ADC set carry", []byte{0xd8, 0x38, 0xa9, 0, 0x85, 0x69, 0x0c, 0}, 0x850d, 13},
		{"SBC no borrow", []byte{0xd8, 0x38, 0xa9, 0, 0x85, 0xe9, 0x0c, 0}, 0x84f4, -12},
		{"SBC borrow", []byte{0xd8, 0x18, 0xa9, 0, 0x85, 0xe9, 0x0c, 0}, 0x84f3, -13},
		{"REP status bits", []byte{0xc2, 9, 0xa9, 0, 0x85, 0x69, 0x0c, 0}, 0x850c, 12},
		{"16-bit wrapping", []byte{0xd8, 0x18, 0xa9, 0xff, 0xff, 0x69, 2, 0}, 1, 2},
		{"ordered adjustments", []byte{0xd8, 0x38, 0xa9, 0, 0x85, 0x3a, 0xe9, 2, 0, 0x1a}, 0x84fe, -2},
		{"reset carry between operations", []byte{0xd8, 0xa9, 0, 0x85, 0x18, 0x69, 1, 0, 0x38, 0xe9, 3, 0}, 0x84fe, -2},
		{"register snapshot across store", []byte{0xd8, 0x18, 0xa9, 0, 0x85, 0x9d, 0x80, 0, 0x69, 0x0c, 0}, 0x850c, 12},
		{"CLD replaces SED", []byte{0xf8, 0xd8, 0x18, 0xa9, 0, 0x85, 0x69, 0x0c, 0}, 0x850c, 12},
		{"BIT preserves carry", []byte{0xd8, 0x18, 0xa9, 0, 0x85, 0x89, 1, 0, 0x69, 0x0c, 0}, 0x850c, 12},
	} {
		t.Run(test.name, func(t *testing.T) {
			writer := storedExpressionWriter(t, test.code)
			e := writer.Expression
			if e == nil || e.Status != "local_word_expression" || e.Constant == nil || *e.Constant != test.want || e.ExactAddend == nil || *e.ExactAddend != test.addend {
				t.Fatalf("expression = %+v", e)
			}
			if writer.StoredValue != nil || writer.Kind != "unknown_value" || writer.ValueAddend != 0 {
				t.Fatal("new local expression changed legacy candidate selection")
			}
		})
	}
}

func TestStoredExpressionNeverAssumesArithmeticStatus(t *testing.T) {
	for _, test := range []struct {
		name   string
		code   []byte
		reason string
	}{
		{"unknown decimal", []byte{0x18, 0xa9, 0, 0x85, 0x69, 1, 0}, "entry_boundary"},
		{"unknown carry", []byte{0xd8, 0xa9, 0, 0x85, 0x69, 1, 0}, "entry_boundary"},
		{"SED", []byte{0xf8, 0x18, 0xa9, 0, 0x85, 0x69, 1, 0}, ""},
		{"SEP decimal", []byte{0xe2, 9, 0xa9, 0, 0x85, 0x69, 1, 0}, ""},
		{"comparison clobbers carry", []byte{0xd8, 0x18, 0xa9, 0, 0x85, 0xc9, 0, 0, 0x69, 1, 0}, "carry_clobber_CMP"},
		{"shift clobbers carry", []byte{0xd8, 0x18, 0x06, 0x80, 0xa9, 0, 0x85, 0x69, 1, 0}, "carry_clobber_ASL"},
		{"earlier ADC clobbers carry", []byte{0xd8, 0x18, 0xa9, 0, 0x85, 0x69, 1, 0, 0x69, 2, 0}, "carry_clobber_ADC"},
		{"call clobbers status", []byte{0xd8, 0x18, 0x20, 0, 0x90, 0xa9, 0, 0x85, 0x69, 1, 0}, "status_barrier_JSR"},
		{"PLP restores unknown status", []byte{0x08, 0xd8, 0x18, 0x28, 0xa9, 0, 0x85, 0x69, 1, 0}, "status_barrier_PLP"},
	} {
		t.Run(test.name, func(t *testing.T) {
			e := storedExpressionWriter(t, test.code).Expression
			if e == nil || e.Status != "conditional_arithmetic" || e.Constant != nil || e.ExactAddend != nil {
				t.Fatalf("unproven arithmetic folded: %+v", e)
			}
			if !strings.Contains(shadowStoredJSONKey(e), test.reason) {
				t.Fatalf("lost reason %q: %+v", test.reason, e)
			}
		})
	}
}

func TestStoredExpressionRegistersAndClobbers(t *testing.T) {
	// The source is the Y value after JSR, not proof that JSR produced it or
	// preserved its incoming value. Both the pointer copy and offset stay open.
	for _, suffix := range [][]byte{{}, {0x18, 0x69, 7, 0}} {
		code := append([]byte{0x20, 0, 0x90, 0xb9, 0x0a, 0, 0x9d, 0x80, 0, 0x98}, suffix...)
		e := storedExpressionWriter(t, code).Expression
		if e == nil || e.Source.Kind != "register_after_call" || e.Source.Register != "Y" || e.Source.PC != 0x8000 || e.ExactAddend != nil {
			t.Fatalf("invented callee register value: %+v", e)
		}
	}
	for _, code := range [][]byte{
		{0xd8, 0x18, 0xa9, 0, 0x85, 0xe2, 0x10, 0xaa, 0xc2, 0x10, 0x8a, 0x69, 1, 0},
		{0xd8, 0x18, 0xa9, 0, 0x85, 0x29, 0xff, 0, 0x69, 1, 0},
		{0xd8, 0x18, 0xa9, 0, 0x85, 0x6d, 0, 0x90},
		{0xd8, 0x18, 0xa9, 0, 0x85, 0x24, 0x80, 0xd0, 2, 0xea, 0xea, 0x69, 1, 0}, // unknown BIT -> ambiguous join
	} {
		e := storedExpressionWriter(t, code).Expression
		if e == nil || e.Status != "unresolved_origin" || e.Constant != nil || e.ExactAddend != nil {
			t.Fatalf("unsupported/truncated value became finite: %+v", e)
		}
	}
}

func TestStoredExpressionFieldLinksAreNotTargetProof(t *testing.T) {
	options, _ := storedTargetFixture(t, []byte{0xa9, 0, 0x85, 0x9d, 0x60, 0, 0x60})
	image, err := os.ReadFile(options.ROMPath)
	if err != nil {
		t.Fatal(err)
	}
	// A proven binary adjustment still cannot prove that another routine's
	// identical addressing expression names this allocation/index/lifetime.
	copy(image[0x200:], []byte{0xd8, 0x18, 0xbd, 0x60, 0, 0x69, 5, 0, 0x9d, 0x40, 0, 0x60})
	if err := os.WriteFile(options.ROMPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := AnalyzeAuthoredShadow(options)
	if err != nil {
		t.Fatal(err)
	}
	flow := inventorySite(t, report, 0x8105).StoredTargetFlows[0]
	if len(flow.Targets) != 0 || len(flow.SourceFields) != 1 || flow.SourceFields[0].Field.Operand != 0x60 || len(flow.SourceFields[0].Writers) != 1 {
		t.Fatalf("source substitution manufactured a target: %+v", flow)
	}
	e := flow.Writers[0].Expression
	if e == nil || e.ExactAddend == nil || *e.ExactAddend != 5 || e.Constant != nil {
		t.Fatal(e)
	}
	db, err := BuildStaticAnalysisDatabase(report)
	if err != nil {
		t.Fatal(err)
	}
	without := report
	without.DispatchSites = nil
	without.DispatchSummary = ShadowDispatchSummary{}
	dbWithout, err := BuildStaticAnalysisDatabase(without)
	if err != nil || !reflect.DeepEqual(db, dbWithout) {
		t.Fatal("report-only expressions changed fact selection", err)
	}
	if report.EntryAblation.Summary.AuthoredHLEObligations != 1 {
		t.Fatal("HLE obligation lost")
	}
	options.Jobs = 1
	serial, err := AnalyzeAuthoredShadow(options)
	if err != nil || !reflect.DeepEqual(serial, report) {
		t.Fatal("non-deterministic report", err)
	}
	var out bytes.Buffer
	if err := WriteShadowReport(&out, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"source-field (conditional alias)", "value-expression (report-only)", "carry=0@", "decimal=0@", "exact-local-word-addend=5"} {
		if !strings.Contains(out.String(), want) {
			t.Fatalf("missing %q", want)
		}
	}
}

func TestStoredFieldDependencyDepthAndCycles(t *testing.T) {
	field := func(offset uint32) ShadowStoredField { return ShadowStoredField{Mode: "abs,x", Operand: offset} }
	for _, cycle := range []bool{false, true} {
		writes := make(map[shadowStoredFieldKey]map[string]ShadowStoredTargetWriter)
		for i := uint32(0); i < 4; i++ {
			next := field((i + 1) * 0x20)
			if cycle && i == 2 {
				next = field(0)
			}
			writes[shadowStoredFieldKey{0, field(i * 0x20)}] = map[string]ShadowStoredTargetWriter{"writer": {StorePC: 0x8100 + i*0x10, SourceField: &next}}
		}
		first, truncated := shadowStoredSourceFields(0, field(0), writes)
		if len(first) != shadowStoredSourceDepth || truncated == cycle {
			t.Fatalf("sources=%+v truncated=%t", first, truncated)
		}
		for i := range 20 {
			next, again := shadowStoredSourceFields(0, field(0), writes)
			if !reflect.DeepEqual(first, next) || again != truncated {
				t.Fatalf("non-deterministic expansion %d", i)
			}
		}
	}
}

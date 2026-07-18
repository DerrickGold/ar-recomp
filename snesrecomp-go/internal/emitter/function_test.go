package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func emitTestFunction(t *testing.T, data []byte, options FunctionOptions) string {
	t.Helper()
	image := make(rom.Image, 0x8000)
	copy(image, data)
	result, err := EmitFunction(image, 0, 0x8000, 1, 1, options)
	if err != nil {
		t.Fatalf("EmitFunction: %v", err)
	}
	return result.Source
}

func TestLinearFunction(t *testing.T) {
	source := emitTestFunction(t, []byte{0xA9, 0x05, 0x85, 0x00, 0x60}, FunctionOptions{Name: "Linear"})
	for _, fragment := range []string{
		"RecompReturn Linear_M1X1(CpuState *cpu)",
		"L_8000_M1X1:",
		"uint8 _v1 = 0x5;",
		"cpu_write8",
		"/* RTS host return */",
	} {
		if !strings.Contains(source, fragment) {
			t.Errorf("source is missing %q:\n%s", fragment, source)
		}
	}
}

func TestConditionalBranchLabelsBothEdges(t *testing.T) {
	source := emitTestFunction(t, []byte{
		0xF0, 0x04,
		0xA9, 0x01,
		0x80, 0x02,
		0xA9, 0x02,
		0x60,
	}, FunctionOptions{Name: "Diamond"})
	if !strings.Contains(source, "if (cpu->_flag_Z == 1) { goto L_8006_M1X1; }") {
		t.Errorf("missing taken branch:\n%s", source)
	}
	if !strings.Contains(source, "goto L_8002_M1X1; /* fall-through */") {
		t.Errorf("missing fall branch:\n%s", source)
	}
}

func TestCallDemandIsCollectedPerJob(t *testing.T) {
	context := codegen.NewContext()
	source := emitTestFunction(t, []byte{0x20, 0x06, 0x80, 0x60, 0xEA, 0xEA, 0x60}, FunctionOptions{Name: "Caller", Codegen: context})
	if !strings.Contains(source, "bank_00_8006_M1X1") {
		t.Fatalf("missing emitted callee dispatch:\n%s", source)
	}
	if len(context.Demands) != 4 {
		t.Fatalf("call demands = %d, want four runtime M/X variants", len(context.Demands))
	}
}

func TestSplitImmediateGarbageTrapRequiresSurvivingSibling(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xA9, 0x07, 0x00, 0xEA, 0x60}) // M0: LDA #$0007; M1: LDA #$07; BRK
	end := uint16(0x8005)

	withoutSurvivors, err := EmitFunction(image, 0, 0x8000, 1, 1, FunctionOptions{Name: "Split", End: &end})
	if err != nil {
		t.Fatalf("EmitFunction without survivors: %v", err)
	}
	if strings.Contains(withoutSurvivors.Source, "ar_garbage_variant_trap") {
		t.Fatalf("trap emitted before survivor map was established:\n%s", withoutSurvivors.Source)
	}
	if len(withoutSurvivors.GarbageEvidence) == 0 {
		t.Fatal("split-immediate evidence was not collected")
	}

	context := codegen.NewContext()
	context.ValidVariants[0x008000] = map[[2]uint8]struct{}{{0, 1}: {}, {1, 1}: {}}
	withSurvivor, err := EmitFunction(image, 0, 0x8000, 1, 1, FunctionOptions{Name: "Split", End: &end, Codegen: context})
	if err != nil {
		t.Fatalf("EmitFunction with survivor: %v", err)
	}
	if !strings.Contains(withSurvivor.Source, "ar_garbage_variant_trap(cpu, \"Split_M1X1\"") {
		t.Fatalf("missing split-immediate trap:\n%s", withSurvivor.Source)
	}
}

func TestEquivalentVariantCoverageIsCollected(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xA2, 0x05, 0x60}) // X width changes; M does not.
	result, err := EmitFunction(image, 0, 0x8000, 0, 1, FunctionOptions{Name: "Equivalent"})
	if err != nil {
		t.Fatalf("EmitFunction: %v", err)
	}
	want := VariantEquivalence{Address: 0x008000, From: [2]uint8{0, 1}, To: [2]uint8{1, 1}}
	for _, got := range result.Equivalences {
		if got == want {
			return
		}
	}
	t.Fatalf("missing M-only equivalence %#v in %#v", want, result.Equivalences)
}

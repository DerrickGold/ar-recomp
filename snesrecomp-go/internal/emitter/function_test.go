package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
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

func TestConditionalHLEKeepsNativeFallback(t *testing.T) {
	source := emitTestFunction(t, []byte{0xA9, 0x05, 0x60}, FunctionOptions{
		Name: "Conditional",
		HLEFunctionIf: config.HLEFunctionIf{
			Function: "HostConditional", Predicate: "HostConditionalEnabled",
		},
	})
	for _, fragment := range []string{
		"if (HostConditionalEnabled(cpu))",
		"RecompReturn _r = HostConditional(cpu);",
		"L_8000_M1X1:",
		"uint8 _v1 = 0x5;",
	} {
		if !strings.Contains(source, fragment) {
			t.Errorf("conditional HLE source is missing %q:\n%s", fragment, source)
		}
	}
}

func TestLinearFunction(t *testing.T) {
	source := emitTestFunction(t, []byte{0xA9, 0x05, 0x85, 0x00, 0x60}, FunctionOptions{Name: "Linear"})
	for _, fragment := range []string{
		"RecompReturn Linear_M1X1(CpuState *cpu)",
		"L_8000_M1X1:",
		"uint8 _v1 = 0x5;",
		"cpu_write8",
		"cpu_trace_resolved_dispatch(cpu, _rpc24, 0x008004u);",
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
	if strings.Contains(withoutSurvivors.Source, "sr_garbage_variant_trap") {
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
	if !strings.Contains(withSurvivor.Source, "sr_garbage_variant_trap(cpu, \"Split_M1X1\"") {
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

func TestUnresolvedIndirectJumpUsesItsOwnDiagnostic(t *testing.T) {
	// JMP ($1234) has no statically declared target set in this fixture.
	source := emitTestFunction(t, []byte{0x6c, 0x34, 0x12},
		FunctionOptions{Name: "Indirect", UnresolvedAllowed: true})
	for _, fragment := range []string{
		"uint16 _trap_address = cpu_read16(cpu, 0x00, (uint16)0x1234u);",
		"uint32 _trap_target = ((uint32)cpu->PB << 16) | (uint32)_trap_address;",
		"cpu_trace_trapped_dispatch(cpu, _trap_target, 0x008000u);",
		"cpu_trace_unresolved_indirect_jump(cpu, 0x008000)",
	} {
		if !strings.Contains(source, fragment) {
			t.Fatalf("unresolved JMP (abs) is missing %q:\n%s", fragment, source)
		}
	}
	if strings.Contains(source, "0xFFFF") ||
		strings.Contains(source, "cpu_trace_dispatch_oob") {
		t.Fatalf("unresolved indirect jump still masquerades as dispatch OOB:\n%s",
			source)
	}
}

func TestUnresolvedIndexedIndirectJumpCensusUsesLiveX(t *testing.T) {
	source := emitTestFunction(t, []byte{0x7c, 0x34, 0x12},
		FunctionOptions{Name: "IndirectX", UnresolvedAllowed: true})
	for _, fragment := range []string{
		"uint16 _trap_pointer = (uint16)(0x1234u + cpu->X);",
		"uint16 _trap_address = cpu_read16(cpu, cpu->PB, _trap_pointer);",
		"uint32 _trap_target = ((uint32)cpu->PB << 16) | (uint32)_trap_address;",
		"cpu_trace_trapped_dispatch(cpu, _trap_target, 0x008000u);",
	} {
		if !strings.Contains(source, fragment) {
			t.Fatalf("unresolved JMP (abs,X) is missing %q:\n%s", fragment, source)
		}
	}
}

func TestUnresolvedLongIndirectJumpCensusReadsBankZeroPointer(t *testing.T) {
	source := emitTestFunction(t, []byte{0xdc, 0x34, 0x12},
		FunctionOptions{Name: "IndirectLong", UnresolvedAllowed: true})
	for _, fragment := range []string{
		"uint16 _trap_pointer = (uint16)0x1234u;",
		"uint16 _trap_address = cpu_read16(cpu, 0x00, _trap_pointer);",
		"uint8 _trap_bank = cpu_read8(cpu, 0x00, (uint16)(_trap_pointer + 2u));",
		"uint32 _trap_target = ((uint32)_trap_bank << 16) | (uint32)_trap_address;",
		"cpu_trace_trapped_dispatch(cpu, _trap_target, 0x008000u);",
	} {
		if !strings.Contains(source, fragment) {
			t.Fatalf("unresolved JML [abs] is missing %q:\n%s", fragment, source)
		}
	}
}

func TestConfiguredAbsoluteIndirectDispatchUsesArchitecturalPointerBanks(t *testing.T) {
	context := codegen.NewContext()
	jump := &cpu65816.Instruction{
		Address: 0x058000, Opcode: 0x6c, Mnemonic: "JMP", Mode: cpu65816.INDIR,
		Operand: 0x0098, DispatchEntries: []uint32{0x05c123},
		DispatchTableBase: []uint16{0x0098}, DispatchIndexReg: "X",
	}
	jumpSource := strings.Join(emitIndexedIndirectDispatch(context, jump, nil), "\n")
	if !strings.Contains(jumpSource, "uint16 _target = cpu_read16(cpu, 0x00, (uint16)0x0098);") {
		t.Fatalf("configured JMP (abs) did not read its pointer from bank zero:\n%s", jumpSource)
	}

	longJump := &cpu65816.Instruction{
		Address: 0x038000, Opcode: 0xdc, Mnemonic: "JMP", Mode: cpu65816.INDIR,
		Operand: 0x1234, DispatchKind: "long", DispatchEntries: []uint32{0x12a000},
		DispatchTableBase: []uint16{0x1234}, DispatchIndexReg: "X",
	}
	longSource := strings.Join(emitIndexedIndirectDispatch(context, longJump, nil), "\n")
	for _, fragment := range []string{
		"uint16 _target_address = cpu_read16(cpu, 0x00, _pointer);",
		"uint8 _target_bank = cpu_read8(cpu, 0x00, (uint16)(_pointer + 2u));",
		"uint32 _target = ((uint32)_target_bank << 16) | (uint32)_target_address;",
		"case 0x12a000:",
	} {
		if !strings.Contains(longSource, fragment) {
			t.Fatalf("configured JML [abs] is missing %q:\n%s", fragment, longSource)
		}
	}
}

func TestCfgDispatchInvalidTargetStillCreatesEmittedDemand(t *testing.T) {
	context := codegen.NewContext()
	instruction := &cpu65816.Instruction{
		Address:         0x008000,
		DispatchKind:    "long",
		DispatchEntries: []uint32{0x001234},
	}
	source := strings.Join(emitJSLDispatch(context, instruction), "\n")
	want := codegen.Variant{Address: 0x001234, M: 1, X: 1}
	if _, found := context.Demands[want]; !found {
		t.Fatalf("cfg dispatch target did not create emitted demand: %#v", want)
	}
	if !strings.Contains(source, "bank_00_1234_M1X1(cpu)") {
		t.Fatalf("cfg dispatch target call was not emitted:\n%s", source)
	}
}

func TestInvalidCrossBankLongJumpUsesLoudTrap(t *testing.T) {
	// JML $00:1234 is outside the supported static LoROM code window.
	source := emitTestFunction(t, []byte{0x5c, 0x34, 0x12, 0x00},
		FunctionOptions{Name: "LongJump", UnresolvedAllowed: true})
	if !strings.Contains(source,
		"cpu_trace_unresolved_stub_trap(cpu, 0x001234") {
		t.Fatalf("invalid cross-bank JML remained silent:\n%s", source)
	}
}

func TestCrossBankLongJumpUsesPostPLPLiveWidths(t *testing.T) {
	image := make(rom.Image, 0x10000)
	copy(image, []byte{
		0x08,       // PHP: save entry M0X0
		0xe2, 0x30, // SEP #$30: block is now M1X1
		0x80, 0x00, // BRA to a new block at $8005
		0x28,                   // PLP: restore M0X0
		0x5c, 0x00, 0x80, 0x01, // JML $01:8000
	})
	image[0x8000] = 0x60 // valid target in LoROM bank $01
	context := codegen.NewContext()
	result, err := EmitFunction(image, 0, 0x8000, 0, 0, FunctionOptions{
		Name: "RestoreThenJump", Codegen: context, UnresolvedAllowed: true,
	})
	if err != nil {
		t.Fatalf("EmitFunction: %v", err)
	}
	if !strings.Contains(result.Source, "bank_01_8000_M0X0(cpu)") ||
		strings.Contains(result.Source, "bank_01_8000_M1X1(cpu)") {
		t.Fatalf("cross-bank JML did not use post-PLP M/X:\n%s", result.Source)
	}
	if _, found := context.Demands[codegen.Variant{
		Address: 0x018000, M: 0, X: 0,
	}]; !found {
		t.Fatalf("post-PLP JML demand = %+v", context.Demands)
	}
}

package tooling

import (
	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// A concrete ROM word read on a decoded input path. The value is conditional
// on that path and cartridge ROM ownership, not a closed input set or a root.
type ShadowCommandInputRead struct {
	Context       analysis.EntryVariant  `json:"context"`
	LoadPC        uint32                 `json:"load_pc"`
	LoadMX        analysis.MXState       `json:"load_mx"`
	Mode          string                 `json:"mode"`
	Operand       uint32                 `json:"operand"`
	IndexRegister string                 `json:"index_register,omitempty"`
	Index         uint16                 `json:"index"`
	DataBank      ShadowRegisterEvidence `json:"data_bank"`
	ReadPC        uint32                 `json:"read_pc"`
	Word          uint16                 `json:"word"`
}

// Private recipe retained before graphs are discarded. Nested indexed reads
// and exact local WRAM stores follow one decoded predecessor chain.
// Exported fields allow the deterministic input key to include the recipe.
type shadowStreamInputRead struct {
	LoadPC        uint32
	LoadMX        analysis.MXState
	Mode          string
	Operand       uint32
	DataBank      ShadowRegisterEvidence
	IndexRegister string
	Index         *ShadowInitializerIndex
	InputRead     *shadowStreamInputRead
	Issue         string
	Local         *shadowStreamLocalRead
}

const shadowStreamInputReadDepth = 4

func collectShadowStreamInputRead(walk shadowPointerWalk, expr ShadowInitializerIndex, depth int) *shadowStreamInputRead {
	if expr.Source.Kind != "load" || expr.Source.Mode == "imm" {
		return nil
	}
	d := walk.graph.Instructions[expr.sourceKey]
	if d == nil || d.Instruction == nil {
		return nil
	}
	i := d.Instruction
	if depth == shadowStreamInputReadDepth {
		return &shadowStreamInputRead{Issue: "ROM_input_read_depth_limit"}
	}
	if local, attempted := collectShadowStreamLocalRead(walk, expr, depth); attempted {
		return local
	}
	index := ""
	switch i.Mode {
	case cpu65816.ABS, cpu65816.LONG:
	case cpu65816.ABSX, cpu65816.LONGX:
		index = "X"
	case cpu65816.ABSY:
		index = "Y"
	default:
		return nil // No DP, indirect pointers, or mutable slot substitution.
	}
	r := &shadowStreamInputRead{LoadPC: d.Key.PC, LoadMX: analysis.MXState{M: d.Key.M, X: d.Key.X}, Mode: i.Mode.String(), Operand: i.Operand, IndexRegister: index}
	if !shadowPointerWord(d.Key, expr.Source.Register) || (index != "" && !shadowPointerWord(d.Key, index)) {
		r.Issue = "ROM_input_read_width_unproven"
		return r
	}
	if i.Mode == cpu65816.LONG || i.Mode == cpu65816.LONGX {
		r.DataBank.Constants = []ShadowRegisterConstant{{Value: uint16(i.Operand >> 16), DefinitionPC: d.Key.PC}}
	} else {
		// Inputs can arrive through a mapper-equivalent program-bank mirror.
		// PHK is live PB, not a literal DB; don't bake in the decoded owner.
		r.DataBank = walk.initializerBankFrom(d.Key, false)
	}
	if _, ok := shadowStreamConstantBank(r.DataBank); !ok {
		r.Issue = "ROM_input_bank_unproven"
		return r
	}
	if index != "" {
		value := walk.indexExpression(d.Key, index, true)
		value.LocalSource = nil
		r.Index = &value
		r.InputRead = collectShadowStreamInputRead(walk, value, depth+1)
	}
	return r
}

func resolveShadowStreamInputRead(image romimage.Image, ctx analysis.EntryVariant, read *shadowStreamInputRead, index uint16) (ShadowCommandInputRead, string) {
	bank, ok := shadowStreamConstantBank(read.DataBank)
	if !ok {
		return ShadowCommandInputRead{}, "ROM_input_bank_unproven"
	}
	address := uint32(uint16(read.Operand)) + uint32(index)
	// As with pointer reads, reject all bank crossings rather than conflate
	// absolute indexed carry, long addressing, and physical ROM adjacency.
	if address > 0xfffe {
		return ShadowCommandInputRead{}, "ROM_input_bank_boundary"
	}
	word, ok := shadowStreamROMWord(image, bank, address)
	if !ok {
		return ShadowCommandInputRead{}, "input_not_ROM_mapped"
	}
	return ShadowCommandInputRead{Context: ctx, LoadPC: read.LoadPC, LoadMX: read.LoadMX, Mode: read.Mode, Operand: read.Operand, IndexRegister: read.IndexRegister, Index: index, DataBank: read.DataBank, ReadPC: uint32(bank)<<16 | address, Word: word}, ""
}

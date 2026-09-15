package tooling

import (
	"slices"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// An exact-address local memory dependency, conditional on this decoded path
// and no asynchronous/HLE writes. Not a global object-field alias or lifetime.
type ShadowCommandLocalInput struct {
	Context        analysis.EntryVariant        `json:"context"`
	LoadPC         uint32                       `json:"load_pc"`
	LoadMX         analysis.MXState             `json:"load_mx"`
	StorePC        uint32                       `json:"store_pc"`
	StoreMX        analysis.MXState             `json:"store_mx"`
	LoadAddress    ShadowCommandWRAMAddress     `json:"load_address"`
	StoreAddress   ShadowCommandWRAMAddress     `json:"store_address"`
	DisjointWrites []ShadowCommandDisjointWrite `json:"disjoint_writes,omitempty"`
	Word           uint16                       `json:"word"`
	Obligations    []string                     `json:"proof_obligations"`
}

type ShadowCommandWRAMAddress struct {
	Mode          string                  `json:"mode"`
	Operand       uint32                  `json:"operand"`
	IndexRegister string                  `json:"index_register,omitempty"`
	Index         *ShadowInitializerIndex `json:"index_expression,omitempty"`
	Base          ShadowRegisterEvidence  `json:"D_or_DB_evidence"`
	BusPC         uint32                  `json:"bus_address"`
	WRAMOffset    uint32                  `json:"wram_offset"`
	Width         int                     `json:"width_bytes"`
}

type ShadowCommandDisjointWrite struct {
	PC      uint32                   `json:"pc"`
	Address ShadowCommandWRAMAddress `json:"address"`
}

type shadowStreamLocalRead struct {
	Evidence  ShadowCommandLocalInput
	Value     ShadowInitializerIndex
	InputRead *shadowStreamInputRead
	Zero      bool
}

const shadowStreamLocalStoreLimit = 64

// The external entry is an additional predecessor even when the decoded graph
// has a loop backedge there. Never use a later iteration's definition for the
// first entry into this body.
func shadowStreamLocalPrecedes(walk shadowPointerWalk, at decoder.DecodeKey, pc uint32) bool {
	for range shadowStreamLocalStoreLimit {
		if at == walk.graph.Entry {
			return false
		}
		prev := walk.previous(at)
		if prev == nil {
			return false
		}
		at = prev.Key
		if at.PC == pc {
			return true
		}
	}
	return false
}

func shadowStreamConstantWord(e ShadowRegisterEvidence) (uint16, bool) {
	if e.UnknownPaths || len(e.Constants) == 0 {
		return 0, false
	}
	v := e.Constants[0].Value
	for _, c := range e.Constants {
		if c.Value != v {
			return 0, false
		}
	}
	return v, true
}

// Exact locally literal indices only. In particular, never form a Cartesian
// product of index arguments and stored-value arguments from different calls.
func shadowStreamWRAMAddress(walk shadowPointerWalk, key decoder.DecodeKey, width int) (ShadowCommandWRAMAddress, string) {
	d := walk.graph.Instructions[key]
	if d == nil || d.Instruction == nil || key.X != 0 || width < 1 || width > 2 {
		return ShadowCommandWRAMAddress{}, "local_input_width_unproven"
	}
	i := d.Instruction
	r := ShadowCommandWRAMAddress{Mode: i.Mode.String(), Operand: i.Operand, Width: width}
	var bank byte
	address := uint32(uint16(i.Operand))
	switch i.Mode {
	case cpu65816.DP, cpu65816.DPX, cpu65816.DPY:
		r.Base = walk.bankOrD(key, "D")
		base, ok := shadowStreamConstantWord(r.Base)
		if !ok {
			return r, "local_input_D_unproven"
		}
		address += uint32(base)
	case cpu65816.ABS, cpu65816.ABSX, cpu65816.ABSY:
		r.Base = walk.initializerBankFrom(key, false)
		b, ok := shadowStreamConstantBank(r.Base)
		if !ok {
			return r, "local_input_DB_unproven"
		}
		bank = b
	case cpu65816.LONG, cpu65816.LONGX:
		bank = byte(i.Operand >> 16)
		r.Base.Constants = []ShadowRegisterConstant{{Value: uint16(bank), DefinitionPC: key.PC}}
	default:
		return r, "local_input_address_mode_unproven"
	}
	if i.Mode != cpu65816.LONG && i.Mode != cpu65816.LONGX {
		for _, c := range r.Base.Constants {
			if !shadowStreamLocalPrecedes(walk, key, c.DefinitionPC) {
				return r, "local_input_base_crosses_entry_or_join"
			}
		}
	}
	switch i.Mode {
	case cpu65816.DPX, cpu65816.ABSX, cpu65816.LONGX:
		r.IndexRegister = "X"
	case cpu65816.DPY, cpu65816.ABSY:
		r.IndexRegister = "Y"
	}
	if r.IndexRegister != "" {
		index := walk.indexExpression(key, r.IndexRegister, true)
		if index.Source.Kind != "load" || index.Source.Mode != "imm" || index.KnownZero|index.KnownOne != 0xffff {
			return r, "local_input_index_unproven"
		}
		if !shadowStreamLocalPrecedes(walk, key, index.Source.PC) {
			return r, "local_input_index_crosses_entry_or_join"
		}
		r.Index = &index
		address += uint32(index.KnownOne)
	}
	if address+uint32(width) > 0x10000 {
		return r, "local_input_bank_boundary"
	}
	r.BusPC = uint32(bank)<<16 | address
	switch {
	case bank == 0x7e || bank == 0x7f:
		r.WRAMOffset = uint32(bank-0x7e)<<16 | address
	case (bank <= 0x3f || bank >= 0x80 && bank <= 0xbf) && address+uint32(width) <= 0x2000:
		r.WRAMOffset = address
	default:
		return r, "local_input_not_WRAM"
	}
	return r, ""
}

func collectShadowStreamLocalRead(walk shadowPointerWalk, expr ShadowInitializerIndex, depth int) (*shadowStreamInputRead, bool) {
	d := walk.graph.Instructions[expr.sourceKey]
	if d == nil || d.Instruction == nil || !shadowPointerWord(d.Key, expr.Source.Register) {
		return nil, false
	}
	address, reason := shadowStreamWRAMAddress(walk, d.Key, 2)
	if reason != "" {
		// DP has no ROM-input recipe. Explain the object-address barrier rather
		// than attach all same-spelling field writers as substitute values.
		fullWRAM := (d.Instruction.Mode == cpu65816.LONG || d.Instruction.Mode == cpu65816.LONGX) && (byte(d.Instruction.Operand>>16) == 0x7e || byte(d.Instruction.Operand>>16) == 0x7f)
		if d.Instruction.Mode == cpu65816.DP || d.Instruction.Mode == cpu65816.DPX || d.Instruction.Mode == cpu65816.DPY || fullWRAM {
			return &shadowStreamInputRead{Issue: reason}, true
		}
		return nil, false
	}
	r := &shadowStreamInputRead{}
	local := &shadowStreamLocalRead{Evidence: ShadowCommandLocalInput{Context: analysis.EntryVariant{PC: walk.graph.Entry.PC, EntryMX: analysis.MXState{M: walk.graph.Entry.M, X: walk.graph.Entry.X}}, LoadPC: d.Key.PC, LoadMX: analysis.MXState{M: d.Key.M, X: d.Key.X}, LoadAddress: address, Obligations: []string{"decoded_local_path_not_reachability", "no_interrupt_HLE_or_external_memory_interference", "no_cross_call_or_object_lifetime_join", "no_callback_root_or_closed_target_promotion"}}}
	at := d.Key
	seen := make(map[decoder.DecodeKey]bool)
	for range shadowStreamLocalStoreLimit {
		if at == walk.graph.Entry {
			r.Issue = "local_input_entry_or_ambiguous_predecessor"
			return r, true
		}
		prev := walk.previous(at)
		if prev == nil || prev.Instruction == nil {
			r.Issue = "local_input_entry_or_ambiguous_predecessor"
			return r, true
		}
		at = prev.Key
		if seen[at] {
			r.Issue = "local_input_predecessor_cycle"
			return r, true
		}
		seen[at] = true
		i := prev.Instruction
		write := false
		switch i.Mnemonic {
		case "STA", "STX", "STY", "STZ", "TRB", "TSB":
			write = true
		case "INC", "DEC", "ASL", "LSR", "ROL", "ROR":
			write = i.Mode != cpu65816.ACC
		}
		if write {
			width := 2 - int(at.M)
			if i.Mnemonic == "STX" || i.Mnemonic == "STY" {
				width = 2 - int(at.X)
			}
			store, reason := shadowStreamWRAMAddress(walk, at, width)
			if reason != "" {
				r.Issue = "local_input_possible_alias_write:" + reason
				return r, true
			}
			if store.WRAMOffset < address.WRAMOffset+2 && address.WRAMOffset < store.WRAMOffset+uint32(width) {
				if width != 2 || store.WRAMOffset != address.WRAMOffset {
					r.Issue = "local_input_partial_alias_write"
					return r, true
				}
				reg := shadowPointerRegister(i.Mnemonic)
				if reg == "" && i.Mnemonic != "STZ" {
					r.Issue = "local_input_RMW_alias_write"
					return r, true
				}
				local.Evidence.StorePC = at.PC
				local.Evidence.StoreMX = analysis.MXState{M: at.M, X: at.X}
				local.Evidence.StoreAddress = store
				slices.Reverse(local.Evidence.DisjointWrites)
				local.Zero = i.Mnemonic == "STZ"
				if !local.Zero {
					local.Value = walk.indexExpression(at, reg, true)
					if local.Value.Source.Kind != "entry_register" && !shadowStreamLocalPrecedes(walk, at, local.Value.Source.PC) {
						r.Issue = "local_input_value_crosses_entry_or_join"
						return r, true
					}
					local.InputRead = collectShadowStreamInputRead(walk, local.Value, depth+1)
				}
				r.Local = local
				return r, true
			}
			local.Evidence.DisjointWrites = append(local.Evidence.DisjointWrites, ShadowCommandDisjointWrite{PC: at.PC, Address: store})
			continue
		}
		// No call, push/pull, DMA, or opaque instruction can be memory-neutral
		// merely because it preserves the register being used as an input.
		if shadowPointerTransparent(i) {
			continue
		}
		if _, dst := shadowPointerTransfer(i.Mnemonic); dst != "" {
			continue
		}
		switch i.Mnemonic {
		case "LDA", "LDX", "LDY", "ADC", "SBC", "AND", "ORA", "EOR", "INX", "INY", "DEX", "DEY", "XBA", "TCD":
			continue
		case "INC", "DEC", "ASL", "LSR", "ROL", "ROR":
			if i.Mode == cpu65816.ACC {
				continue
			}
		}
		r.Issue = "local_input_memory_barrier_" + i.Mnemonic
		return r, true
	}
	r.Issue = "local_input_store_budget"
	return r, true
}

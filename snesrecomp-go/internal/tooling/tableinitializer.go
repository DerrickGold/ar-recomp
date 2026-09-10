package tooling

import (
	"fmt"
	"io"
	"math/bits"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func writeShadowInitializer(out io.Writer, r *ShadowInitializerRead, indent string) {
	fmt.Fprintf(out, "%sload=%s M%dX%d mode=%s operand=$%X index=%s domain-superset=%d known-zero=$%04X known-one=$%04X bank-unknown=%t\n", indent, shadowAddress(r.LoadPC), r.LoadMX.M, r.LoadMX.X, r.Mode, r.Operand, r.IndexRegister, r.Index.DomainSize, r.Index.KnownZero, r.Index.KnownOne, r.DataBank.UnknownPaths)
	fmt.Fprintf(out, "%sindex-origin=%s %s %s $%X reason=%s conditional-source-writers=%d\n", indent, shadowAddress(r.Index.Source.PC), r.Index.Source.Kind, r.Index.Source.Mode, r.Index.Source.Operand, r.Index.Source.Reason, len(r.Index.Writers))
	writeShadowLocalSlot(out, r.Index.LocalSource, indent)
	for _, op := range r.Index.Operations {
		fmt.Fprintf(out, "%sindex-operation=%s %s $%04X\n", indent, shadowAddress(op.PC), op.Mnemonic, op.Operand)
	}
	for _, bank := range r.DataBank.Constants {
		fmt.Fprintf(out, "%sDB=$%02X defined=%s\n", indent, bank.Value, shadowAddress(bank.DefinitionPC))
	}
	for _, writer := range r.Index.Writers {
		writeShadowStoredWriter(out, writer)
	}
	for _, sample := range r.Samples {
		fmt.Fprintf(out, "%ssample index=$%04X DB=$%02X evidence=%s status=%s", indent, sample.Index, sample.Bank, sample.IndexEvidence, sample.Status)
		if sample.ReadPC != 0 {
			fmt.Fprintf(out, " read=%s", shadowAddress(sample.ReadPC))
		}
		if sample.Word != nil {
			fmt.Fprintf(out, " word=$%04X", *sample.Word)
		}
		fmt.Fprintln(out)
	}
	if r.SamplesTruncated {
		fmt.Fprintf(out, "%ssamples truncated at %d; not a completeness claim\n", indent, shadowPointerSampleLimit)
	}
	if r.PointerSource != nil {
		fmt.Fprintf(out, "%sconditional pointer-store=%s (alias/wrap unproven)\n", indent, shadowAddress(r.PointerStorePC))
		writeShadowInitializer(out, r.PointerSource, indent+"  ")
	}
	fmt.Fprintf(out, "%sobligations=%v\n", indent, r.Obligations)
}

// A table word feeding an identically spelled index/pointer field. This is
// report-only producer evidence, not a reaching definition or a handler root.
type ShadowTableInitializer struct {
	StorePC     uint32                  `json:"store_pc"`
	Contexts    []analysis.EntryVariant `json:"contexts"`
	ValueAddend int                     `json:"value_addend"`
	Read        ShadowInitializerRead   `json:"read"`
}

type ShadowInitializerRead struct {
	LoadPC           uint32                    `json:"load_pc"`
	LoadMX           analysis.MXState          `json:"load_mx"`
	Mode             string                    `json:"mode"`
	Operand          uint32                    `json:"operand"`
	IndexRegister    string                    `json:"index_register"`
	Index            ShadowInitializerIndex    `json:"index"`
	DataBank         ShadowRegisterEvidence    `json:"data_bank"`
	PointerStorePC   uint32                    `json:"pointer_store_pc,omitempty"`
	PointerSource    *ShadowInitializerRead    `json:"pointer_source,omitempty"`
	Samples          []ShadowInitializerSample `json:"rom_word_samples,omitempty"`
	SamplesTruncated bool                      `json:"samples_truncated,omitempty"`
	Obligations      []string                  `json:"proof_obligations"`
}

type ShadowInitializerIndex struct {
	Source       ShadowStoredOrigin         `json:"source"`
	Field        *ShadowStoredField         `json:"field,omitempty"`
	Operations   []ShadowStoredOperation    `json:"operations,omitempty"`
	KnownZero    uint16                     `json:"known_zero_bits"`
	KnownOne     uint16                     `json:"known_one_bits"`
	DomainSize   uint32                     `json:"local_domain_size"`
	DomainValues []uint16                   `json:"local_domain_values,omitempty"`
	Writers      []ShadowStoredTargetWriter `json:"conditional_source_writers,omitempty"`
	LocalSource  *ShadowLocalSlotSource     `json:"local_slot_source,omitempty"`
	sourceKey    decoder.DecodeKey
}

type ShadowInitializerSample struct {
	Index         uint16   `json:"index"`
	IndexEvidence string   `json:"index_evidence"`
	WriterPCs     []uint32 `json:"conditional_index_writer_pcs,omitempty"`
	Bank          byte     `json:"bank"`
	ReadPC        uint32   `json:"read_pc,omitempty"`
	Word          *uint16  `json:"word,omitempty"`
	Status        string   `json:"status"`
}

const shadowInitializerDomainLimit = 256
const shadowInitializerBankLimit = 256

// Bit constraints over a word are a local overapproximation, not a table span.
// No origin load's global range is guessed. Masks/shifts after an unknown
// origin can still establish a useful finite superset on this decoded path.
func (walk shadowPointerWalk) initializerIndex(at decoder.DecodeKey, reg string) ShadowInitializerIndex {
	return walk.indexExpression(at, reg, false)
}

// Argument queries may cross stack/bank operations that preserve the value
// register, but never pulls into that register or unknown status restoration.
// Keep this separate from the previously published initializer expression.
func (walk shadowPointerWalk) indexExpression(at decoder.DecodeKey, reg string, argument bool) ShadowInitializerIndex {
	result := ShadowInitializerIndex{}
	finish := func(kind, reason string, ins *cpu65816.Instruction) ShadowInitializerIndex {
		result.sourceKey = at
		result.Source = ShadowStoredOrigin{Kind: kind, PC: at.PC, Register: reg, Reason: reason}
		if ins != nil {
			result.Source.Mode, result.Source.Operand = ins.Mode.String(), ins.Operand
			if kind == "load" {
				if field, valid := shadowMemoryField(ins); valid {
					result.Field = &field
				}
				if ins.Mode == cpu65816.IMM {
					result.KnownOne = uint16(ins.Operand)
					result.KnownZero = ^result.KnownOne
				}
			}
		}
		slices.Reverse(result.Operations)
		for _, op := range result.Operations {
			result.KnownZero, result.KnownOne = initializerBits(result.KnownZero, result.KnownOne, op)
		}
		free := ^(result.KnownZero | result.KnownOne)
		result.DomainSize = uint32(1) << bits.OnesCount16(free)
		if result.DomainSize <= shadowInitializerDomainLimit {
			for subset := free; ; subset = (subset - 1) & free {
				result.DomainValues = append(result.DomainValues, result.KnownOne|subset)
				if subset == 0 {
					break
				}
			}
			slices.Sort(result.DomainValues)
		}
		return result
	}
	if argument && !shadowPointerWord(at, reg) {
		return finish("unknown", "byte_or_truncated_input", nil)
	}
	for range shadowPointerWalkLimit {
		previous := walk.previous(at)
		if previous == nil || previous.Instruction == nil {
			if argument && at == walk.graph.Entry && len(walk.preds[at]) == 0 && shadowPointerWord(at, reg) {
				return finish("entry_register", "entry_value_unproven", nil)
			}
			return finish("unknown", "entry_or_ambiguous_predecessor", nil)
		}
		at = previous.Key
		ins := previous.Instruction
		if !shadowPointerWord(at, reg) {
			return finish("unknown", "byte_or_truncated_origin", nil)
		}
		if ins.Mnemonic == "LD"+reg {
			return finish("load", "", ins)
		}
		if src, dst := shadowPointerTransfer(ins.Mnemonic); dst != "" {
			if dst == reg {
				if !shadowPointerWord(at, src) {
					return finish("unknown", "truncated_transfer", nil)
				}
				reg = src
			}
			continue
		}
		if reg == "A" && ((ins.Mode == cpu65816.ACC && (ins.Mnemonic == "ASL" || ins.Mnemonic == "LSR")) ||
			(ins.Mode == cpu65816.IMM && (ins.Mnemonic == "AND" || ins.Mnemonic == "ORA" || ins.Mnemonic == "EOR"))) {
			result.Operations = append(result.Operations, ShadowStoredOperation{PC: at.PC, Mnemonic: ins.Mnemonic, Operand: uint16(ins.Operand)})
			continue
		}
		switch ins.Mnemonic {
		case "JSR", "JSL":
			return finish("register_after_call", "callee_value_unproven", nil)
		case "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ":
			continue
		}
		if argument {
			switch ins.Mnemonic {
			case "PHB", "PHK", "PLB", "PHD", "PLD", "PHP", "PHA", "PHX", "PHY", "PEA", "PEI", "PER":
				continue
			}
		}
		if shadowPointerTransparent(ins) {
			continue
		}
		if reg != "A" {
			switch ins.Mnemonic {
			case "ADC", "SBC", "AND", "ORA", "EOR":
				continue
			case "ASL", "LSR", "ROL", "ROR", "INC", "DEC":
				if ins.Mode == cpu65816.ACC {
					continue
				}
			}
		}
		return finish("unknown", "unsupported_value_effect_"+ins.Mnemonic, nil)
	}
	return finish("unknown", "predecessor_budget", nil)
}

func initializerBits(zero, one uint16, op ShadowStoredOperation) (uint16, uint16) {
	switch op.Mnemonic {
	case "ASL":
		return zero<<1 | 1, one << 1
	case "LSR":
		return zero>>1 | 0x8000, one >> 1
	case "AND":
		return zero | ^op.Operand, one & op.Operand
	case "ORA":
		return zero & ^op.Operand, one | op.Operand
	case "EOR":
		return zero & ^op.Operand | one&op.Operand, one & ^op.Operand | zero&op.Operand
	}
	return 0, 0
}

// Only the initializer query uses the longer walk. Calls/joins/loops still
// stop recovery; the existing producer query and generated facts are unchanged.
func (walk shadowPointerWalk) initializerBank(at decoder.DecodeKey) ShadowRegisterEvidence {
	for range shadowInitializerBankLimit {
		prev := walk.previous(at)
		if prev == nil || prev.Instruction == nil {
			break
		}
		at = prev.Key
		switch prev.Instruction.Mnemonic {
		case "JSR", "JSL", "RTI", "BRK", "COP", "WAI", "MVN", "MVP":
			return ShadowRegisterEvidence{UnknownPaths: true}
		case "PLB":
			if value, ok := walk.initializerStackByte(at); ok {
				return ShadowRegisterEvidence{Constants: []ShadowRegisterConstant{{Value: uint16(value), DefinitionPC: at.PC}}}
			}
			return ShadowRegisterEvidence{UnknownPaths: true}
		}
	}
	return ShadowRegisterEvidence{UnknownPaths: true}
}

func (walk shadowPointerWalk) initializerStackByte(at decoder.DecodeKey) (byte, bool) {
	offset := 0
	for range 8 {
		prev := walk.previous(at)
		if prev == nil || prev.Instruction == nil {
			return 0, false
		}
		at = prev.Key
		ins := prev.Instruction
		switch ins.Mnemonic {
		case "PLB":
			offset++
		case "PHK":
			if offset == 0 {
				return byte(at.PC >> 16), true
			}
			offset--
		case "PEA":
			if offset < 2 {
				return byte(ins.Operand >> (8 * offset)), true
			}
			offset -= 2
		case "PHA":
			width := 2 - int(at.M)
			if offset < width {
				if source := walk.registerLoad(at, "A", width == 2); source != nil && source.Instruction.Mode == cpu65816.IMM {
					return byte(source.Instruction.Operand >> (8 * offset)), true
				}
				return 0, false
			}
			offset -= width
		default:
			return 0, false
		}
	}
	return 0, false
}

func (walk shadowPointerWalk) initializerRead(key decoder.DecodeKey, allowPointer bool) *ShadowInitializerRead {
	d := walk.graph.Instructions[key]
	if d == nil || d.Instruction == nil {
		return nil
	}
	ins := d.Instruction
	index := ""
	switch ins.Mode {
	case cpu65816.ABSX, cpu65816.LONGX:
		index = "X"
	case cpu65816.ABSY, cpu65816.INDIRY:
		index = "Y"
	}
	reg := shadowPointerRegister(ins.Mnemonic)
	if index == "" || (ins.Mnemonic != "LDA" && ins.Mnemonic != "LDX" && ins.Mnemonic != "LDY") || !shadowPointerWord(key, reg) || !shadowPointerWord(key, index) {
		return nil
	}
	r := &ShadowInitializerRead{LoadPC: key.PC, LoadMX: analysis.MXState{M: key.M, X: key.X}, Mode: ins.Mode.String(), Operand: ins.Operand, IndexRegister: index, Index: walk.initializerIndex(key, index),
		Obligations: []string{"decoded_context_not_reachability", "local_index_superset_not_table_extent", "ROM_mapping_and_data_ownership", "writer_alias_lifetime_and_reaching_definition", "stored_word_not_handler_or_stream_root"}}
	if r.Index.Field != nil {
		r.Index.LocalSource = walk.localSlotSource(r.Index.sourceKey, *r.Index.Field)
	}
	if ins.Mode == cpu65816.LONGX {
		r.DataBank.Constants = []ShadowRegisterConstant{{Value: uint16(ins.Operand >> 16), DefinitionPC: key.PC}}
	} else {
		r.DataBank = walk.initializerBank(key)
	}
	if ins.Mode == cpu65816.INDIRY {
		r.Obligations = append(r.Obligations, "direct_page_pointer_value_alias_and_wrap")
		if allowPointer {
			at := key
			for range shadowPointerWalkLimit {
				prev := walk.previous(at)
				if prev == nil || prev.Instruction == nil {
					break
				}
				at = prev.Key
				i := prev.Instruction
				if i.Mnemonic == "STA" && i.Mode == cpu65816.DP && i.Operand == ins.Operand && at.M == 0 {
					if source := walk.registerLoad(at, "A", true); source != nil {
						r.PointerSource = walk.initializerRead(source.Key, false)
						if r.PointerSource != nil {
							r.PointerStorePC = at.PC
						}
					}
					break
				}
				if i.Mnemonic == "LDA" || i.Mnemonic == "LDX" || i.Mnemonic == "LDY" || shadowPointerTransparent(i) {
					continue
				}
				if _, dst := shadowPointerTransfer(i.Mnemonic); dst != "" {
					continue
				}
				if i.Mode == cpu65816.ACC && (i.Mnemonic == "ASL" || i.Mnemonic == "LSR") {
					continue
				}
				if i.Mode == cpu65816.IMM && (i.Mnemonic == "AND" || i.Mnemonic == "ORA" || i.Mnemonic == "EOR") {
					continue
				}
				break // Writes, D changes, stack shuffles, and callees may alias.
			}
		}
	}
	return r
}

func attachInitializerSamples(image romimage.Image, results []shadowDecodeResult, sites map[uint32]*ShadowDispatchSite) {
	var reads []*ShadowInitializerRead
	var add func(*ShadowInitializerRead)
	add = func(r *ShadowInitializerRead) {
		reads = append(reads, r)
		if r.PointerSource != nil {
			copy := *r.PointerSource
			r.PointerSource = &copy
			add(&copy)
		}
	}
	for _, s := range sites {
		for i := range s.PointerProducers {
			if e := s.PointerProducers[i].IndexEvidence; e != nil {
				for j := range e.Initializers {
					add(&e.Initializers[j].Read)
				}
			}
		}
	}
	attachInitializerReadSamples(image, results, reads)
}

// Reusable for one-hop caller table reads without visiting or mutating the
// already-populated initializer tree a second time.
func attachInitializerReadSamples(image romimage.Image, results []shadowDecodeResult, reads []*ShadowInitializerRead) {
	wanted := make(map[ShadowStoredField]bool)
	for _, r := range reads {
		if r.Index.Field != nil {
			wanted[*r.Index.Field] = true
		}
	}
	writers := shadowFieldWriterIndex(results, wanted)
	for _, r := range reads {
		candidates := make(map[uint16][]uint32)
		if r.Index.Field != nil {
			for _, w := range writers[*r.Index.Field] {
				r.Index.Writers = append(r.Index.Writers, w)
				if w.Width != 2 || w.StoredValue == nil {
					continue
				}
				zero, one := ^*w.StoredValue, *w.StoredValue
				for _, op := range r.Index.Operations {
					zero, one = initializerBits(zero, one, op)
				}
				if zero|one == 0xffff {
					candidates[one] = appendUniqueAddresses(candidates[one], w.StorePC)
				}
			}
		}
		local := make(map[uint16]bool)
		for _, v := range r.Index.DomainValues {
			local[v] = true
			if _, ok := candidates[v]; !ok {
				candidates[v] = nil
			}
		}
		var values []uint16
		for v := range candidates {
			values = append(values, v)
		}
		slices.Sort(values)
		bankset := make(map[byte]bool)
		for _, d := range r.DataBank.Constants {
			bankset[byte(d.Value)] = true
		}
		var banks []byte
		for b := range bankset {
			banks = append(banks, b)
		}
		slices.Sort(banks)
		if r.Mode == "(dp),y" {
			continue
		} // Never substitute pointer-source samples through an unproven alias.
		for _, index := range values {
			for _, bank := range banks {
				if len(r.Samples) >= shadowPointerSampleLimit {
					r.SamplesTruncated = true
					break
				}
				slices.Sort(candidates[index])
				sample := ShadowInitializerSample{Index: index, Bank: bank, WriterPCs: candidates[index], IndexEvidence: "conditional_writer_value"}
				if local[index] {
					sample.IndexEvidence = "local_word_domain_superset"
				}
				address := uint32(uint16(r.Operand)) + uint32(index)
				switch {
				case address > 0xfffe:
					sample.Status = "bank_boundary_not_sampled"
				case bank == 0x7e || bank == 0x7f:
					sample.Status = "not_rom_mapped"
				default:
					sample.ReadPC = uint32(bank)<<16 | address
					if bytes, err := image.Slice(bank, uint16(address), 2); err == nil {
						word := uint16(bytes[0]) | uint16(bytes[1])<<8
						sample.Word = &word
						sample.Status = "conditional_rom_word_not_a_root"
					} else {
						sample.Status = "not_rom_mapped"
					}
				}
				r.Samples = append(r.Samples, sample)
			}
		}
	}
}

// Exact expression spellings across decoded program banks, not alias proof.
func shadowFieldWriterIndex(results []shadowDecodeResult, wanted map[ShadowStoredField]bool) map[ShadowStoredField][]ShadowStoredTargetWriter {
	writers := make(map[ShadowStoredField]map[string]ShadowStoredTargetWriter)
	for _, r := range results {
		if r.issue != nil {
			continue
		}
		for _, w := range r.storedWrites {
			if !wanted[w.field] {
				continue
			}
			if writers[w.field] == nil {
				writers[w.field] = make(map[string]ShadowStoredTargetWriter)
			}
			v, contexts := w.writer, w.writer.Contexts
			v.Contexts = nil
			id := shadowStoredJSONKey(v)
			v.Contexts = mergeStoredContexts(writers[w.field][id].Contexts, contexts)
			writers[w.field][id] = v
		}
	}
	index := make(map[ShadowStoredField][]ShadowStoredTargetWriter)
	for field, matches := range writers {
		for _, w := range matches {
			index[field] = append(index[field], w)
		}
		sort.Slice(index[field], func(i, j int) bool {
			return shadowStoredJSONKey(index[field][i]) < shadowStoredJSONKey(index[field][j])
		})
	}
	return index
}

package tooling

import (
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// Index evidence joins identically spelled fields, not proven memory aliases.
// Samples answer "what word would this writer value select on this local path?"
// They do not prove a reachable handler, entry width, or a closed target domain.
type ShadowPointerIndexEvidence struct {
	Field          *ShadowStoredField         `json:"field,omitempty"`
	OriginRegister string                     `json:"origin_register"`
	WriterMatch    string                     `json:"writer_match"`
	ValueSetStatus string                     `json:"value_set_status"`
	Path           []ShadowPointerPathStep    `json:"origin_to_index_path,omitempty"`
	Writers        []ShadowStoredTargetWriter `json:"writer_candidates,omitempty"`
	Values         []ShadowPointerIndexValue  `json:"literal_value_candidates,omitempty"`
	Reads          []ShadowPointerReadSample  `json:"rom_read_samples,omitempty"`
	Truncated      bool                       `json:"samples_truncated,omitempty"`
	Initializers   []ShadowTableInitializer   `json:"table_initializers,omitempty"`
}

type ShadowPointerIndexValue struct {
	Value     uint16   `json:"value"`
	WriterPCs []uint32 `json:"writer_pcs,omitempty"`
}

type ShadowPointerPathStep struct {
	PC          uint32           `json:"pc"`
	Mnemonic    string           `json:"mnemonic"`
	Mode        string           `json:"mode"`
	Operand     uint32           `json:"operand"`
	MX          analysis.MXState `json:"mx"`
	BranchTaken *bool            `json:"branch_taken,omitempty"`
}

type ShadowPointerReadSample struct {
	IndexValue    uint16  `json:"index_value"`
	DataBank      byte    `json:"data_bank"`
	ReadPC        uint32  `json:"read_pc,omitempty"`
	Word          *uint16 `json:"word,omitempty"`
	Status        string  `json:"status"`
	TargetPC      *uint32 `json:"conditional_target_pc,omitempty"`
	AuthoredEntry bool    `json:"authored_address"`
	TargetROM     bool    `json:"target_rom_mapped"`
}

const shadowPointerSampleLimit = 512

func (walk shadowPointerWalk) pointerPath(from, to decoder.DecodeKey) ([]ShadowPointerPathStep, bool) {
	var path []ShadowPointerPathStep
	for range shadowPointerWalkLimit {
		decoded := walk.previous(to)
		if decoded == nil || decoded.Instruction == nil {
			return nil, false
		}
		if decoded.Key == from {
			slices.Reverse(path)
			return path, true
		}
		ins := decoded.Instruction
		step := ShadowPointerPathStep{PC: decoded.Key.PC, Mnemonic: ins.Mnemonic, Mode: ins.Mode.String(), Operand: ins.Operand, MX: analysis.MXState{M: decoded.Key.M, X: decoded.Key.X}}
		switch ins.Mnemonic {
		case "BCC", "BCS", "BEQ", "BNE", "BMI", "BPL", "BVC", "BVS":
			target := decoded.Key.PC&0xff0000 | ins.Operand&0xffff
			next := decoded.Key.PC&0xff0000 | uint32(uint16(decoded.Key.PC)+uint16(ins.Length))
			if target != next && (to.PC == target || to.PC == next) {
				taken := to.PC == target
				step.BranchTaken = &taken
			}
		}
		path = append(path, step)
		to = decoded.Key
	}
	return nil, false
}

// Evaluate only the decoded local path, for a hypothetical loaded word. This
// does not prove that a matching memory writer supplies that word at runtime.
// Unrelated comparisons (e.g. the object-loop counter) cannot bound the index.
func shadowPointerPathStatus(path []ShadowPointerPathStep, source string, value uint16) string {
	regs := map[string]*uint16{source: &value}
	var n, z, c, v *bool
	setNZ := func(word *uint16) {
		n, z = nil, nil
		if word != nil {
			negative, zero := *word&0x8000 != 0, *word == 0
			n, z = &negative, &zero
		}
	}
	setNZ(&value)
	unknown := false
	for _, step := range path {
		wordReg := func(reg string) bool { return (reg == "A" && step.MX.M == 0) || (reg != "A" && step.MX.X == 0) }
		if src, dst := shadowPointerTransfer(step.Mnemonic); dst != "" {
			regs[dst] = nil
			if wordReg(src) && wordReg(dst) {
				regs[dst] = regs[src]
			}
			setNZ(regs[dst])
			continue
		}
		switch step.Mnemonic {
		case "LDA", "LDX", "LDY":
			reg := shadowPointerRegister(step.Mnemonic)
			regs[reg] = nil
			if step.Mode == "imm" && wordReg(reg) {
				word := uint16(step.Operand)
				regs[reg] = &word
			}
			setNZ(regs[reg])
		case "CMP", "CPX", "CPY":
			reg := "A"
			if step.Mnemonic == "CPX" {
				reg = "X"
			}
			if step.Mnemonic == "CPY" {
				reg = "Y"
			}
			n, z, c = nil, nil, nil
			if step.Mode == "imm" && regs[reg] != nil && wordReg(reg) {
				diff := *regs[reg] - uint16(step.Operand)
				setNZ(&diff)
				carry := *regs[reg] >= uint16(step.Operand)
				c = &carry
			}
		case "BIT":
			z = nil
			if step.Mode == "imm" && regs["A"] != nil && step.MX.M == 0 {
				zero := *regs["A"]&uint16(step.Operand) == 0
				z = &zero
			} else if step.Mode != "imm" {
				n, v = nil, nil
			}
		case "CLC", "SEC":
			carry := step.Mnemonic == "SEC"
			c = &carry
		case "CLV":
			overflow := false
			v = &overflow
		case "BCC", "BCS", "BEQ", "BNE", "BMI", "BPL", "BVC", "BVS":
			var flag *bool
			positive := true
			switch step.Mnemonic {
			case "BCC":
				flag, positive = c, false
			case "BCS":
				flag = c
			case "BEQ":
				flag = z
			case "BNE":
				flag, positive = z, false
			case "BMI":
				flag = n
			case "BPL":
				flag, positive = n, false
			case "BVC":
				flag, positive = v, false
			case "BVS":
				flag = v
			}
			if flag == nil || step.BranchTaken == nil {
				unknown = true
			} else if (*flag == positive) != *step.BranchTaken {
				return "fails_local_guards"
			}
		case "NOP", "BRA", "BRL", "JMP", "CLI", "SEI", "CLD", "SED":
		default:
			// Do not infer flag preservation for an unmodeled operation.
			n, z, c, v = nil, nil, nil, nil
			regs = make(map[string]*uint16)
			unknown = true
		}
	}
	if unknown {
		return "unknown_local_guard"
	}
	return "passes_local_guards"
}

func attachShadowPointerIndexEvidence(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, sites map[uint32]*ShadowDispatchSite) {
	wanted := make(map[ShadowStoredField]bool)
	hasEvidence := false
	for _, site := range sites {
		for _, producer := range site.PointerProducers {
			if producer.IndexEvidence != nil {
				hasEvidence = true
			}
			if producer.IndexEvidence != nil && producer.IndexEvidence.Field != nil {
				wanted[*producer.IndexEvidence.Field] = true
			}
		}
	}
	if !hasEvidence {
		return
	}
	writes := make(map[ShadowStoredField]map[string]ShadowStoredTargetWriter)
	initializers := make(map[ShadowStoredField]map[string]ShadowTableInitializer)
	for _, result := range results {
		if result.issue == nil {
			for _, write := range result.storedWrites {
				if !wanted[write.field] {
					continue
				}
				if writes[write.field] == nil {
					writes[write.field] = make(map[string]ShadowStoredTargetWriter)
				}
				writer, contexts := write.writer, write.writer.Contexts
				writer.Contexts = nil
				identity := shadowStoredJSONKey(writer)
				writer.Contexts = mergeStoredContexts(writes[write.field][identity].Contexts, contexts)
				writes[write.field][identity] = writer
				if write.initializer != nil {
					if initializers[write.field] == nil {
						initializers[write.field] = make(map[string]ShadowTableInitializer)
					}
					init := *write.initializer
					contexts := init.Contexts
					init.Contexts = nil
					id := shadowStoredJSONKey(init)
					init.Contexts = mergeStoredContexts(initializers[write.field][id].Contexts, contexts)
					initializers[write.field][id] = init
				}
			}
		}
	}
	authored := make(map[uint32]bool)
	for _, bank := range banks {
		for _, entry := range bank.Config.Entries {
			authored[uint32(bank.ID)<<16|uint32(entry.Start)] = true
		}
	}
	for _, site := range sites {
		for i := range site.PointerProducers {
			p := &site.PointerProducers[i]
			if p.IndexEvidence == nil {
				continue
			}
			// Do not mutate decode-result evidence shared by another inventory use.
			e := *p.IndexEvidence
			p.IndexEvidence = &e
			if e.Field != nil {
				for _, init := range initializers[*e.Field] {
					e.Initializers = append(e.Initializers, init)
				}
				sort.Slice(e.Initializers, func(i, j int) bool {
					return shadowStoredJSONKey(e.Initializers[i]) < shadowStoredJSONKey(e.Initializers[j])
				})
				values := make(map[uint16][]uint32)
				for _, writer := range writes[*e.Field] {
					e.Writers = append(e.Writers, writer)
					if writer.Width == 2 && writer.StoredValue != nil {
						values[*writer.StoredValue] = appendUniqueAddresses(values[*writer.StoredValue], writer.StorePC)
					}
				}
				for value, pcs := range values {
					slices.Sort(pcs)
					e.Values = append(e.Values, ShadowPointerIndexValue{Value: value, WriterPCs: pcs})
				}
				sort.Slice(e.Writers, func(i, j int) bool {
					if e.Writers[i].StorePC != e.Writers[j].StorePC {
						return e.Writers[i].StorePC < e.Writers[j].StorePC
					}
					return shadowStoredJSONKey(e.Writers[i]) < shadowStoredJSONKey(e.Writers[j])
				})
				sort.Slice(e.Values, func(i, j int) bool { return e.Values[i].Value < e.Values[j].Value })
			}
			bankSet := make(map[byte]bool)
			for _, constant := range p.DataBank.Constants {
				bankSet[byte(constant.Value)] = true
			}
			var dataBanks []byte
			for bank := range bankSet {
				dataBanks = append(dataBanks, bank)
			}
			slices.Sort(dataBanks)
			for _, value := range e.Values {
				for _, bank := range dataBanks {
					if len(e.Reads) >= shadowPointerSampleLimit {
						e.Truncated = true
						break
					}
					sample := ShadowPointerReadSample{IndexValue: value.Value, DataBank: bank}
					if status := shadowPointerPathStatus(e.Path, e.OriginRegister, value.Value); status != "passes_local_guards" {
						sample.Status = "index_" + status
					} else if uint32(p.TableOffset)+uint32(value.Value) > 0xfffe {
						sample.Status = "bank_boundary_not_sampled"
					} else if bank == 0x7e || bank == 0x7f {
						sample.Status = "not_rom_mapped"
					} else {
						address := uint16(uint32(p.TableOffset) + uint32(value.Value))
						sample.ReadPC = uint32(bank)<<16 | uint32(address)
						// Validate the effective address, not the unindexed base: a
						// stream read based at $0000 can reach mapped ROM through Y.
						if bytes, err := image.Slice(bank, address, 2); err == nil {
							word := uint16(bytes[0]) | uint16(bytes[1])<<8
							sample.Word = &word
							sample.Status = "word_" + shadowPointerPathStatus(p.LoadPath, p.LoadRegister, word)
							if sample.Status == "word_passes_local_guards" {
								pc := p.TrampolinePC&0xff0000 | uint32(word)
								sample.TargetPC = &pc
								sample.AuthoredEntry = authored[pc]
								_, err := image.Slice(byte(pc>>16), uint16(pc), 1)
								sample.TargetROM = err == nil && byte(pc>>16) != 0x7e && byte(pc>>16) != 0x7f
							}
						} else {
							sample.Status = "not_rom_mapped"
						}
					}
					e.Reads = append(e.Reads, sample)
				}
			}
		}
	}
	attachInitializerSamples(image, results, sites)
	attachShadowEntryCallInputs(results, sites)
	attachShadowCallerSources(image, results, sites)
	attachShadowBankContexts(image, banks, results, sites)
}

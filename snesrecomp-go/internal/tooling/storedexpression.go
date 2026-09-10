package tooling

import (
	"slices"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// This is a local word-expression audit, not an interprocedural value proof.
// In particular a register after a call may be preserved, replaced, or unknown.
type ShadowStoredExpression struct {
	Source      ShadowStoredOrigin      `json:"source"`
	Operations  []ShadowStoredOperation `json:"operations,omitempty"` // execution order
	Status      string                  `json:"status"`
	ExactAddend *int                    `json:"exact_addend,omitempty"`
	Constant    *uint16                 `json:"constant_word,omitempty"`
	SourceField *ShadowStoredField      `json:"source_field,omitempty"`
}

type ShadowStoredOrigin struct {
	Kind     string `json:"kind"`
	PC       uint32 `json:"pc"`
	Register string `json:"register"`
	Mode     string `json:"mode,omitempty"`
	Operand  uint32 `json:"operand,omitempty"`
	Reason   string `json:"reason,omitempty"`
}

type ShadowStoredOperation struct {
	PC       uint32            `json:"pc"`
	Mnemonic string            `json:"mnemonic"`
	Operand  uint16            `json:"operand"`
	Carry    *ShadowStoredFlag `json:"carry_before,omitempty"`
	Decimal  *ShadowStoredFlag `json:"decimal_before,omitempty"`
}

type ShadowStoredFlag struct {
	Value        *uint8 `json:"value,omitempty"`
	DefinitionPC uint32 `json:"definition_pc,omitempty"`
	Reason       string `json:"reason"`
}

// storedFlag returns only a local constant definition on a unique predecessor
// chain. Entry flags are unknown; no game-wide "decimal is always clear"
// convention, reset-root inference, or callee-preservation assumption applies.
func (walk shadowPointerWalk) storedFlag(key decoder.DecodeKey, mask byte) ShadowStoredFlag {
	for range shadowPointerWalkLimit {
		decoded := walk.previous(key)
		if decoded == nil || decoded.Instruction == nil {
			return ShadowStoredFlag{Reason: "entry_boundary_or_ambiguous_predecessor"}
		}
		key = decoded.Key
		ins := decoded.Instruction
		var value uint8
		defined := false
		switch {
		case ins.Mnemonic == "REP" && byte(ins.Operand)&mask != 0:
			defined = true
		case ins.Mnemonic == "SEP" && byte(ins.Operand)&mask != 0:
			defined, value = true, 1
		case mask == 1 && ins.Mnemonic == "CLC", mask == 8 && ins.Mnemonic == "CLD":
			defined = true
		case mask == 1 && ins.Mnemonic == "SEC", mask == 8 && ins.Mnemonic == "SED":
			defined, value = true, 1
		}
		if defined {
			return ShadowStoredFlag{Value: &value, DefinitionPC: key.PC, Reason: "local_constant_definition"}
		}
		switch ins.Mnemonic {
		case "JSR", "JSL", "PLP", "RTI", "BRK", "COP", "XCE":
			return ShadowStoredFlag{Reason: "status_barrier_" + ins.Mnemonic}
		case "ADC", "SBC", "CMP", "CPX", "CPY", "ASL", "LSR", "ROL", "ROR":
			if mask == 1 {
				return ShadowStoredFlag{Reason: "carry_clobber_" + ins.Mnemonic}
			}
			continue // These do not change decimal mode.
		case "REP", "SEP", "CLC", "SEC", "CLD", "SED", "PHP", "PHA", "PHX", "PHY", "PHB", "PHD", "PHK", "PEA", "PEI", "PER",
			"PLA", "PLX", "PLY", "PLB", "PLD", "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ",
			"AND", "ORA", "EOR", "BIT", "TRB", "TSB", "INC", "DEC", "INX", "INY", "DEX", "DEY", "XBA",
			"TAX", "TAY", "TXA", "TYA", "TXY", "TYX", "TCD", "TDC", "TCS", "TSC", "TSX", "TXS":
			continue
		}
		if shadowPointerTransparent(ins) {
			continue
		}
		return ShadowStoredFlag{Reason: "unsupported_status_effect_" + ins.Mnemonic}
	}
	return ShadowStoredFlag{Reason: "predecessor_budget"}
}

// storedExpression augments an otherwise unknown writer with its symbolic
// origin and arithmetic. It never changes the legacy writer's StoredValue,
// ValueAddend, or Kind, nor feeds candidates into the dispatch proof pipeline.
func (walk shadowPointerWalk) storedExpression(key decoder.DecodeKey, reg string) *ShadowStoredExpression {
	expression := &ShadowStoredExpression{}
	finish := func(kind, reason string, decoded *decoder.DecodedInstruction) *ShadowStoredExpression {
		expression.Source = ShadowStoredOrigin{Kind: kind, PC: key.PC, Register: reg, Reason: reason}
		if decoded != nil {
			expression.Source.Mode = decoded.Instruction.Mode.String()
			expression.Source.Operand = decoded.Instruction.Operand
			if field, valid := shadowStoredField(decoded.Instruction); valid && kind == "load" {
				expression.SourceField = &field
			}
		}
		slices.Reverse(expression.Operations)
		expression.Status = "unresolved_origin"
		if kind != "load" && kind != "stack_pull" {
			return expression
		}
		addend := 0
		for _, operation := range expression.Operations {
			switch operation.Mnemonic {
			case "INC", "INX", "INY":
				addend++
			case "DEC", "DEX", "DEY":
				addend--
			case "ADC", "SBC":
				if operation.Carry.Value == nil || operation.Decimal.Value == nil || *operation.Decimal.Value != 0 {
					expression.Status = "conditional_arithmetic"
					return expression
				}
				if operation.Mnemonic == "ADC" {
					addend += int(operation.Operand) + int(*operation.Carry.Value)
				} else {
					addend -= int(operation.Operand) + 1 - int(*operation.Carry.Value)
				}
			}
		}
		expression.Status, expression.ExactAddend = "local_word_expression", &addend
		if kind == "load" && decoded.Instruction.Mode == cpu65816.IMM {
			value := uint16(int(decoded.Instruction.Operand) + addend)
			expression.Constant = &value
		}
		return expression
	}
	for range shadowPointerWalkLimit {
		decoded := walk.previous(key)
		if decoded == nil || decoded.Instruction == nil {
			kind, reason := "unknown", "ambiguous_or_missing_predecessor"
			if key == walk.graph.Entry {
				kind, reason = "entry_register", "entry_register_value_unproven"
			}
			return finish(kind, reason, nil)
		}
		key = decoded.Key
		ins := decoded.Instruction
		if !shadowPointerWord(key, reg) {
			return finish("unknown", "byte_or_truncated_value", decoded)
		}
		if ins.Mnemonic == "LD"+reg {
			return finish("load", "", decoded)
		}
		if ins.Mnemonic == "PL"+reg {
			return finish("stack_pull", "stack_origin_unproven", decoded)
		}
		if (reg == "A" && ins.Mode == cpu65816.ACC && (ins.Mnemonic == "INC" || ins.Mnemonic == "DEC")) ||
			(reg == "X" && (ins.Mnemonic == "INX" || ins.Mnemonic == "DEX")) ||
			(reg == "Y" && (ins.Mnemonic == "INY" || ins.Mnemonic == "DEY")) {
			expression.Operations = append(expression.Operations, ShadowStoredOperation{PC: key.PC, Mnemonic: ins.Mnemonic})
			continue
		}
		if reg == "A" && (ins.Mnemonic == "ADC" || ins.Mnemonic == "SBC") && ins.Mode == cpu65816.IMM {
			carry, decimal := walk.storedFlag(key, 1), walk.storedFlag(key, 8)
			expression.Operations = append(expression.Operations, ShadowStoredOperation{PC: key.PC, Mnemonic: ins.Mnemonic, Operand: uint16(ins.Operand), Carry: &carry, Decimal: &decimal})
			continue
		}
		if src, dst := shadowPointerTransfer(ins.Mnemonic); dst != "" {
			if dst == reg {
				if !shadowPointerWord(key, src) {
					return finish("unknown", "truncated_transfer", decoded)
				}
				reg = src
			}
			continue
		}
		switch ins.Mnemonic {
		case "JSR", "JSL":
			return finish("register_after_call", "callee_register_summary_missing", decoded)
		case "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ", "REP", "SEP":
			// A store does not clobber the value held in a register. A load
			// origin denotes its earlier snapshot, NOT a fresh read at store PC.
			continue
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
		return finish("unknown", "unsupported_value_effect_"+ins.Mnemonic, decoded)
	}
	return finish("unknown", "predecessor_budget", nil)
}

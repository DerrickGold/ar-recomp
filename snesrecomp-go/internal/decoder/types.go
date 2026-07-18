package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
)

// DecodeKey is comparable so it can be used directly as a Go map key. PHP
// history stores two bits per pushed M/X pair in PStack, with PDepth entries.
type DecodeKey struct {
	PC     uint32
	M      uint8
	X      uint8
	PStack uint32
	PDepth uint8
}

type DecodedInstruction struct {
	Key         DecodeKey
	Instruction *cpu65816.Instruction
	Successors  []DecodeKey
}

type SuppressedIndirectCall struct {
	SitePC, FunctionEntry uint32
	TableBase             uint16
	EntryM, EntryX        uint8
}

type DispatchTargetSuppressed struct {
	SitePC, TargetPC uint32
	Reason           string
	TableIndex       int
}

type UnresolvedIndirect struct {
	SitePC, FunctionEntry uint32
	Mnemonic              string
	Mode                  cpu65816.AddressingMode
	Operand               uint32
	EntryM, EntryX        uint8
}

type ConstantZFold struct {
	BranchPC, PreviousPC             uint32
	BranchMnemonic, PreviousMnemonic string
	PreviousImmediate                uint32
	WidthBits, ZValue                int
	TakenKind                        string
	LivePC, DeadPC, FunctionEntry    uint32
	EntryM, EntryX                   uint8
}

type Graph struct {
	Entry                     DecodeKey
	Instructions              map[DecodeKey]*DecodedInstruction
	Order                     []DecodeKey
	SuppressedIndirectCalls   []SuppressedIndirectCall
	ConstantZFolds            []ConstantZFold
	DispatchTargetsSuppressed []DispatchTargetSuppressed
	UnresolvedIndirects       []UnresolvedIndirect
}

// record preserves Python's insertion-ordered dictionary behavior. Stable
// decode order is part of generated-output reproducibility when multiple PHP
// histories collapse to the same PC/M/X variant.
func (graph *Graph) record(decoded *DecodedInstruction) {
	if _, found := graph.Instructions[decoded.Key]; !found {
		graph.Order = append(graph.Order, decoded.Key)
	}
	graph.Instructions[decoded.Key] = decoded
}

func (graph *Graph) KeysAtPC(pc uint32) []DecodeKey {
	var keys []DecodeKey
	for key := range graph.Instructions {
		if key.PC == pc {
			keys = append(keys, key)
		}
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].M != keys[j].M {
			return keys[i].M < keys[j].M
		}
		if keys[i].X != keys[j].X {
			return keys[i].X < keys[j].X
		}
		if keys[i].PDepth != keys[j].PDepth {
			return keys[i].PDepth < keys[j].PDepth
		}
		return keys[i].PStack < keys[j].PStack
	})
	return keys
}

type Variant struct {
	Address uint32
	M, X    uint8
}
type MX struct{ M, X int8 }

type IndirectCallTable struct {
	Base  uint16
	Count int
	Kind  string
}

type DispatchAuth struct {
	Count      int
	IndexReg   string
	TableBases []uint16
	ReturnPC   *uint16
	SEPMask    byte
	RTSTrick   bool
	Targets    []uint16
}

type DataRegion struct {
	Bank       byte
	Start, End uint16
}

type Options struct {
	End                *uint16
	MaxInstructions    int
	DispatchHelpers    map[uint32]string
	IndirectCallTables map[uint32]IndirectCallTable
	IndirectDispatch   map[uint32]DispatchAuth
	HLEDispatch        map[uint16]string
	DataRegions        []DataRegion
	CalleeExitMX       map[Variant]MX
	CalleeExitModes    map[Variant][]MX
	SiblingEntryPCs    map[uint16]struct{}
}

func Address24(bank byte, pc uint16) uint32 { return uint32(bank)<<16 | uint32(pc) }

const maxPHPDepth = 8

func PostState(instruction *cpu65816.Instruction, key DecodeKey) DecodeKey {
	result := key
	switch instruction.Mnemonic {
	case "REP":
		if instruction.Operand&0x20 != 0 {
			result.M = 0
		}
		if instruction.Operand&0x10 != 0 {
			result.X = 0
		}
	case "SEP":
		if instruction.Operand&0x20 != 0 {
			result.M = 1
		}
		if instruction.Operand&0x10 != 0 {
			result.X = 1
		}
	case "PHP":
		if result.PDepth < maxPHPDepth {
			shift := result.PDepth * 2
			result.PStack |= uint32((result.M&1)<<1|(result.X&1)) << shift
			result.PDepth++
		}
	case "PLP":
		if result.PDepth > 0 {
			result.PDepth--
			shift := result.PDepth * 2
			value := uint8(result.PStack >> shift & 3)
			result.M, result.X = value>>1, value&1
			result.PStack &= ^(uint32(3) << shift)
		}
	}
	return result
}

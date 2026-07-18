// Package ir defines the stateful intermediate representation used by v2.
package ir

type Reg string

const (
	A  Reg = "A"
	B  Reg = "B"
	X  Reg = "X"
	Y  Reg = "Y"
	S  Reg = "S"
	D  Reg = "D"
	DB Reg = "DB"
	PB Reg = "PB"
	P  Reg = "P"
	M  Reg = "M"
	XF Reg = "XF"
	E  Reg = "E"
	N  Reg = "N"
	V  Reg = "V"
	ZF Reg = "ZF"
	C  Reg = "C"
	I  Reg = "I"
	DF Reg = "DF"
)

type SegKind string

const (
	Direct                 SegKind = "direct"
	AbsoluteBank           SegKind = "abs_bank"
	Long                   SegKind = "long"
	Stack                  SegKind = "stack"
	DPIndirect             SegKind = "dp_indirect"
	DPIndirectLong         SegKind = "dp_indirect_long"
	AbsoluteIndirect       SegKind = "abs_indirect"
	AbsoluteIndirectLong   SegKind = "abs_indirect_long"
	AbsoluteIndirectX      SegKind = "abs_indirect_x"
	DPIndirectX            SegKind = "dp_indirect_x"
	StackRelativeIndirectY SegKind = "stack_rel_indirect_y"
)

type SegRef struct {
	Kind   SegKind
	Offset uint32
	Bank   *byte
	Index  *Reg
}

type Value struct{ ID int }

// Op is intentionally represented as an interface. Concrete operations are a
// closed convention enforced by lowering and emitter type switches.
type Op = any

type Read struct {
	Seg   SegRef
	Width int
	Out   Value
}
type Write struct {
	Seg   SegRef
	Src   Value
	Width int
}
type ReadReg struct {
	Reg Reg
	Out Value
}
type WriteReg struct {
	Reg Reg
	Src Value
}
type ConstI struct {
	Value uint32
	Width int
	Out   Value
}

type AluKind string

const (
	Add     AluKind = "add"
	Sub     AluKind = "sub"
	And     AluKind = "and"
	Or      AluKind = "or"
	Xor     AluKind = "xor"
	Compare AluKind = "cmp"
)

type Alu struct {
	Kind     AluKind
	LHS, RHS Value
	Width    int
	Out      *Value
}

type ShiftKind string

const (
	ASL ShiftKind = "asl"
	LSR ShiftKind = "lsr"
	ROL ShiftKind = "rol"
	ROR ShiftKind = "ror"
)

type Shift struct {
	Kind  ShiftKind
	Src   Value
	Width int
	Out   Value
}
type IncReg struct {
	Reg   Reg
	Delta int
}
type IncMem struct {
	Seg          SegRef
	Width, Delta int
}
type BitTest struct {
	Operand Value
	Width   int
	ZOnly   bool
}
type BitSetMem struct {
	Seg   SegRef
	Width int
}
type BitClearMem struct {
	Seg   SegRef
	Width int
}
type SetFlag struct {
	Flag  Reg
	Value int
}
type SetNZ struct {
	Src   Value
	Width int
}
type RepFlags struct{ Mask byte }
type SepFlags struct{ Mask byte }
type ExchangeCarryEmulation struct{}
type Push struct {
	Src   Value
	Width int
}
type Pull struct {
	Width int
	Out   Value
}
type PushReg struct {
	Reg              Reg
	StaticM, StaticX *uint8
}
type PullReg struct {
	Reg              Reg
	StaticM, StaticX *uint8
}
type BlockMove struct {
	Direction                   string
	SourceBank, DestinationBank byte
}
type CondBranch struct {
	Flag   Reg
	TakeIf int
}
type Goto struct{}
type IndirectGoto struct{ Seg SegRef }
type Call struct {
	Target         *uint32
	Long, Indirect bool
	EntryM, EntryX uint8
	SourcePC       *uint32
	TableBase      *uint16
}
type Return struct {
	Long, Interrupt bool
	SourcePC        *uint32
}
type Transfer struct{ Source, Destination Reg }
type ExchangeAccumulatorBytes struct{}
type Nop struct{}
type Break struct{ COP bool }
type Stop struct{ Wait bool }
type PushEffectiveAddress struct{ Seg SegRef }
type Block struct{ Ops []Op }

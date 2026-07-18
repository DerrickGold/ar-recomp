package lowering

import (
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/ir"
)

func factory() ValueFactory {
	next := 0
	return func() ir.Value { value := ir.Value{ID: next}; next++; return value }
}

func TestEveryOpcodeMnemonicIsLowered(t *testing.T) {
	known := KnownMnemonics()
	for opcode := 0; opcode < 256; opcode++ {
		instruction, err := cpu65816.Decode([]byte{byte(opcode), 0, 0, 0}, 0, 0x8000, 0, 1, 1)
		if err != nil {
			t.Fatal(err)
		}
		if _, ok := known[instruction.Mnemonic]; !ok {
			t.Errorf("opcode %02x mnemonic %s has no lowering", opcode, instruction.Mnemonic)
		}
		if operations := Lower(instruction, factory()); len(operations) == 0 {
			t.Errorf("opcode %02x lowered to no operations", opcode)
		}
	}
}

func TestJSRStampsBankAndMode(t *testing.T) {
	instruction, err := cpu65816.Decode([]byte{0x20, 0x34, 0x92}, 0, 0x8123, 3, 0, 1)
	if err != nil {
		t.Fatal(err)
	}
	instruction.M, instruction.X = 0, 1
	call := Lower(instruction, factory())[0].(ir.Call)
	if call.Target == nil || *call.Target != 0x039234 || call.EntryM != 0 || call.EntryX != 1 {
		t.Fatalf("call mismatch: %#v", call)
	}
}

package cpu65816

import (
	"crypto/sha256"
	"fmt"
	"testing"
)

func TestOpcodeTableIsComplete(t *testing.T) {
	if got := OpcodeCount(); got != 256 {
		t.Fatalf("opcode count=%d, want 256", got)
	}
}

func TestImmediateWidths(t *testing.T) {
	data := []byte{0xA9, 0x34, 0x12}
	narrow, err := Decode(data, 0, 0x8000, 0, 1, 1)
	if err != nil {
		t.Fatal(err)
	}
	wide, err := Decode(data, 0, 0x8000, 0, 0, 1)
	if err != nil {
		t.Fatal(err)
	}
	if narrow.Length != 2 || narrow.Operand != 0x34 {
		t.Fatalf("narrow LDA mismatch: %#v", narrow)
	}
	if wide.Length != 3 || wide.Operand != 0x1234 {
		t.Fatalf("wide LDA mismatch: %#v", wide)
	}
}

func TestRelativeWrap(t *testing.T) {
	instruction, err := Decode([]byte{0x80, 0x02}, 0, 0xfffe, 3, 1, 1)
	if err != nil {
		t.Fatal(err)
	}
	if instruction.Operand != 0x0002 {
		t.Fatalf("relative target=%04x, want 0002", instruction.Operand)
	}
}

func TestPythonOpcodeParityDigest(t *testing.T) {
	// Generated once from snes65816.py across every opcode and all four M/X
	// combinations. This pins mnemonic, mode, length, and operand behavior
	// without requiring Python during Go tests.
	digest := sha256.New()
	for opcode := 0; opcode < 256; opcode++ {
		for m := 0; m < 2; m++ {
			for x := 0; x < 2; x++ {
				instruction, err := Decode([]byte{byte(opcode), 0x34, 0x12, 0x56}, 0, 0x9234, 3, uint8(m), uint8(x))
				if err != nil {
					t.Fatal(err)
				}
				fmt.Fprintf(digest, "%02x,%d,%d,%s,%d,%d,%06x\n", opcode, m, x, instruction.Mnemonic, instruction.Mode, instruction.Length, instruction.Operand)
			}
		}
	}
	want := "03463ace072b9c3543412733e8ef98b52dd00b9e49e063df24055521272d4345"
	if got := fmt.Sprintf("%x", digest.Sum(nil)); got != want {
		t.Fatalf("opcode parity digest=%s, want %s", got, want)
	}
}

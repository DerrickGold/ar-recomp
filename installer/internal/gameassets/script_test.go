package gameassets

import "testing"

func TestScriptBoundsAndOwnership(t *testing.T) {
	rom := make([]byte, ScriptBase+3)
	// Highest set bit selects the operand grammar, just as the native VM does.
	for bit, count := range operandCounts {
		rom = append(rom, byte(bit), 2, byte(1<<bit))
		rom = append(rom, make([]byte, count)...)
		rom = append(rom, 0)
	}
	rom = append(rom, 8, 1, 0)
	entries, err := Script(rom)
	if err != nil || len(entries) != 9 {
		t.Fatal(entries, err)
	}
	entries[0].Commands[0].Operands[0] = 99
	if rom[ScriptBase+6] != 0 {
		t.Fatal("operands alias input")
	}
	for end := ScriptBase + 3; end < len(rom); end++ {
		if _, err := Script(rom[:end]); err == nil {
			t.Fatal("truncation accepted", end)
		}
	}
	for _, input := range [][]byte{nil, make([]byte, ScriptLimit), make([]byte, ScriptLimit+8)} {
		if _, err := Script(input); err == nil {
			t.Fatal("missing end accepted")
		}
	}
}

func FuzzScript(f *testing.F) {
	f.Add([]byte{8, 1, 0})
	f.Fuzz(func(t *testing.T, data []byte) {
		if len(data) > ScriptLimit-ScriptBase {
			return
		}
		rom := make([]byte, ScriptBase+3)
		rom = append(rom, data...)
		entries, err := Script(rom)
		if err != nil {
			return
		}
		for _, e := range entries {
			if e.Start < ScriptBase+3 || e.End > len(rom) || e.End > ScriptLimit || e.End <= e.Start {
				t.Fatal(e)
			}
		}
	})
}

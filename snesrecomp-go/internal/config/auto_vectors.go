package config

import "github.com/DerrickGold/snesrecomp-go/internal/rom"

// AppendLoROMAutoVectorEntries adds the reset and native interrupt roots used
// by the auto_vectors directive. Reset begins in emulation mode with M=1/X=1.
// Native NMI and IRQ preserve the interrupted width flags, so all four live
// variants are roots unless an authored entry already owns the vector PC or
// reserved name.
func AppendLoROMAutoVectorEntries(image []byte, entries []Entry) []Entry {
	return appendVectorEntries(image, entries, 0x7fc0)
}

// AppendAutoVectorEntries uses the same cartridge header as ROM decoding.
func AppendAutoVectorEntries(image []byte, entries []Entry) []Entry {
	return appendVectorEntries(image, entries, rom.Image(image).Header().Offset)
}

func appendVectorEntries(image []byte, entries []Entry, header int) []Entry {
	if len(image) < header+0x40 {
		return entries
	}
	read := func(offset int) uint16 {
		return uint16(image[offset]) | uint16(image[offset+1])<<8
	}
	type vectorSeed struct {
		name     string
		pc       uint16
		variants []MX
	}
	allLiveWidths := []MX{
		{M: 0, X: 0},
		{M: 0, X: 1},
		{M: 1, X: 0},
		{M: 1, X: 1},
	}
	seeds := []vectorSeed{
		{name: "I_RESET", pc: read(header + 0x3c), variants: []MX{{M: 1, X: 1}}},
		{name: "I_NMI", pc: read(header + 0x2a), variants: allLiveWidths},
		{name: "I_IRQ", pc: read(header + 0x2e), variants: allLiveWidths},
	}
	starts := make(map[uint16]struct{}, len(entries))
	names := make(map[string]struct{}, len(entries))
	for _, entry := range entries {
		starts[entry.Start] = struct{}{}
		names[entry.Name] = struct{}{}
	}
	for _, seed := range seeds {
		if seed.pc == 0 || seed.pc == 0xffff {
			continue
		}
		if _, found := starts[seed.pc]; found {
			continue
		}
		if _, found := names[seed.name]; found {
			continue
		}
		for _, mx := range seed.variants {
			entries = append(entries, Entry{
				Name: seed.name, Start: seed.pc, EntryMX: mx,
			})
		}
		starts[seed.pc] = struct{}{}
		names[seed.name] = struct{}{}
	}
	return entries
}

package regionalmedia

import (
	"encoding/binary"
	"fmt"
)

// sequenceImage reads the five reviewed song-image blocks and their sample
// selection tail. Only block 2 is musical sequence data; the other blocks and
// tail must match the US host before a donor can be used with its audio driver.
func sequenceImage(rom []byte, source int) (blocks [][]byte, tail []byte, err error) {
	addresses := [...]uint16{0x2e48, 0x2f00, 0x1200, 0x2c30, 0x11fd}
	for _, address := range addresses {
		if source < 0 || source > len(rom)-4 {
			return nil, nil, fmt.Errorf("truncated song block header")
		}
		size := int(binary.LittleEndian.Uint16(rom[source:]))
		if size == 0 || binary.LittleEndian.Uint16(rom[source+2:]) != address || size > len(rom)-source-4 {
			return nil, nil, fmt.Errorf("unexpected song block shape")
		}
		source += 4
		blocks = append(blocks, rom[source:source+size])
		source += size
	}
	if source > len(rom)-4 || binary.LittleEndian.Uint16(rom[source:]) != 0 {
		return nil, nil, fmt.Errorf("missing song terminator")
	}
	count := int(rom[source+2])
	if count == 0 || count > len(rom)-source-3 {
		return nil, nil, fmt.Errorf("truncated song sample selection")
	}
	return blocks, rom[source+3 : source+3+count], nil
}

func japaneseSequences(rom []byte) ([]Resource, error) {
	var resources []Resource
	for i, source := range []int{0xc0000, 0xd31e1} {
		blocks, _, err := sequenceImage(rom, source)
		if err != nil {
			return nil, err
		}
		resources = append(resources, Resource{Sequence09 + uint32(i), append([]byte(nil), blocks[2]...)})
	}
	return resources, nil
}

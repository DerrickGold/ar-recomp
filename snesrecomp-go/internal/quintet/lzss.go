package quintet

import (
	"encoding/binary"
	"fmt"
)

// Decompress decodes native assets. Compression is one MSB-first bit stream, NOT byte-aligned tokens.
// Match reads overlap the 256-byte, space-filled ring starting at $EF.
// The returned count preserves the reference's stream-cursor convention:
// floor(bits_read/8)+1, excluding the two-byte decoded-size header. It must
// not be treated as an exact byte span for copying/deleting the source blob.
func Decompress(rom []byte, offset int) ([]byte, int, error) {
	if offset < 0 || offset > len(rom)-2 {
		return nil, 0, fmt.Errorf("truncated compressed header")
	}
	size := int(binary.LittleEndian.Uint16(rom[offset:]))
	start := offset + 2
	bitCursor := start * 8
	read := func(count int) (int, error) {
		value := 0
		for i := 0; i < count; i++ {
			if bitCursor/8 >= len(rom) {
				return 0, fmt.Errorf("truncated compressed token")
			}
			value = value<<1 | int(rom[bitCursor/8]>>(7-bitCursor%8)&1)
			bitCursor++
		}
		return value, nil
	}
	ring := [256]byte{}
	for i := range ring {
		ring[i] = 0x20
	}
	write := 0xef
	out := make([]byte, 0, size)
	for len(out) < size {
		literal, err := read(1)
		if err != nil {
			return nil, 0, err
		}
		value, err := read(8)
		if err != nil {
			return nil, 0, err
		}
		if literal != 0 {
			out = append(out, byte(value))
			ring[write] = byte(value)
			write = (write + 1) & 255
		} else {
			length, err := read(4)
			if err != nil {
				return nil, 0, err
			}
			for i := 0; i < length+2 && len(out) < size; i++ {
				b := ring[value]
				value = (value + 1) & 255
				ring[write] = b
				write = (write + 1) & 255
				out = append(out, b)
			}
		}
	}
	return out, bitCursor/8 - start + 1, nil
}

package tooling

import (
	"bytes"
	"testing"
)

func packQuintetBits(bits []uint8) []byte {
	output := make([]byte, (len(bits)+7)/8)
	for index, bit := range bits {
		output[index/8] |= bit << uint(7-index%8)
	}
	return output
}

func appendQuintetBits(bits []uint8, value, count int) []uint8 {
	for shift := count - 1; shift >= 0; shift-- {
		bits = append(bits, uint8(value>>shift)&1)
	}
	return bits
}

func TestDecompressQuintetLZSSLiteralsAndOverlappingMatch(t *testing.T) {
	var bits []uint8
	for _, value := range []byte{'A', 'B'} {
		bits = append(bits, 1)
		bits = appendQuintetBits(bits, int(value), 8)
	}
	// The two literals land at ring $EF/$F0. Copy four bytes from $EF; the
	// overlapping match must read back the bytes it is writing (ABAB).
	bits = append(bits, 0)
	bits = appendQuintetBits(bits, 0xef, 8)
	bits = appendQuintetBits(bits, 2, 4)
	stream := append([]byte{6, 0}, packQuintetBits(bits)...)
	output, consumed, err := DecompressQuintetLZSS(stream, 0, 0, true)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(output, []byte("ABABAB")) {
		t.Fatalf("output=%q", output)
	}
	if consumed < 2 {
		t.Fatalf("consumed=%d", consumed)
	}
}

func TestDecompressQuintetLZSSTruncated(t *testing.T) {
	_, _, err := DecompressQuintetLZSS([]byte{1, 0}, 0, 0, true)
	if err == nil {
		t.Fatal("expected truncated stream error")
	}
}

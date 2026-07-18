package rom

import (
	"fmt"
	"os"
)

// Image is a headerless SNES ROM image.
type Image []byte

// Load reads a ROM and removes a 512-byte copier header when present. This
// mirrors the historical Python recompiler's load_rom behavior.
func Load(path string) (Image, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read ROM: %w", err)
	}
	if len(data)%1024 == 512 {
		data = data[512:]
	}
	return Image(data), nil
}

// LoROMOffset maps a bank/address pair to a physical ROM offset.
func LoROMOffset(bank byte, addr uint16) (int, error) {
	if addr < 0x8000 {
		return 0, fmt.Errorf("address $%04X is outside LoROM range $8000-$FFFF", addr)
	}
	return int(bank&0x7f)*0x8000 + int(addr-0x8000), nil
}

// Slice returns length bytes from a LoROM address.
func (image Image) Slice(bank byte, addr uint16, length int) ([]byte, error) {
	if length < 0 {
		return nil, fmt.Errorf("negative ROM slice length %d", length)
	}
	offset, err := LoROMOffset(bank, addr)
	if err != nil {
		return nil, err
	}
	if offset > len(image) || length > len(image)-offset {
		return nil, fmt.Errorf("ROM slice $%02X:%04X+%d exceeds %d-byte image", bank, addr, length, len(image))
	}
	return image[offset : offset+length], nil
}

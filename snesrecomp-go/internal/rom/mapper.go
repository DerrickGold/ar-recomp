package rom

import "fmt"

// Mapper is a cartridge's ROM address layout, not a CPU bank or speed mode.
// Its zero value preserves headerless LoROM synthetic fixtures. Image bytes
// are immutable during analysis; no process-global mapper/cache is used.
type Mapper uint8

const (
	LoROM Mapper = iota
	HiROM
	ExHiROM
)

func (m Mapper) String() string {
	switch m {
	case HiROM:
		return "hirom"
	case ExHiROM:
		return "exhirom"
	default:
		return "lorom"
	}
}

// Header describes the mapping evidence shared by inspection and compilation.
type Header struct {
	Offset int
	Mapper Mapper
	Score  int
}

// Headers ranks the bounded standard header locations. A checksum pair is
// evidence for a header, not proof of the ROM's authenticity or code ownership.
func (image Image) Headers() []Header {
	var result []Header
	for _, h := range []Header{{Offset: 0x7fc0, Mapper: LoROM}, {Offset: 0xffc0, Mapper: HiROM}, {Offset: 0x40ffc0, Mapper: ExHiROM}} {
		if h.Offset+0x40 > len(image) {
			continue
		}
		result = append(result, image.scoreHeader(h))
	}
	return result
}

func (image Image) scoreHeader(h Header) Header {
	p := image[h.Offset:]
	assumed := h.Mapper
	switch p[0x15] & 0x2f {
	case 0x20:
		h.Mapper = LoROM
	case 0x21:
		h.Mapper = HiROM
	case 0x25:
		h.Mapper = ExHiROM
	}
	if (uint16(p[0x1c])|uint16(p[0x1d])<<8)^(uint16(p[0x1e])|uint16(p[0x1f])<<8) == 0xffff {
		h.Score += 8
	}
	for _, b := range p[:21] {
		if b != 0 && b != ' ' {
			h.Score += 2
			break
		}
	}
	if h.Mapper == assumed {
		h.Score += 4
	}
	if (uint16(p[0x3c]) | uint16(p[0x3d])<<8) >= 0x8000 {
		h.Score += 4
	}
	return h
}

func (image Image) Header() Header {
	best := Header{Offset: 0x7fc0, Mapper: LoROM, Score: -1}
	for _, h := range [3]Header{{Offset: 0x7fc0, Mapper: LoROM}, {Offset: 0xffc0, Mapper: HiROM}, {Offset: 0x40ffc0, Mapper: ExHiROM}} {
		if h.Offset+0x40 > len(image) {
			continue
		}
		h = image.scoreHeader(h)
		if h.Score > best.Score {
			best = h
		}
	}
	return best
}

func (image Image) Mapper() Mapper { return image.Header().Mapper }

// ValidateMapping fails closed for positively identified unsupported layouts.
// Headerless synthetic fixtures retain their historical LoROM interpretation.
func (image Image) ValidateMapping() error {
	h := image.Header()
	if h.Mapper == ExHiROM {
		return fmt.Errorf("unsupported cartridge mapper exhirom: LoROM and HiROM are supported; ROM repacking is not a substitute for mapper support")
	}
	if h.Offset+0x40 <= len(image) {
		p := image[h.Offset:]
		pair := (uint16(p[0x1c]) | uint16(p[0x1d])<<8) ^ (uint16(p[0x1e]) | uint16(p[0x1f])<<8)
		if pair == 0xffff {
			mode := p[0x15] & 0x2f
			if mode != 0x20 && mode != 0x21 {
				return fmt.Errorf("unsupported cartridge map mode $%02X at header $%X", p[0x15], h.Offset)
			}
			if p[0x16] > 2 {
				return fmt.Errorf("unsupported cartridge extension type $%02X at header $%X", p[0x16], h.Offset)
			}
		}
	}
	return nil
}

// Offset maps a CPU ROM address to a physical byte. It does not canonicalize
// program banks: PB-sensitive code must keep its CPU address. Existing LoROM
// admission remains conservative; HiROM adds full-ROM banks' lower halves.
func (m Mapper) Offset(bank byte, addr uint16) (int, error) {
	if m == LoROM {
		return LoROMOffset(bank, addr)
	}
	if m != HiROM {
		return 0, fmt.Errorf("unsupported cartridge mapper %s", m)
	}
	if bank == 0x7e || bank == 0x7f || (bank&0x7f < 0x40 && addr < 0x8000) {
		return 0, fmt.Errorf("address $%02X:%04X is not in a HiROM ROM window", bank, addr)
	}
	return int(bank&0x3f)*0x10000 + int(addr), nil
}

func (image Image) Offset(bank byte, addr uint16) (int, error) {
	mapper := image.Mapper()
	offset, err := mapper.Offset(bank, addr)
	if err != nil {
		return 0, err
	}
	// A power-of-two HiROM image mirrors through the cartridge's ROM windows.
	// Non-power-of-two images need a separately tested physical mirror policy.
	if mapper == HiROM && len(image) > 0 && len(image)&(len(image)-1) == 0 {
		offset &= len(image) - 1
	}
	return offset, nil
}

func (image Image) IsROM(bank byte, addr uint16) bool {
	offset, err := image.Offset(bank, addr)
	return err == nil && offset >= 0 && offset < len(image)
}

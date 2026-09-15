package rom

import "testing"

func hiROMFixture() Image {
	image := make(Image, 0x400000)
	copy(image[0xffc0:], "SYNTHETIC HIROM TEST  ")
	image[0xffd5], image[0xffd6] = 0x31, 2
	image[0xffdc], image[0xffdd] = 0xff, 0xff
	image[0xfffc], image[0xfffd] = 0x00, 0x81
	return image
}

func TestHiROMAddressWindows(t *testing.T) {
	image := hiROMFixture()
	if image.Mapper() != HiROM || image.Header().Offset != 0xffc0 {
		t.Fatal(image.Header())
	}
	if err := image.ValidateMapping(); err != nil {
		t.Fatal(err)
	}
	for _, tt := range []struct {
		bank   byte
		pc     uint16
		offset int
	}{
		{0x00, 0x8100, 0x8100}, {0x80, 0x8100, 0x8100},
		{0xc0, 0x8100, 0x8100}, {0x40, 0x8100, 0x8100},
		{0xc0, 0x0100, 0x0100}, {0x41, 0x1234, 0x11234},
		{0xf0, 0, 0x300000}, {0xff, 0xffff, 0x3fffff},
		{0xfe, 0x1234, 0x3e1234},
	} {
		offset, err := image.Offset(tt.bank, tt.pc)
		if err != nil || offset != tt.offset {
			t.Errorf("%02X:%04X: %X %v, want %X", tt.bank, tt.pc, offset, err, tt.offset)
		}
	}
	for _, bank := range []byte{0, 0x3f, 0x80, 0xbf, 0x7e, 0x7f} {
		if image.IsROM(bank, 0x6000) {
			t.Errorf("RAM/hardware is not ROM: %02X:6000", bank)
		}
	}
	if image.IsROM(0x7e, 0x9000) || image.IsROM(0x7f, 0xffff) {
		t.Fatal("WRAM banks accepted as ROM")
	}
	image[0x8100] = 0x78
	got, err := image.Slice(0, 0x8100, 1)
	if err != nil || got[0] != 0x78 {
		t.Fatalf("mapped slice: %v %v", got, err)
	}
}

func TestMapperIsPerImage(t *testing.T) {
	hi, lo := hiROMFixture(), make(Image, 0x8000)
	for i := 0; i < 10; i++ {
		if hi.Mapper() != HiROM || lo.Mapper() != LoROM {
			t.Fatal("mapper leaked between ROMs")
		}
		if off, _ := lo.Offset(0, 0x8100); off != 0x100 {
			t.Fatal(off)
		}
	}
}

func TestUnsupportedMappingFailsClosed(t *testing.T) {
	image := hiROMFixture()
	image[0xffd5] = 0x35
	if err := image.ValidateMapping(); err == nil {
		t.Fatal("ExHiROM accepted")
	}
	image[0xffd5] = 0x23
	if err := image.ValidateMapping(); err == nil {
		t.Fatal("coprocessor mapper accepted")
	}
}

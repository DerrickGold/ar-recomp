package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestMemoryPollWaitEmission(t *testing.T) {
	for _, tc := range []struct {
		name       string
		bytes      []byte
		address    string
		indexWidth bool
	}{
		{"absolute A", []byte{0xad, 0x34, 0x12, 0xd0, 0xfb, 0x6b}, "(((uint32)cpu->DB << 16) | 0x1234u)", false},
		{"direct A", []byte{0xa5, 0x34, 0xf0, 0xfc, 0x6b}, "(uint32)(uint16)(cpu->D + 0x34u)", false},
		{"long A", []byte{0xaf, 0x34, 0x12, 0x7f, 0x30, 0xfa, 0x6b}, "0x7f1234u", false},
		{"absolute X", []byte{0xae, 0x34, 0x12, 0x10, 0xfb, 0x6b}, "(((uint32)cpu->DB << 16) | 0x1234u)", true},
		{"absolute Y", []byte{0xac, 0x34, 0x12, 0xd0, 0xfb, 0x6b}, "(((uint32)cpu->DB << 16) | 0x1234u)", true},
	} {
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				image := make(rom.Image, 0x8000)
				copy(image, tc.bytes)
				r, err := EmitFunction(image, 0, 0x8000, m, x, FunctionOptions{Name: "Poll"})
				if err != nil {
					t.Fatal(err)
				}
				width := "2"
				if (!tc.indexWidth && m == 1) || (tc.indexWidth && x == 1) {
					width = "1"
				}
				want := "cpu_poll_wait(cpu, 0x008000u, " + tc.address + ", " + width + "u); goto L_8000_"
				if !strings.Contains(r.Source, want) || strings.Count(r.Source, "cpu_poll_wait(") != 1 {
					t.Fatalf("%s M%dX%d: missing %s\n%s", tc.name, m, x, want, r.Source)
				}
				line := ""
				for _, v := range strings.Split(r.Source, "\n") {
					if strings.Contains(v, "cpu_poll_wait(") {
						line = v
					}
				}
				if !strings.Contains(line, "if (cpu->_flag_") {
					t.Fatal("poll callback escaped taken-edge guard")
				}
			}
		}
	}
}

func TestMemoryPollWaitRefusals(t *testing.T) {
	for name, code := range map[string][]byte{
		"immediate":                   {0xa9, 1, 0xd0, 0xfc, 0x6b},
		"indexed":                     {0xbd, 0x34, 0x12, 0xd0, 0xfb, 0x6b},
		"indirect":                    {0xb2, 0x34, 0xd0, 0xfc, 0x6b},
		"read modify write":           {0xce, 0x34, 0x12, 0xd0, 0xfb, 0x6b},
		"BIT depends on A":            {0x2c, 0x34, 0x12, 0xd0, 0xfb, 0x6b},
		"changes index":               {0xad, 0x34, 0x12, 0xe8, 0xd0, 0xfa, 0x6b},
		"masked loop not modeled yet": {0xad, 0x34, 0x12, 0x29, 1, 0xd0, 0xf9, 0x6b},
		"carry unrelated to load":     {0xad, 0x34, 0x12, 0xb0, 0xfb, 0x6b},
		"forward edge":                {0xad, 0x34, 0x12, 0xd0, 0, 0x6b},
	} {
		t.Run(name, func(t *testing.T) {
			image := make(rom.Image, 0x8000)
			copy(image, code)
			r, err := EmitFunction(image, 0, 0x8000, 1, 1, FunctionOptions{Name: "NotPoll"})
			if err != nil {
				t.Fatal(err)
			}
			if strings.Contains(r.Source, "cpu_poll_wait(") {
				t.Fatal("non-poll loop scheduled")
			}
		})
	}
}

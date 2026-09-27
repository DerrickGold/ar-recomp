package decoder

import "testing"

func TestHLEBoundaryAppliesToEveryIncomingEdge(t *testing.T) {
	for _, test := range []struct {
		name string
		code []byte
		hook uint16
	}{
		{"fall", []byte{0xea, 0xe6, 0x10, 0x60}, 0x8001},
		{"conditional", []byte{0xd0, 2, 0xea, 0xea, 0xe6, 0x10, 0x60}, 0x8004},
		{"backward", []byte{0x80, 3, 0xe6, 0x10, 0x60, 0x80, 0xfb}, 0x8002},
		{"brk", []byte{0x00, 0, 0xe6, 0x10, 0x60}, 0x8002},
		{"saved status", []byte{0x08, 0xc2, 0x20, 0xe6, 0x10, 0x28, 0x60}, 0x8003},
	} {
		t.Run(test.name, func(t *testing.T) {
			image := bank0(map[uint16][]byte{0x8000: test.code})
			options := Options{
				HLEEntryPCs: map[uint16]struct{}{test.hook: {}},
				// An authored interception wins over an internal resume hint.
				InternalResumePCs: map[uint16]struct{}{test.hook: {}},
			}
			graph, err := DecodeFunction(image, 0, 0x8000, 1, 0, options)
			if err != nil {
				t.Fatal(err)
			}
			if len(graph.KeysAtPC(uint32(test.hook))) != 0 {
				t.Fatal("native instructions bypassed the HLE wrapper")
			}
			incoming := false
			for _, instruction := range graph.Instructions {
				for _, successor := range instruction.Successors {
					incoming = incoming || successor.PC == uint32(test.hook)
				}
			}
			if !incoming {
				t.Fatal("lost the external HLE continuation")
			}
			// The wrapper still needs its native body when the predicate is false.
			graph, err = DecodeFunction(image, 0, test.hook, 1, 0, options)
			if err != nil || len(graph.KeysAtPC(uint32(test.hook))) != 1 {
				t.Fatalf("lost native fallback at its own root: %v", err)
			}
		})
	}
}

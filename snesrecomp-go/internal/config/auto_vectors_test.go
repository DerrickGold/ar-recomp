package config

import "testing"

func TestAppendLoROMAutoVectorEntriesUsesLiveNativeInterruptWidths(t *testing.T) {
	image := make([]byte, 0x8000)
	image[0x7ffc], image[0x7ffd] = 0x00, 0x80
	image[0x7fea], image[0x7feb] = 0x10, 0x80
	image[0x7fee], image[0x7fef] = 0x20, 0x80
	entries := AppendLoROMAutoVectorEntries(image, nil)
	if len(entries) != 9 {
		t.Fatalf("auto-vector entries = %d, want 9", len(entries))
	}
	want := []Entry{
		{Name: "I_RESET", Start: 0x8000, EntryMX: MX{M: 1, X: 1}},
		{Name: "I_NMI", Start: 0x8010, EntryMX: MX{M: 0, X: 0}},
		{Name: "I_NMI", Start: 0x8010, EntryMX: MX{M: 0, X: 1}},
		{Name: "I_NMI", Start: 0x8010, EntryMX: MX{M: 1, X: 0}},
		{Name: "I_NMI", Start: 0x8010, EntryMX: MX{M: 1, X: 1}},
		{Name: "I_IRQ", Start: 0x8020, EntryMX: MX{M: 0, X: 0}},
		{Name: "I_IRQ", Start: 0x8020, EntryMX: MX{M: 0, X: 1}},
		{Name: "I_IRQ", Start: 0x8020, EntryMX: MX{M: 1, X: 0}},
		{Name: "I_IRQ", Start: 0x8020, EntryMX: MX{M: 1, X: 1}},
	}
	for index := range want {
		if entries[index] != want[index] {
			t.Fatalf("entry %d = %+v, want %+v", index, entries[index], want[index])
		}
	}
}

func TestAppendLoROMAutoVectorEntriesHonorsAuthoredOverride(t *testing.T) {
	image := make([]byte, 0x8000)
	image[0x7ffc], image[0x7ffd] = 0x00, 0x80
	image[0x7fea], image[0x7feb] = 0x10, 0x80
	image[0x7fee], image[0x7fef] = 0x20, 0x80
	authored := Entry{Name: "GameNMI", Start: 0x8010, EntryMX: MX{M: 1, X: 0}}
	entries := AppendLoROMAutoVectorEntries(image, []Entry{authored})
	if len(entries) != 6 {
		t.Fatalf("entries with authored NMI = %d, want 6", len(entries))
	}
	for _, entry := range entries[1:] {
		if entry.Start == 0x8010 || entry.Name == "I_NMI" {
			t.Fatalf("authored NMI was supplemented by auto-vector entry: %+v", entry)
		}
	}
}

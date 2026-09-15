package regen

import (
	"fmt"
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestDecodeOptionsUnionActiveSiblingVariants(t *testing.T) {
	// Exhaust every dormant/active mixture across two four-width siblings.
	// The current address must be excluded even if a different width is active.
	for mask := 0; mask < 256; mask++ {
		bank := &bankState{ID: 1, Config: &config.Config{}}
		repo := &repository{byBank: map[byte]*bankState{1: bank}, dormantEntryRoots: map[decoder.Variant]struct{}{}}
		for _, pc := range []uint16{0x8000, 0x8100, 0x8200} {
			for mx := 0; mx < 4; mx++ {
				e := config.Entry{Start: pc, EntryMX: config.MX{M: uint8(mx / 2), X: uint8(mx % 2)}}
				bank.Config.Entries = append(bank.Config.Entries, e)
				if pc != 0x8000 && mask&(1<<((int(pc)-0x8100)/0x100*4+mx)) != 0 {
					repo.dormantEntryRoots[entryVariant(1, e)] = struct{}{}
				}
			}
		}
		before := slices.Clone(bank.Config.Entries)
		want := map[uint16]struct{}{}
		if mask&15 != 15 {
			want[0x8100] = struct{}{}
		}
		if mask&240 != 240 {
			want[0x8200] = struct{}{}
		}
		for range 2 {
			options := repo.decodeOptions(bank, config.Entry{Start: 0x8000})
			if !reflect.DeepEqual(options.SiblingEntryPCs, want) {
				t.Fatalf("mask %02X: got %v want %v", mask, options.SiblingEntryPCs, want)
			}
			slices.Reverse(bank.Config.Entries)
		}
		if !reflect.DeepEqual(before, bank.Config.Entries) {
			t.Fatal("entries mutated")
		}
	}
}

func BenchmarkDecodeOptionsSiblingInventory(b *testing.B) {
	for _, count := range []int{128, 1024, 4096} {
		b.Run(fmt.Sprint(count), func(b *testing.B) {
			bank := &bankState{ID: 1, Config: &config.Config{}}
			for n := 0; n < count; n++ {
				bank.Config.Entries = append(bank.Config.Entries, config.Entry{Start: uint16(0x8000 + n), EntryMX: config.MX{}})
			}
			repo := &repository{byBank: map[byte]*bankState{1: bank}}
			b.ReportAllocs()
			b.ResetTimer()
			for b.Loop() {
				if len(repo.decodeOptions(bank, bank.Config.Entries[0]).SiblingEntryPCs) != count-1 {
					b.Fatal("missing siblings")
				}
			}
		})
	}
}

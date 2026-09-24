// Package regionalmedia extracts reviewed regional graphics and note sequences.
// It does not copy executable code, sprite hitboxes or animation programs.
package regionalmedia

import (
	"fmt"

	"github.com/DerrickGold/ar-recomp/installer/internal/gameassets"
	"github.com/DerrickGold/ar-recomp/installer/internal/gamerom"
	"github.com/DerrickGold/ar-recomp/installer/internal/quintet"
)

const (
	DeathHeimBG2        uint32 = 1
	ActionHealthPickup  uint32 = 2
	ActionSpellHUD      uint32 = 3
	TownFollowerSymbols uint32 = 4
	TownLairSymbols     uint32 = 5
	TownPyramidDetail   uint32 = 6
	TitleBackground     uint32 = 7
	Sequence09          uint32 = 8
	Sequence12          uint32 = 9
	ActorArt            uint32 = 10
)

type Resource struct {
	ID    uint32
	Bytes []byte
}
type Extraction struct {
	Release   gamerom.Release
	Resources []Resource
}

func Extract(rom []byte) (*Extraction, error) {
	release, err := gamerom.Identify(rom)
	if err != nil {
		return nil, err
	}
	entries, err := gameassets.Script(rom)
	if err != nil {
		return nil, err
	}
	result := &Extraction{Release: release}
	for _, entry := range entries {
		if entry.Mode != 7 || entry.Submode != 1 {
			continue
		}
		for _, command := range entry.Commands {
			p := command.Operands
			if command.Code&0x80 == 0 || p[2] != 0x10 {
				continue
			}
			if p[0] != 0 || p[1] != 0x10 || len(result.Resources) != 0 {
				return nil, fmt.Errorf("unexpected Death Heim BG2 resource declaration")
			}
			source := int(p[3]) | int(p[4])<<8 | int(p[5])<<16
			data, _, err := quintet.Decompress(rom, source)
			if err != nil {
				return nil, err
			}
			if len(data) != 8192 {
				return nil, fmt.Errorf("Death Heim BG2 has %d bytes", len(data))
			}
			result.Resources = append(result.Resources, Resource{DeathHeimBG2, data})
		}
	}
	if len(result.Resources) != 1 {
		return nil, fmt.Errorf("missing Death Heim BG2 resource")
	}
	if release.ID == "eu-en" || release.ID == "de" || release.ID == "fr" {
		// Action item1 is06:A880, NOT Story's06:A080 extra-life icon.
		// HUD windows at06:AC00 use128-byte strides; zero selects index4.
		// Include its adjacent128 bytes: room entry uploads256, not128.
		result.Resources = append(result.Resources,
			Resource{ActionHealthPickup, append([]byte(nil), rom[0x32880:0x32900]...)},
			Resource{ActionSpellHUD, append([]byte(nil), rom[0x32c00:0x32f00]...)})
	}
	if release.ID == "jp" {
		// Sparse compatible character groups, not whole regional CHR banks.
		// Native compositions, palettes and act-completion bank selection stay
		// US-owned. Adjacent regional/unused graphics are deliberately excluded.
		characters := func(base int, tiles ...int) []byte {
			out := make([]byte, 0, len(tiles)*32)
			for _, tile := range tiles {
				out = append(out, rom[base+tile*32:base+(tile+1)*32]...)
			}
			return out
		}
		result.Resources = append(result.Resources,
			Resource{TownFollowerSymbols, characters(0x68000, 0x1a2, 0x1a3, 0x1b2, 0x1b3, 0x1a8, 0x1a9, 0x1b8, 0x1b9)},
			Resource{TownLairSymbols, characters(0x60000, 0x1ce, 0x1cf, 0x1de, 0x1df)},
			Resource{TownPyramidDetail, characters(0x60000, 0x113)})
		// One coherent Mode 7 resource: 16 KiB characters, 16 KiB map
		// (without its two dimension bytes), then 128 BGR555 colors.
		// Font, menu, copyright scripts and palette animation stay independent.
		title := make([]byte, 0, 33024)
		title = append(title, rom[0x58300:0x5c300]...)
		title = append(title, rom[0x28e79:0x2ce79]...)
		title = append(title, rom[0xe1cdd:0xe1ddd]...)
		result.Resources = append(result.Resources, Resource{TitleBackground, title})
		sequences, err := japaneseSequences(rom)
		if err != nil {
			return nil, err
		}
		result.Resources = append(result.Resources, sequences...)
		actors, err := japaneseActorArt(rom)
		if err != nil {
			return nil, err
		}
		result.Resources = append(result.Resources, Resource{ActorArt, actors})
	}
	return result, nil
}

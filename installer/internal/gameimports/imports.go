// Package gameimports owns the Workshop's game-content import catalog. Each
// importer owns ROM validation, extraction, installation and availability.
// New content sets (including a complete game) register here without adding a
// game-specific HTTP endpoint or changing the Workshop's upload controller.
// Installers store their content beneath game-assets/imports, which travels
// with packaged games and existing-installation imports.
package gameimports

import (
	"fmt"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
)

type Item struct {
	ID       string `json:"id"`
	LabelKey string `json:"labelKey"`
	HelpKey  string `json:"helpKey"`
	NextKey  string `json:"nextKey"`
	Status   string `json:"status"`
}

type importer struct {
	item            Item
	maximumROMBytes int64
	extract         func([]byte) ([]byte, error)
	install         func(string, []byte) (bool, error)
	status          func(string) string
}

var importers = []importer{
	{
		item:            Item{ID: "ar2-master", LabelKey: "builder.imports.ar2_master.label", HelpKey: "builder.imports.ar2_master.help", NextKey: "builder.imports.ar2_master.next"},
		maximumROMBytes: ar2master.ROMBytes + 512,
		extract:         ar2master.Extract,
		install:         ar2master.Install,
		status:          ar2master.Status,
	},
}

func List(root string) []Item {
	items := make([]Item, 0, len(importers))
	for _, entry := range importers {
		item := entry.item
		item.Status = entry.status(root)
		items = append(items, item)
	}
	return items
}

func MaximumROMBytes() int64 {
	var maximum int64
	for _, entry := range importers {
		if entry.maximumROMBytes > maximum {
			maximum = entry.maximumROMBytes
		}
	}
	return maximum
}

// Import returns only after a validated content set has been installed. A
// filename supplied by the browser never determines a destination path.
func Import(root, id string, rom []byte) (bool, error) {
	for _, entry := range importers {
		if entry.item.ID != id {
			continue
		}
		if int64(len(rom)) > entry.maximumROMBytes {
			return false, fmt.Errorf("ROM exceeds the size supported by this import")
		}
		pack, err := entry.extract(rom)
		if err != nil {
			return false, err
		}
		return entry.install(root, pack)
	}
	return false, fmt.Errorf("select a supported content import")
}

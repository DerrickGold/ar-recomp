// Package gameimports owns the Workshop's game-content import catalog. Each
// importer owns ROM validation, extraction, installation and availability.
// New content sets (including a complete game) register here without adding a
// game-specific HTTP endpoint or changing the Workshop's upload controller.
// Installers store their content beneath game-assets/imports, which travels
// with packaged games and existing-installation imports.
package gameimports

import (
	"fmt"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2game"
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
	preview         bool
	extract         func([]byte) ([]byte, error)
	install         func(string, []byte) (bool, error)
	status          func(string) string
}

var importers = []importer{
	{
		item:            Item{ID: "ar2-world", LabelKey: "builder.imports.ar2_world.label", HelpKey: "builder.imports.ar2_world.help", NextKey: "builder.imports.ar2_world.next"},
		maximumROMBytes: ar2master.ROMBytes + 512,
		preview:         true,
		extract:         ar2game.Extract,
		install:         ar2game.Install,
		status:          ar2game.Status,
	},
	{
		item:            Item{ID: "ar2-master", LabelKey: "builder.imports.ar2_master.label", HelpKey: "builder.imports.ar2_master.help", NextKey: "builder.imports.ar2_master.next"},
		maximumROMBytes: ar2master.ROMBytes + 512,
		preview:         true,
		extract:         ar2master.Extract,
		install:         ar2master.Install,
		status:          ar2master.Status,
	},
}

func List(root string) []Item {
	return list(root, true)
}

// WorkshopList does not promote unfinished imports to a fresh installation.
// An existing pack remains visible for prototype users to inspect or repair;
// explicit ROM selection and the developer CLI can still install previews.
func WorkshopList(root string) []Item {
	return list(root, false)
}

func list(root string, includePreviews bool) []Item {
	items := make([]Item, 0, len(importers))
	for _, entry := range importers {
		item := entry.item
		item.Status = entry.status(root)
		if entry.preview && !includePreviews && item.Status == "missing" {
			continue
		}
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

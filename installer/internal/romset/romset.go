// Package romset prepares a build's unordered ROM selection before installing
// anything. Identity comes from supported hashes, never filenames or order.
package romset

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2game"
	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
	"github.com/DerrickGold/ar-recomp/installer/internal/ar2rom"
	"github.com/DerrickGold/ar-recomp/installer/internal/gamerom"
	"github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

type Input struct {
	Name string
	Data []byte
}
type Plan struct {
	Baseline      []byte
	Releases      []string
	master, world []byte
	media         [][]byte
	sources       []*localization.AuthorProject
}

const SequelID = "ar2-us"

// Identify checks the supported clean ROM identities without extracting content.
func Identify(data []byte) (string, []byte, error) {
	if normalized, err := ar2rom.Normalize(data); err == nil {
		return SequelID, normalized, nil
	}
	if len(data) == gamerom.Size+512 {
		data = data[512:]
	}
	release, err := gamerom.Identify(data)
	return release.ID, data, err
}

func Prepare(inputs []Input) (*Plan, error) {
	if len(inputs) == 0 || len(inputs) > 6 {
		return nil, fmt.Errorf("select between one and six supported game ROMs")
	}
	plan := &Plan{}
	seen := map[string]bool{}
	for _, input := range inputs {
		id, data, err := Identify(input.Data)
		if err != nil {
			return nil, fmt.Errorf("%s: unsupported ROM; select a clean supported ActRaiser ROM", input.Name)
		}
		if seen[id] {
			continue
		}
		seen[id] = true
		if id == SequelID {
			plan.Releases = append(plan.Releases, "ActRaiser 2 (USA): optional content")
			plan.master, err = ar2master.Extract(data)
			if err != nil {
				return nil, err
			}
			plan.world, err = ar2game.Extract(data)
			if err != nil {
				return nil, err
			}
			continue
		}
		plan.Releases = append(plan.Releases, "ActRaiser ("+id+"): regional graphics, music and text source")
		if id == "us" {
			plan.Baseline = data
		}
		extraction, err := regionalmedia.Extract(data)
		if err != nil {
			return nil, err
		}
		media, err := regionalmedia.Pack(extraction)
		if err != nil {
			return nil, err
		}
		plan.media = append(plan.media, media)
		if id == "us" {
			continue
		} // Build prepares the runtime baseline.
		decoder, err := localization.NewDecoder(data)
		if err != nil {
			return nil, err
		}
		pack, err := decoder.BuildNativeAuthorPack(decoder.NativeSourceMetadata())
		if err != nil {
			return nil, err
		}
		project, err := localization.NewSourceProject(pack)
		if err != nil {
			return nil, err
		}
		plan.sources = append(plan.sources, project)
	}
	return plan, nil
}

// Install uses each content owner's bounded, atomic installer. Previously
// edited language sources are preserved; a repeated ROM selection is harmless.
func (plan *Plan) Install(root string) error {
	if plan.world != nil {
		if _, err := ar2game.Install(root, plan.world); err != nil {
			return err
		}
		if _, err := ar2master.Install(root, plan.master); err != nil {
			return err
		}
	}
	for _, data := range plan.media {
		if _, _, err := regionalmedia.Install(root, data, true); err != nil {
			return err
		}
	}
	if len(plan.sources) == 0 {
		return nil
	}
	store, err := localization.NewAuthorStore(filepath.Join(root, "game-assets", "languages", "projects"))
	if err != nil {
		return err
	}
	for _, source := range plan.sources {
		id := source.Pack().Manifest().Metadata().ID
		if _, err := store.Open(id); err == nil {
			continue
		} else if !os.IsNotExist(err) {
			return err
		}
		if err := store.Save(source, ""); err != nil {
			return err
		}
	}
	return nil
}

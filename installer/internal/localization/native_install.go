package localization

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

// NativeSourceMetadata is shared by build and GUI callers. Locale describes
// prose, while SourceProfile and Target describe its game semantic contract.
func (d *Decoder) NativeSourceMetadata() PackMetadata {
	target := "reference-only"
	id := "source." + d.profile.ID
	if d.profile.ID == "us" {
		target, id = "us-runtime", "native-us"
	}
	return PackMetadata{ID: id, Locale: d.profile.Locale, Name: "Native " + d.profile.Label,
		Autonym: d.profile.Label, Author: "Local ROM extraction", License: "Local use only",
		Direction: "auto", Target: target, SourceProfile: d.profile.ID, Fallback: "native-us", Coverage: "complete"}
}

// EnsureNativeUSSource preserves existing messages, supplementing older native
// baselines with newly transcribed graphical labels. Community packs are never
// upgraded here. Build/editor share this path without Python or profile guessing.
func EnsureNativeUSSource(directory string, rom []byte) (*AuthorPack, error) {
	if pack, err := OpenNativeUSSource(directory); err != nil {
		return nil, err
	} else if pack != nil {
		var credits []AuthorMessage
		if len(rom) != 0 {
			d, err := NewDecoder(rom)
			if err != nil {
				return nil, err
			}
			if d.ReleaseID() != "us" {
				return nil, fmt.Errorf("the runtime baseline requires the US ROM")
			}
			entries, err := d.assetScript()
			if err != nil {
				return nil, err
			}
			pages, err := d.nativeCredits(entries)
			if err != nil {
				return nil, err
			}
			credits = nativeCreditsMessages(pages)
		}
		return supplementNativeUSSource(directory, pack, credits...)
	}
	d, err := NewDecoder(rom)
	if err != nil {
		return nil, err
	}
	if d.ReleaseID() != "us" {
		return nil, fmt.Errorf("the runtime baseline requires the US ROM")
	}
	pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
	if err != nil {
		return nil, err
	}
	return InstallNativeUSSource(directory, pack)
}

// OpenNativeUSSource validates a generated baseline without extracting or
// writing anything. A missing manifest returns (nil, nil); incompatible or
// incomplete packs return an error. Build reuse and GUI readiness agree here.
func OpenNativeUSSource(directory string) (*AuthorPack, error) {
	if _, err := os.Lstat(filepath.Join(directory, "pack.ini")); err == nil {
		pack, err := OpenAuthorPack(directory)
		if err != nil {
			return nil, err
		}
		m := pack.Manifest().Metadata()
		if m.ID != "native-us" || m.SourceProfile != "us" || m.Target != "us-runtime" || m.Coverage != "complete" {
			return nil, fmt.Errorf("existing native US source is incompatible; move it aside explicitly before extracting")
		}
		return pack, nil
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	return nil, nil
}

// InstallNativeUSSource shares the build's non-overwrite policy with a GUI
// which already holds its validated extraction. No second ROM scan is needed.
func InstallNativeUSSource(directory string, pack *AuthorPack) (*AuthorPack, error) {
	if pack == nil {
		return nil, fmt.Errorf("native source is required")
	}
	m := pack.Manifest().Metadata()
	if m.ID != "native-us" || m.SourceProfile != "us" || m.Target != "us-runtime" || m.Coverage != "complete" {
		return nil, fmt.Errorf("incompatible native US source")
	}
	if old, err := OpenNativeUSSource(directory); err != nil {
		return nil, err
	} else if old != nil {
		var credits []AuthorMessage
		for page := 0; page < endingPageCount; page++ {
			id := creditPageID("us", page)
			if ops, err := pack.MessageOperations(id); id != "" && err == nil {
				// Supplemental content is interpreted once by the destination
				// version. Do not feed v2 inline styles through the v1 emitter.
				for i := range ops {
					ops[i].Style = texttemplate.Style{}
				}
				credits = append(credits, AuthorMessage{ID: id, Operations: ops})
			}
		}
		return supplementNativeUSSource(directory, old, credits...)
	}
	p, err := NewSourceProject(pack)
	if err != nil {
		return nil, err
	}
	if _, err = InstallAuthorProject(directory, p, false); err != nil {
		return nil, err
	}
	return pack, nil
}

func supplementNativeUSSource(directory string, old *AuthorPack, supplemental ...AuthorMessage) (*AuthorPack, error) {
	pack := old
	for _, label := range append(append(append(nativeHUDLabels("us"), nativeHUDValues()...), nativeWorldLabel()), supplemental...) {
		if view, found := pack.workspace.Message(label.ID); found && view.Present {
			continue
		}
		if pack.manifest.Version() == 2 {
			label = inferV1Appearance(label, "us")
			var err error
			pack, err = addNativeTreatmentDefinitions(pack, label)
			if err != nil {
				return nil, err
			}
		}
		script, err := EmitAuthorScriptVersion([]AuthorMessage{label}, "supplement.artext", pack.manifest.Version())
		if err != nil {
			return nil, err
		}
		body, _ := script.Body(label.ID)
		pack, err = pack.AddMessage(label.ID, pack.manifest.Sources()[0],
			body, TranslationNotStarted)
		if err != nil {
			return nil, err
		}
	}
	if pack.manifest.Version() == 1 {
		var err error
		pack, _, err = upgradeAuthorPackV2(pack)
		if err != nil {
			return nil, err
		}
	}
	if pack == old {
		return old, nil
	}
	project, err := NewSourceProject(pack)
	if err != nil {
		return nil, err
	}
	// The atomic manifest selects a complete new version; old source files and
	// root progress remain intact and recoverable. Never replace existing IDs.
	if _, err := installAuthorProject(directory, project, true, old.RuntimeRevision()); err != nil {
		return nil, err
	}
	return OpenNativeUSSource(directory)
}

func addNativeTreatmentDefinitions(pack *AuthorPack, message AuthorMessage) (*AuthorPack, error) {
	if message.Appearance.Style.Font == "hud" {
		fonts := pack.manifest.Fonts()
		if !slices.ContainsFunc(fonts.Roles, func(role PackFontRole) bool { return role.Name == "hud" }) {
			fonts.Roles = append(fonts.Roles, PackFontRole{Name: "hud", Primary: fonts.Primary, Fallback: slices.Clone(fonts.Fallback)})
			var err error
			pack, err = pack.WithFonts(fonts, nil)
			if err != nil {
				return nil, err
			}
		}
	}
	needed := map[string]bool{message.Appearance.Style.Treatment: true}
	for _, op := range message.Operations {
		needed[op.Style.Treatment] = true
	}
	for _, treatment := range pack.Treatments() {
		delete(needed, treatment.Definition.Name)
	}
	definitions := v1TreatmentDefinitions(needed)
	if len(definitions) == 0 {
		return pack, nil
	}
	source := pack.workspace.scripts[0]
	prefix, err := EmitAuthorScriptVersion(nil, source.path, 2, definitions...)
	if err != nil {
		return nil, err
	}
	replacement, err := ParseAuthorScriptVersion(prefix.text+source.text, source.path, 2)
	if err != nil {
		return nil, err
	}
	scripts := append([]*AuthorScript{}, pack.workspace.scripts...)
	scripts[0] = replacement
	workspace, err := newAuthorWorkspace(pack.workspace.profile, pack.workspace.coverage, scripts, pack.workspace.progress.text)
	if err != nil {
		return nil, err
	}
	return pack.withWorkspace(workspace)
}

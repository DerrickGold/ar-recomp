package localization

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"

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

// EnsureNativeUSSource preserves existing messages while supplementing newly
// discovered native routes. Community packs are never upgraded here.
func EnsureNativeUSSource(directory string, rom []byte) (*AuthorPack, error) {
	old, err := OpenNativeUSSource(directory)
	if err != nil {
		return nil, err
	}
	if old != nil && len(rom) == 0 {
		return supplementNativeUSSource(directory, old)
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
		var supplemental []AuthorMessage
		for _, route := range authorContracts.ordered {
			if view, found := old.workspace.Message(route.ID); found && view.Present &&
				!strings.HasPrefix(route.ID, "dialogue.event.relay.") {
				continue
			}
			if view, found := pack.workspace.Message(route.ID); !found || !view.Present {
				continue
			}
			ops, err := pack.MessageOperations(route.ID)
			if err != nil {
				return nil, err
			}
			// Apply the destination version's native appearance once; avoid
			// passing v2 inline styles through the v1 compatibility emitter.
			for i := range ops {
				ops[i].Style = texttemplate.Style{}
			}
			supplemental = append(supplemental, AuthorMessage{ID: route.ID, Operations: ops})
		}
		return supplementNativeUSSource(directory, old, supplemental...)
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
	for _, label := range append(append(append(append(nativeHUDLabels("us"), nativeHUDValues()...), nativeWorldLabel()), nativeHelpMessages()...), supplemental...) {
		if view, found := pack.workspace.Message(label.ID); found && view.Present {
			if strings.HasPrefix(label.ID, "dialogue.event.relay.") {
				original, err := pack.ResolvedMessage(label.ID)
				if err != nil {
					return nil, err
				}
				if changed, ok := nativeRelaySpacingUpgrade(original, label); ok {
					script, err := EmitAuthorScriptVersion([]AuthorMessage{changed}, "native-spacing.artext", pack.manifest.Version())
					if err != nil {
						return nil, err
					}
					body, _ := script.Body(label.ID)
					pack, err = pack.EditMessage(label.ID, body, view.Status)
					if err != nil {
						return nil, err
					}
				}
			}
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
	for _, route := range authorContracts.ordered {
		id := route.ID
		if id != "dialogue.event.wrapper_05.call_00.source_00" && id != "title.start_prompt" &&
			!slices.ContainsFunc(route.Allowed, func(name string) bool { return AbilityNameSource(name) != "" }) {
			continue
		}
		if view, found := pack.workspace.Message(id); found && view.Present {
			message, err := pack.ResolvedMessage(id)
			if err != nil {
				return nil, err
			}
			changed := nativeAbilityReferences(nativeButtonReferences(message.Operations, id), id)
			if !slices.EqualFunc(changed, message.Operations, func(a, b AuthorOperation) bool { return a == b }) {
				message.Operations = changed
				script, err := EmitAuthorScriptVersion([]AuthorMessage{message}, "native-references.artext", pack.manifest.Version())
				if err != nil {
					return nil, err
				}
				body, _ := script.Body(id)
				pack, err = pack.EditMessage(id, body, view.Status)
				if err != nil {
					return nil, err
				}
			}
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

// Repair only the old, single-run relay export when a fresh ROM extraction
// supplies verified fragment separators. Preserve edited wording, explicit
// layout, default appearance, inline treatment and translation progress.
func nativeRelaySpacingUpgrade(original, fresh AuthorMessage) (AuthorMessage, bool) {
	if len(original.Operations) != 2 || original.Operations[0].Op != "text" || original.Operations[1].Op != "end" {
		return AuthorMessage{}, false
	}
	var joined strings.Builder
	ops := slices.Clone(fresh.Operations)
	for i := range ops {
		switch ops[i].Op {
		case "text":
			joined.WriteString(ops[i].Value)
			ops[i].Style = original.Operations[0].Style
		case "preferred_line":
		case "end":
			ops[i] = original.Operations[1]
		default:
			return AuthorMessage{}, false
		}
	}
	withoutSpaces := func(text string) string {
		return strings.Map(func(r rune) rune {
			if sourceSpace(r) {
				return -1
			}
			return r
		}, text)
	}
	if withoutSpaces(original.Operations[0].Value) != withoutSpaces(joined.String()) || slices.Equal(original.Operations, ops) {
		return AuthorMessage{}, false
	}
	original.Operations = ops
	return original, true
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

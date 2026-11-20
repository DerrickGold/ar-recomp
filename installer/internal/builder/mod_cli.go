package builder

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

// RunMods is shared by the backend CLI and the packaged desktop entry point.
func RunMods(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 || args[0] == "help" || args[0] == "--help" || args[0] == "-h" {
		fmt.Fprintln(stdout, "Usage: actraiser-builder mods <inspect|install|rescan|list|enable|disable> [--output-dir PATH] [--mods-dir PATH] [--json] [FILE or ID]")
		if len(args) == 0 {
			return 2
		}
		return 0
	}
	action := args[0]
	flags := flag.NewFlagSet("mods "+action, flag.ContinueOnError)
	flags.SetOutput(stderr)
	root := flags.String("output-dir", "", "selected writable game output (install can create it)")
	jsonOutput := flags.Bool("json", false, "emit machine-readable JSON")
	var directories []string
	flags.Func("mods-dir", "register an external mod directory (repeatable)", func(directory string) error {
		directories = append(directories, directory)
		return nil
	})
	if err := flags.Parse(args[1:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	fail := func(err error, code int) int {
		if *jsonOutput {
			_ = json.NewEncoder(stdout).Encode(map[string]string{"error": err.Error()})
		} else {
			fmt.Fprintf(stderr, "mods: %v\n", err)
		}
		return code
	}
	required := 0
	switch action {
	case "inspect", "install", "enable", "disable":
		required = 1
	case "list", "rescan":
	default:
		return fail(fmt.Errorf("unknown mods command %q", action), 2)
	}
	if flags.NArg() != required || (action != "inspect" && *root == "") {
		return fail(fmt.Errorf("invalid arguments; see mods help"), 2)
	}
	var source *os.File
	if action == "inspect" || action == "install" {
		if !strings.EqualFold(filepath.Ext(flags.Arg(0)), ".o2r") {
			return fail(fmt.Errorf("select an .o2r mod pack"), 2)
		}
		var err error
		source, err = openModFile(flags.Arg(0))
		if err != nil {
			return fail(err, 1)
		}
		defer source.Close()
	}
	if action == "inspect" {
		pack, err := inspectMod(source)
		if err != nil {
			return fail(err, 1)
		}
		if *jsonOutput {
			if err := json.NewEncoder(stdout).Encode(modCLIInfo(pack.modMetadata, pack.modPayload)); err != nil {
				return fail(err, 1)
			}
		} else {
			fmt.Fprintf(stdout, "%s — %s %s (%s)\n", pack.ID, pack.Name, pack.Version, pack.Type)
		}
		return 0
	}
	var change *modChange
	if action == "enable" || action == "disable" {
		change = &modChange{Action: action, Pack: flags.Arg(0)}
	}
	var archive io.Reader
	if source != nil {
		if err := os.MkdirAll(*root, 0755); err != nil {
			return fail(err, 1)
		}
		archive = source
	}
	catalog, err := updateModCatalog(*root, directories, archive, change)
	if err != nil {
		return fail(err, 1)
	}
	if *jsonOutput {
		if err := json.NewEncoder(stdout).Encode(modCLIResult(catalog)); err != nil {
			return fail(err, 1)
		}
		return 0
	}
	if len(catalog.Packs) == 0 {
		fmt.Fprintln(stdout, "No installed mods.")
	}
	for _, pack := range catalog.Packs {
		state := "disabled"
		switch {
		case pack.Error != "":
			state = pack.Error
		case pack.Enabled:
			state = "enabled"
		}
		fmt.Fprintf(stdout, "%s\t%s\t%s\n", pack.ID, pack.Filename, state)
	}
	for _, slot := range catalog.Slots {
		if len(slot.Candidates) > 1 {
			fmt.Fprintf(stdout, "Conflict: %s (%s)\n", slot.ID, strings.Join(slot.Candidates, ", "))
		}
	}
	return 0
}

type modCLIPack struct {
	modMetadata
	ID   string `json:"id"`
	Game string `json:"game"`
	Type string `json:"type"`
}
type modCLIInstallation struct {
	modCLIPack
	Filename string `json:"filename"`
	External bool   `json:"external"`
	Enabled  bool   `json:"enabled"`
	Error    string `json:"error,omitempty"`
}
type modCLIConflict struct {
	Target     string   `json:"target"`
	Candidates []string `json:"candidates"`
	Winner     string   `json:"winner,omitempty"`
}
type modCLIState struct {
	Packs     []modCLIInstallation `json:"packs"`
	Conflicts []modCLIConflict     `json:"conflicts"`
}

func modCLIInfo(metadata modMetadata, payload modPayload) modCLIPack {
	return modCLIPack{modMetadata: metadata, ID: payload.ID, Game: payload.Game, Type: payload.Type}
}

func modCLIResult(catalog modCatalog) modCLIState {
	result := modCLIState{
		Packs:     make([]modCLIInstallation, 0, len(catalog.Packs)),
		Conflicts: []modCLIConflict{},
	}
	for _, pack := range catalog.Packs {
		result.Packs = append(result.Packs, modCLIInstallation{
			modCLIPack: modCLIInfo(pack.modMetadata, pack.modPayload),
			Filename:   pack.Filename, External: pack.External, Enabled: pack.Enabled, Error: pack.Error,
		})
	}
	for _, slot := range catalog.Slots {
		if len(slot.Candidates) > 1 {
			result.Conflicts = append(result.Conflicts, modCLIConflict{
				Target: slot.ID, Candidates: slot.Candidates, Winner: slot.Winner,
			})
		}
	}
	return result
}

func inspectMod(file *os.File) (modArchive, error) {
	stat, err := file.Stat()
	if err != nil {
		return modArchive{}, err
	}
	pack, err := readModArchive(file, stat.Size())
	if err != nil {
		return modArchive{}, err
	}
	if pack.Type == languageModType {
		_, err := lk.ReadLanguageMod(file, stat.Size())
		return pack, err
	}
	for _, name := range pack.assetPaths() {
		if _, err := pack.copyAsset(io.Discard, name); err != nil {
			return modArchive{}, fmt.Errorf("%s: %w", name, err)
		}
	}
	return pack, nil
}

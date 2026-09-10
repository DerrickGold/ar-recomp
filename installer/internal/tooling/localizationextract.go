package tooling

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

// RunLocalizationExtractCommand uses the same decoder/author core as the build
// and GUI. Evidence JSON is deliberately separate from playable pack archives.
func RunLocalizationExtractCommand(args []string, cwd string, output io.Writer) error {
	flags := flag.NewFlagSet("localization-extract", flag.ContinueOnError)
	flags.SetOutput(output)
	romPath := flags.String("rom", "", "path to an exact supported regional ROM")
	outPath := flags.String("out", "", "new output file (never overwritten)")
	format := flags.String("format", "pack", "pack (private workshop ZIP), catalog (evidence JSON), runtime-routes or compose-routes (US address-only JSON)")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 || *romPath == "" || *outPath == "" || (*format != "pack" && *format != "catalog" && *format != "runtime-routes" && *format != "compose-routes") {
		return fmt.Errorf("usage: actraiser-builder localization-extract --rom game.sfc --out new-file [--format pack|catalog|runtime-routes|compose-routes]")
	}
	d, err := readLocalizationROM(resolveLocalizationPath(cwd, *romPath))
	if err != nil {
		return err
	}
	var write func(io.Writer) error
	if *format == "pack" {
		pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
		if err != nil {
			return err
		}
		project, err := localization.NewSourceProject(pack)
		if err != nil {
			return err
		}
		write = func(w io.Writer) error { return project.WriteArchive(w, "backup") }
	} else {
		var evidence any
		switch *format {
		case "catalog":
			evidence, err = d.BuildNativeEvidence()
		case "runtime-routes":
			evidence, err = d.USRuntimeDialogueRoutes()
		case "compose-routes":
			evidence, err = d.USRuntimeComposeRoutes()
		}
		if err != nil {
			return err
		}
		write = func(w io.Writer) error {
			encoder := json.NewEncoder(w)
			encoder.SetIndent("", "  ")
			encoder.SetEscapeHTML(false)
			return encoder.Encode(evidence)
		}
	}
	path := resolveLocalizationPath(cwd, *outPath)
	if err := writeNewLocalizationFile(path, write); err != nil {
		return err
	}
	fmt.Fprintf(output, "Extracted %s %s to %s. Local reference only; source packs are not publications or installed automatically.\n", d.ReleaseID(), *format, path)
	return nil
}

func resolveLocalizationPath(cwd, path string) string {
	if filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(cwd, path)
}

func readLocalizationROM(path string) (*localization.Decoder, error) {
	input, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	data, err := io.ReadAll(io.LimitReader(input, (1<<20)+1))
	closeErr := input.Close()
	if err != nil {
		return nil, err
	}
	if closeErr != nil {
		return nil, closeErr
	}
	return localization.NewDecoder(data)
}

// An output is never opened until extraction/validation succeeds. O_EXCL also
// rejects symlinks/existing files; a failed writer removes only its new file.
func writeNewLocalizationFile(path string, write func(io.Writer) error) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	complete := false
	defer func() {
		_ = file.Close()
		if !complete {
			_ = os.Remove(path)
		}
	}()
	if err := write(file); err != nil {
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	complete = true
	return nil
}

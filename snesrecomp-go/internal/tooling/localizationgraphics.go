package tooling

import (
	"archive/zip"
	"flag"
	"fmt"
	"io"
	"slices"
)

// RunLocalizationGraphicsCommand exports a local reference archive, never a
// playable language pack. Native assets and their notices remain together.
func RunLocalizationGraphicsCommand(args []string, cwd string, output io.Writer) error {
	flags := flag.NewFlagSet("localization-graphics", flag.ContinueOnError)
	flags.SetOutput(output)
	romPath := flags.String("rom", "", "path to an exact supported regional ROM")
	outPath := flags.String("out", "", "new local-only graphics reference ZIP (never overwritten)")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 || *romPath == "" || *outPath == "" {
		return fmt.Errorf("usage: snesbuild localization-graphics --rom game.sfc --out reference.zip")
	}
	decoder, err := readLocalizationROM(resolveLocalizationPath(cwd, *romPath))
	if err != nil {
		return err
	}
	files, err := decoder.NativeGraphicsFiles()
	if err != nil {
		return err
	}
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	slices.Sort(names)
	destination := resolveLocalizationPath(cwd, *outPath)
	if err := writeNewLocalizationFile(destination, func(w io.Writer) error {
		archive := zip.NewWriter(w)
		for _, name := range names {
			member, err := archive.Create(name)
			if err != nil {
				return err
			}
			if _, err := member.Write(files[name]); err != nil {
				return err
			}
		}
		return archive.Close()
	}); err != nil {
		return err
	}
	fmt.Fprintf(output, "Extracted %s: %d files to %s. ROM-derived reference assets; do not redistribute.\n", decoder.ReleaseID(), len(files), destination)
	return nil
}

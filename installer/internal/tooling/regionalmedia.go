package tooling

import (
	"flag"
	"fmt"
	"io"

	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

func RunRegionalMediaCommand(args []string, cwd string, output io.Writer) error {
	flags := flag.NewFlagSet("regional-media", flag.ContinueOnError)
	flags.SetOutput(output)
	romPath := flags.String("rom", "", "exact supported regional ROM")
	outPath := flags.String("out", "", "new private .armedia file; never overwritten")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 || *romPath == "" || *outPath == "" {
		return fmt.Errorf("usage: actraiser-builder regional-media --rom game.sfc --out jp.armedia")
	}
	rom, err := readGameROMFile(resolveToolPath(cwd, *romPath))
	if err != nil {
		return err
	}
	extraction, err := regionalmedia.Extract(rom)
	if err != nil {
		return err
	}
	data, err := regionalmedia.Pack(extraction)
	if err != nil {
		return err
	}
	destination := resolveToolPath(cwd, *outPath)
	if err := writeNewToolFile(destination, func(w io.Writer) error { _, err := w.Write(data); return err }); err != nil {
		return err
	}
	fmt.Fprintf(output, "Extracted %s: %d reviewed resources to %s. Private ROM-derived assets; do not redistribute.\n", extraction.Release.ID, len(extraction.Resources), destination)
	return nil
}

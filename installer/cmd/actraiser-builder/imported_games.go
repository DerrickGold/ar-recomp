package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2game"
)

// Native modules follow the same compiler and isolated scratch policy as the
// main game. Missing content is optional; corrupt installed content is an error.
func buildImportedGames(values guiFlags, root, dataRoot, scratch, driver, binary string,
	run func(string, io.Writer, ...string) (commandResult, error), output io.Writer) error {
	rom, err := ar2game.ReadROM(dataRoot)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read imported AR2 content: %w", err)
	}
	fmt.Fprintln(output, "Building optional imported game support")
	directory := filepath.Join(scratch, "ar2")
	if err := os.MkdirAll(filepath.Join(directory, "include"), 0700); err != nil {
		return err
	}
	romPath := filepath.Join(directory, "source.sfc")
	if err := os.WriteFile(romPath, rom, 0600); err != nil {
		return err
	}
	generated := filepath.Join(directory, "generated")
	functions := filepath.Join(directory, "include", "funcs.h")
	_, err = run(driver, output, "regen", "--root", root, "--rom", romPath,
		"--toolchain-dir", values.toolchainDir, "--cfg-dir", "recomp/ar2", "--out-dir", generated,
		"--funcs-out", functions, "--metadata-out", filepath.Join(directory, "metadata.json"),
		"--rts-report", filepath.Join(directory, "rts.txt"),
		"--rts-previous", filepath.Join(directory, "rts.previous.txt"),
		"--experimental-proven-analysis", "--experimental-stored-targets", "--experimental-internal-tails", "--allow-stubs",
		"--jobs", fmt.Sprint(values.jobs))
	if err != nil {
		return err
	}
	args := []string{"build", "--root", root, "--rom", romPath, "--hermetic",
		"--toolchain-dir", values.toolchainDir, "--manifest", "recomp/ar2/module.ini",
		"--generated-dir", generated, "--funcs-header", functions,
		"--build-dir", filepath.Join(directory, "objects"), "--jobs", fmt.Sprint(values.jobs),
		"--optimize", values.optimize}
	if values.inputID != "" {
		args = append(args, "--input-id", values.inputID)
	}
	result, err := run(driver, output, args...)
	if err != nil {
		return err
	}
	module, err := oneArtifact(result, "game-binary")
	if err != nil {
		return err
	}
	data, err := os.ReadFile(module)
	if err != nil {
		return err
	}
	// Both paths belong to this attempt's scratch tree. The existing desktop
	// installer owns publishing/rollback after this step succeeds.
	return os.WriteFile(filepath.Join(filepath.Dir(binary), filepath.Base(module)), data, 0755)
}

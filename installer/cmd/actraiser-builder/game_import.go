package main

import (
	"flag"
	"fmt"
	"io"
	"os"

	"github.com/DerrickGold/ar-recomp/installer/internal/gameimports"
)

func runGameImport(args []string) error {
	flags := flag.NewFlagSet("game-import", flag.ContinueOnError)
	root := flags.String("root", ".", "game data root")
	content := flags.String("content", "", "content import ID (run without options to list available imports)")
	romPath := flags.String("rom", "", "source ROM path")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if len(args) == 0 {
		for _, item := range gameimports.List(*root) {
			fmt.Println(item.ID)
		}
		return nil
	}
	if flags.NArg() != 0 || *romPath == "" || *content == "" {
		return fmt.Errorf("game-import requires --content and --rom and accepts only named options")
	}
	file, err := os.Open(*romPath)
	if err != nil {
		return err
	}
	rom, err := io.ReadAll(io.LimitReader(file, gameimports.MaximumROMBytes()+1))
	file.Close()
	if err != nil {
		return err
	}
	if _, err := gameimports.Import(*root, *content, rom); err != nil {
		return err
	}
	fmt.Printf("Game content imported: %s into %s\n", *content, *root)
	return nil
}

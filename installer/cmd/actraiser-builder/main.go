// actraiser-builder owns the ActRaiser Workshop and all game-specific content
// preparation. Generic recompilation is delegated to the standalone snesbuild
// executable through its versioned JSONL contract.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/languagecli"
	"github.com/DerrickGold/ar-recomp/installer/internal/tooling"
)

var version = "dev"

func main() {
	if err := run(os.Args[1:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return
		}
		fmt.Fprintf(os.Stderr, "actraiser-builder: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	bundledMac := false
	if executable, err := os.Executable(); err == nil && filepath.Base(filepath.Dir(executable)) == "MacOS" {
		// Finder invokes CFBundleExecutable without a subcommand. Keep utility
		// commands available for the game's language/archive and font workflows.
		bundledMac = true
		if len(args) == 0 {
			return runAppLaunch(args)
		}
	}
	if len(args) == 0 {
		usage()
		return errors.New("missing command")
	}
	switch args[0] {
	case "package":
		return runPackage(args[1:])
	case "app-launch":
		return runAppLaunch(args[1:])
	case "gui":
		return runGUI(args[1:])
	case "language":
		return languagecli.Run(context.Background(), args[1:], os.Stdout)
	case "localization-extract":
		return tooling.RunLocalizationExtractCommand(args[1:], ".", os.Stdout)
	case "localization-graphics":
		return tooling.RunLocalizationGraphicsCommand(args[1:], ".", os.Stdout)
	case "regional-media":
		return tooling.RunRegionalMediaCommand(args[1:], ".", os.Stdout)
	case "native-source":
		return runNativeSource(args[1:])
	case "quintet-lzss":
		return runQuintetLZSS(args[1:])
	case "audio-preview":
		return runAudioPreview(args[1:])
	case "version", "--version":
		fmt.Printf("actraiser-builder %s (%s/%s)\n", version, runtime.GOOS, runtime.GOARCH)
		return nil
	case "help", "-h", "--help":
		usage()
		return nil
	default:
		if bundledMac {
			return runAppLaunch(args)
		}
		usage()
		return fmt.Errorf("unknown command %q", args[0])
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `Usage: actraiser-builder <command> [options]

Commands:
  gui                    Open the ActRaiser Recomp Builder and Workshop
  package                Create a local .app, AppImage, or AppDir from a built game
  app-launch             Launch an application (--portable, --global, --data-dir)
  language               Validate, author, package, install, or prepare .arlang packs
  localization-extract   Extract a private source pack or localization evidence
  localization-graphics  Extract regional graphical references to a private ZIP
  regional-media         Extract reviewed regional assets to a private .armedia file
  native-source          Ensure the runtime Native US source from a local ROM
  audio-preview          Render local ActRaiser soundtrack preview WAVs
  quintet-lzss           Decode a game content blob for diagnostics
  version                Print the Builder version`)
}

func runNativeSource(args []string) error {
	flags := flag.NewFlagSet("native-source", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	rom := flags.String("rom", "game.sfc", "US ActRaiser ROM, relative to project root")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("native-source accepts only named options")
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return err
	}
	romPath := *rom
	if !filepath.IsAbs(romPath) {
		romPath = filepath.Join(absoluteRoot, romPath)
	}
	return prepareNativeUS(absoluteRoot, romPath, os.Stdout)
}

func runQuintetLZSS(args []string) error {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return errors.New("quintet-lzss needs a linear input offset before its options")
	}
	offsetText, args := args[0], args[1:]
	flags := flag.NewFlagSet("quintet-lzss", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	inputPath := flags.String("input", "game.sfc", "ROM or compressed input path, relative to project root")
	size := flags.Int("size", 0, "exact decompressed size (default: little-endian word at offset)")
	outputPath := flags.String("out", "", "optional output path, relative to project root")
	comparePath := flags.String("compare", "", "optional expected binary, relative to project root")
	compareOffset := flags.Int("compare-offset", 0, "byte offset within --compare")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	offset, err := strconv.ParseInt(strings.TrimSpace(offsetText), 0, 64)
	if err != nil || offset < 0 || int64(int(offset)) != offset {
		return fmt.Errorf("parse input offset %q as a non-negative integer", offsetText)
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	resolve := func(path string) string {
		if path == "" || filepath.IsAbs(path) {
			return path
		}
		return filepath.Join(absoluteRoot, path)
	}
	headered := true
	flags.Visit(func(item *flag.Flag) {
		if item.Name == "size" {
			headered = false
		}
	})
	report, output, err := tooling.BuildQuintetLZSS(tooling.QuintetLZSSOptions{
		InputPath: resolve(*inputPath), Offset: int(offset), Size: *size, Headered: headered,
		ComparePath: resolve(*comparePath), CompareOffset: *compareOffset,
	})
	if err != nil {
		return err
	}
	if strings.TrimSpace(*outputPath) != "" {
		resolved := resolve(*outputPath)
		if err := os.WriteFile(resolved, output, 0o644); err != nil {
			return fmt.Errorf("write decompressed output %s: %w", resolved, err)
		}
		report.NoWrite = false
		fmt.Fprintf(os.Stderr, "quintet-lzss: wrote %d bytes to %s\n", len(output), resolved)
	}
	return tooling.WriteQuintetLZSSReport(os.Stdout, report, *format)
}

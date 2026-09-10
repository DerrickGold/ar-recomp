package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
)

func runPackage(args []string) error {
	flags := flag.NewFlagSet("package", flag.ContinueOnError)
	options := desktop.PackageOptions{Version: version, Output: os.Stdout}
	flags.StringVar(&options.Root, "root", ".", "game data/project root (utils in a downloaded bundle)")
	flags.StringVar(&options.Binary, "binary", "", "built game executable")
	flags.StringVar(&options.ROM, "rom", "", "user-supplied US ActRaiser ROM")
	flags.StringVar(&options.Builder, "builder", "", "Builder/launcher to embed (default: this executable)")
	flags.StringVar(&options.Destination, "destination", "", "output directory")
	flags.StringVar(&options.Format, "format", "", "app, appimage, or appdir (default: native application)")
	flags.StringVar(&options.AppImageTool, "appimagetool", "", "local appimagetool executable")
	flags.StringVar(&options.AppImageRuntime, "appimage-runtime", "", "local type-2 AppImage runtime")
	flags.BoolVar(&options.Replace, "replace", false, "retain the existing application as a backup and replace it")
	portable := flags.Bool("portable", false, "write a sidecar selecting the existing project data")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("package accepts only named options")
	}
	if options.Binary == "" || options.ROM == "" || options.Destination == "" {
		return errors.New("package requires --binary, --rom, and --destination")
	}
	// CLI file arguments are relative to the caller, as with the native tools.
	for _, path := range []*string{&options.Root, &options.Binary, &options.ROM, &options.Destination} {
		absolute, err := filepath.Abs(*path)
		if err != nil {
			return err
		}
		*path = absolute
	}
	if options.Builder == "" {
		var err error
		options.Builder, err = os.Executable()
		if err != nil {
			return err
		}
	}
	if *portable {
		relative, err := filepath.Rel(options.Destination, options.Root)
		if err != nil || !filepath.IsLocal(relative) {
			return errors.New("portable data must be inside the destination directory")
		}
	}
	if err := prepareNativeUS(options.Root, options.ROM, options.Output); err != nil {
		return err
	}
	if err := builder.PrepareRuntimeAssets(options.Root); err != nil {
		return err
	}
	artifact, err := desktop.Package(context.Background(), options)
	if err != nil {
		return err
	}
	if *portable {
		if err := desktop.WritePortableMarker(artifact.Path, options.Root); err != nil {
			return err
		}
	}
	if artifact.Backup != "" {
		fmt.Printf("Previous application retained at %s\n", artifact.Backup)
	}
	return nil
}

func runAppLaunch(args []string) error {
	for _, argument := range args {
		if argument == "--help" || argument == "-h" {
			fmt.Println("Usage: application [--portable | --global | --data-dir PATH] [--print-paths | --prepare-only] [-- game arguments]")
			return nil
		}
		if argument == "--" {
			break
		}
	}
	options, err := desktop.ParseLaunchOptions(args)
	if err == nil {
		err = desktop.Launch(options)
	}
	if err != nil && !options.PrintPaths && !options.PrepareOnly && os.Getenv("AR_HEADLESS") != "1" {
		desktop.ReportLaunchError(err)
	}
	return err
}

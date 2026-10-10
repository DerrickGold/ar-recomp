package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run() error {
	mode := flag.String("mode", "pack", "pack, verify, or extract (inspection only; no execution)")
	payload := flag.String("payload", "", "clean Windows CMake install tree")
	shell := flag.String("shell", "", "unsigned Windows GUI shell")
	arch := flag.String("arch", "", "amd64 or arm64")
	release := flag.String("version", "", "release version for the executable's version information")
	output := flag.String("output", "", "new output executable or extraction directory")
	input := flag.String("input", "", "packaged Windows executable to inspect")
	flag.Parse()
	if *mode == "pack" {
		if *payload == "" || *shell == "" || *output == "" || *arch == "" || *release == "" {
			return fmt.Errorf("pack requires --payload, --shell, --output, --arch and --version")
		}
		stage, err := os.MkdirTemp("", "builder-shell-")
		if err != nil {
			return err
		}
		defer os.RemoveAll(stage)
		described := filepath.Join(stage, "Builder.exe")
		if err = writeShellResources(*shell, described, *release); err != nil {
			return err
		}
		if err = winbundle.Create(winbundle.Options{Shell: described, Payload: *payload, Output: *output, Arch: *arch}); err != nil {
			return err
		}
		fmt.Println(*output)
		return nil
	}
	if *input == "" || (*mode != "verify" && *mode != "extract") {
		return fmt.Errorf("verify/extract requires --input")
	}
	a, err := winbundle.Open(*input)
	if err != nil {
		return err
	}
	defer a.Close()
	if *mode == "extract" {
		if *output == "" {
			return fmt.Errorf("extract requires a new --output directory")
		}
		dest, err := filepath.Abs(*output)
		if err != nil {
			return err
		}
		if err = a.Extract(dest); err != nil {
			return err
		}
		if err = a.VerifyDirectory(dest); err != nil {
			return err
		}
	}
	return json.NewEncoder(os.Stdout).Encode(map[string]any{"schema": a.Manifest.Schema, "arch": a.Manifest.Arch, "archiveSha256": a.ID, "files": len(a.Manifest.Files)})
}

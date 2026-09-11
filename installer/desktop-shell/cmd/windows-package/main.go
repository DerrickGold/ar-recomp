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
	runtimeDir := flag.String("runtime", "", "unpacked Fixed Version directory containing msedgewebview2.exe")
	lock := flag.String("runtime-lock", "windows-webview2.json", "pinned runtime provenance lock")
	arch := flag.String("arch", "", "amd64 or arm64")
	output := flag.String("output", "", "new output executable or extraction directory")
	input := flag.String("input", "", "packaged Windows executable to inspect")
	flag.Parse()
	if *mode == "pack" {
		if *payload == "" || *shell == "" || *runtimeDir == "" || *output == "" {
			return fmt.Errorf("pack requires --payload, --shell, --runtime, --output and --arch")
		}
		data, err := os.ReadFile(*lock)
		if err != nil {
			return err
		}
		var pins struct {
			Schema   int                          `json:"schema"`
			Runtimes map[string]winbundle.Runtime `json:"runtimes"`
		}
		if err = json.Unmarshal(data, &pins); err != nil {
			return err
		}
		if pins.Schema != 1 {
			return fmt.Errorf("unsupported runtime lock schema")
		}
		pin, ok := pins.Runtimes[*arch]
		if !ok {
			return fmt.Errorf("no WebView2 pin for %s", *arch)
		}
		if err = winbundle.Create(winbundle.Options{Shell: *shell, Payload: *payload, WebView: *runtimeDir, Output: *output, Arch: *arch, Runtime: pin}); err != nil {
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
		if err = a.Extract(dest, nil); err != nil {
			return err
		}
		if err = a.VerifyDirectory(dest); err != nil {
			return err
		}
	}
	return json.NewEncoder(os.Stdout).Encode(map[string]any{"schema": a.Manifest.Schema, "arch": a.Manifest.Arch, "archiveSha256": a.ID, "webview": a.Manifest.WebView, "files": len(a.Manifest.Files)})
}

package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
)

func TestIconShellPackagesWithIntactPayload(t *testing.T) {
	if testing.Short() {
		t.Skip("cross-compiles Windows fixtures")
	}
	for _, arch := range []string{"amd64", "arm64"} {
		t.Run(arch, func(t *testing.T) {
			root := t.TempDir()
			source, iconShell, output := filepath.Join(root, "shell.exe"), filepath.Join(root, "icon.exe"), filepath.Join(root, "Builder.exe")
			cmd := exec.Command("go", "build", "-trimpath", "-ldflags=-H windowsgui", "-o", source, "../../../internal/appicons/testdata/main.go")
			cmd.Env = append(os.Environ(), "CGO_ENABLED=0", "GOOS=windows", "GOARCH="+arch)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("cross-build: %v: %s", err, out)
			}
			original, err := os.ReadFile(source)
			if err != nil {
				t.Fatal(err)
			}
			if err := writeIconShell(source, iconShell); err != nil {
				t.Fatal(err)
			}
			after, err := os.ReadFile(source)
			if err != nil || !bytes.Equal(original, after) {
				t.Fatal("input shell changed", err)
			}
			put := func(name string, data []byte) {
				t.Helper()
				path := filepath.Join(root, filepath.FromSlash(name))
				if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(path, data, 0755); err != nil {
					t.Fatal(err)
				}
			}
			for _, name := range []string{"payload/utils/tools/actraiser-builder.exe", "payload/utils/tools/snesbuild.exe", "webview/msedgewebview2.exe"} {
				put(name, original)
			}
			for _, name := range []string{"payload/utils/snesbuild.ini", "payload/utils/defaults/config.ini", "payload/utils/tools/sdl3/include/SDL3/SDL.h", "webview/msedge.dll", "webview/icudtl.dat", "webview/resources.pak", "webview/Locales/en-US.pak"} {
				put(name, []byte("fixture"))
			}
			pin := winbundle.Runtime{Version: "152.0.4191.62", URL: "https://msedge.sf.dl.delivery.mp.microsoft.com/test.cab", SHA256: strings.Repeat("a", 64)}
			if err := winbundle.Create(winbundle.Options{Shell: iconShell, Payload: filepath.Join(root, "payload"), WebView: filepath.Join(root, "webview"), Output: output, Arch: arch, Runtime: pin}); err != nil {
				t.Fatal(err)
			}
			bundle, err := winbundle.Open(output)
			if err != nil {
				t.Fatal(err)
			}
			defer bundle.Close()
			dest := filepath.Join(root, "extracted")
			if err := bundle.Extract(dest, nil); err != nil {
				t.Fatal(err)
			}
			if err := bundle.VerifyDirectory(dest); err != nil {
				t.Fatal(err)
			}
			if err := writeIconShell(output, filepath.Join(root, "repacked.exe")); err == nil {
				t.Fatal("must reject already packaged shell")
			}
		})
	}
}

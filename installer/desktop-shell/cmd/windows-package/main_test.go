package main

import (
	"bytes"
	"debug/pe"
	"encoding/xml"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
	"github.com/tc-hib/winres"
	"github.com/tc-hib/winres/version"
)

func TestShellResourcesPackageWithIntactPayload(t *testing.T) {
	if testing.Short() {
		t.Skip("cross-compiles Windows fixtures")
	}
	for _, fixture := range []struct {
		arch, release string
		numbers       [4]uint16
	}{
		{"amd64", "v0801-3-g0123abc", [4]uint16{0, 8, 0, 1}},
		{"arm64", "v1.0-3-g0123abc-dirty", [4]uint16{1, 0, 0, 0}},
	} {
		t.Run(fixture.arch, func(t *testing.T) {
			root := t.TempDir()
			source, described, output := filepath.Join(root, "shell.exe"), filepath.Join(root, "described.exe"), filepath.Join(root, "Builder.exe")
			cmd := exec.Command("go", "build", "-trimpath", "-ldflags=-H windowsgui", "-o", source, "../../../internal/appicons/testdata/main.go")
			cmd.Env = append(os.Environ(), "CGO_ENABLED=0", "GOOS=windows", "GOARCH="+fixture.arch)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("cross-build: %v: %s", err, out)
			}
			original, err := os.ReadFile(source)
			if err != nil {
				t.Fatal(err)
			}
			if err := writeShellResources(source, described, ""); err == nil {
				t.Fatal("accepted an unversioned shell")
			}
			if err := writeShellResources(source, described, fixture.release); err != nil {
				t.Fatal(err)
			}
			after, err := os.ReadFile(source)
			if err != nil || !bytes.Equal(original, after) {
				t.Fatal("input shell changed", err)
			}
			checkBuilderResources(t, described, fixture.release, fixture.numbers, false)
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
			put("payload/utils/tools/snesbuild.exe", original)
			helper := filepath.Join(root, "payload", "utils", "tools", "actraiser-builder.exe")
			rawHelper := filepath.Join(root, "console.exe")
			console := exec.Command("go", "build", "-trimpath", "-ldflags=-s -w", "-o", rawHelper, "../../../internal/appicons/testdata/main.go")
			console.Env = cmd.Env
			if out, err := console.CombinedOutput(); err != nil {
				t.Fatalf("cross-build console helper: %v: %s", err, out)
			}
			if err := writeBuilderResources(rawHelper, helper, fixture.release, pe.IMAGE_SUBSYSTEM_WINDOWS_CUI); err != nil {
				t.Fatal(err)
			}
			checkBuilderResources(t, helper, fixture.release, fixture.numbers, true)
			for _, name := range []string{"payload/utils/snesbuild.ini", "payload/utils/defaults/config.ini", "payload/utils/tools/sdl3/include/SDL3/SDL.h"} {
				put(name, []byte("fixture"))
			}
			if err := winbundle.Create(winbundle.Options{Shell: described, Payload: filepath.Join(root, "payload"), Output: output, Arch: fixture.arch}); err != nil {
				t.Fatal(err)
			}
			// The appended archive must not hide the shell's resources from Windows.
			checkBuilderResources(t, output, fixture.release, fixture.numbers, false)
			bundle, err := winbundle.Open(output)
			if err != nil {
				t.Fatal(err)
			}
			defer bundle.Close()
			dest := filepath.Join(root, "extracted")
			if err := bundle.Extract(dest); err != nil {
				t.Fatal(err)
			}
			if err := bundle.VerifyDirectory(dest); err != nil {
				t.Fatal(err)
			}
			checkBuilderResources(t, filepath.Join(dest, "payload", "utils", "tools", "actraiser-builder.exe"), fixture.release, fixture.numbers, true)
			if err := writeShellResources(output, filepath.Join(root, "repacked.exe"), "v0801"); err == nil {
				t.Fatal("must reject already packaged shell")
			}
		})
	}
}

func checkBuilderResources(t *testing.T, path string, release string, numbers [4]uint16, console bool) {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	resources, err := winres.LoadFromEXE(f)
	if err != nil {
		t.Fatal(err)
	}
	if resources.Get(winres.RT_GROUP_ICON, winres.ID(3), winres.LCIDNeutral) == nil {
		t.Fatal("lost the Builder icon")
	}
	vi, err := version.FromBytes(resources.Get(winres.RT_VERSION, winres.ID(1), version.LangDefault))
	if err != nil {
		t.Fatal("missing version information:", err)
	}
	if vi.FileVersion != numbers || vi.ProductVersion != numbers {
		t.Fatalf("numeric versions %v/%v, want %v", vi.FileVersion, vi.ProductVersion, numbers)
	}
	values := vi.Table().GetMainTranslation()
	name, description := "ActRaiserRecompBuilder", "ActRaiser Recomp Builder"
	subsystem := uint16(pe.IMAGE_SUBSYSTEM_WINDOWS_GUI)
	if console {
		name, description = "actraiser-builder", "ActRaiser Recomp Builder CLI"
		subsystem = pe.IMAGE_SUBSYSTEM_WINDOWS_CUI
	}
	image, err := pe.NewFile(f)
	if err != nil {
		t.Fatal(err)
	}
	defer image.Close()
	if got := image.OptionalHeader.(*pe.OptionalHeader64).Subsystem; got != subsystem {
		t.Errorf("PE subsystem %d, want %d", got, subsystem)
	}
	for key, want := range map[string]string{
		version.FileDescription:  description,
		version.ProductName:      "ActRaiser Recomp Builder",
		version.InternalName:     name,
		version.OriginalFilename: name + ".exe",
		version.FileVersion:      release,
		version.ProductVersion:   release,
	} {
		if values[key] != want {
			t.Errorf("%s = %q, want %q", key, values[key], want)
		}
	}
	for _, key := range []string{version.CompanyName, version.LegalCopyright} {
		if values[key] == "" {
			t.Errorf("missing %s", key)
		}
	}
	manifest := resources.Get(winres.RT_MANIFEST, winres.ID(1), winres.LCIDDefault)
	if console && manifest != nil {
		t.Fatal("console helper acquired a GUI manifest")
	}
	if !console && !bytes.Equal(manifest, []byte(shellManifest)) {
		t.Fatalf("manifest not embedded verbatim:\n%s", manifest)
	}
}

// A malformed manifest stops Windows from starting the executable at all, and
// a DPI declaration could make Wails' own SetProcessDPIAware call fail.
func TestShellManifestIsMinimalAndWellFormed(t *testing.T) {
	var parsed struct {
		XMLName       xml.Name `xml:"urn:schemas-microsoft-com:asm.v1 assembly"`
		Version       string   `xml:"manifestVersion,attr"`
		Compatibility struct {
			OS []struct {
				ID string `xml:"Id,attr"`
			} `xml:"application>supportedOS"`
		} `xml:"urn:schemas-microsoft-com:compatibility.v1 compatibility"`
		Level struct {
			Level    string `xml:"level,attr"`
			UIAccess string `xml:"uiAccess,attr"`
		} `xml:"urn:schemas-microsoft-com:asm.v3 trustInfo>security>requestedPrivileges>requestedExecutionLevel"`
		Application *struct{} `xml:"application"`
		Dependency  *struct{} `xml:"dependency"`
		Identity    *struct{} `xml:"assemblyIdentity"`
	}
	if err := xml.Unmarshal([]byte(shellManifest), &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.Version != "1.0" || parsed.Level.Level != "asInvoker" || parsed.Level.UIAccess != "false" {
		t.Fatalf("unexpected execution request: %+v", parsed)
	}
	if len(parsed.Compatibility.OS) != 1 || parsed.Compatibility.OS[0].ID != "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}" {
		t.Fatalf("expected only the Windows 10/11 compatibility GUID: %+v", parsed.Compatibility)
	}
	if parsed.Application != nil || parsed.Dependency != nil || parsed.Identity != nil {
		t.Fatal("manifest must not declare DPI/window settings, dependencies or an assembly identity")
	}
}

func TestReleaseNumbers(t *testing.T) {
	for release, want := range map[string][4]uint16{
		"v049":                     {0, 4, 9, 0},
		"v0491":                    {0, 4, 9, 1},
		"v0800":                    {0, 8, 0, 0},
		"v0800-9-g03b54133":        {0, 8, 0, 0},
		"v0801-dirty":              {0, 8, 0, 1},
		"v0900":                    {0, 9, 0, 0},
		"v1000":                    {1, 0, 0, 0},
		"v1.0":                     {1, 0, 0, 0},
		"v1.0.1":                   {1, 0, 1, 0},
		"v12.34.56.78":             {12, 34, 56, 78},
		"v1.0-9-g03b54133-dirty":   {1, 0, 0, 0},
		"v1.2.3-rc.1+build.4":      {1, 2, 3, 0},
		"v1.2+build.4":             {1, 2, 0, 0},
		"v65535.65535.65535.65535": {65535, 65535, 65535, 65535},
		"v65536.0":                 {},
		"v1.65536":                 {},
		"v1..2":                    {},
		"v1.2.":                    {},
		"v.1":                      {},
		"v1.2.3.4.5":               {},
		"v1.0invalid":              {},
		"03b54133":                 {},
		"0.0.0-dev":                {},
		"v1":                       {},
		"v12345":                   {},
		"version":                  {},
	} {
		if got := releaseNumbers(release); got != want {
			t.Errorf("releaseNumbers(%q) = %v, want %v", release, got, want)
		}
		info, err := builderVersionInfo(release, "ActRaiser Recomp Builder CLI", "actraiser-builder")
		if err != nil {
			t.Fatal(err)
		}
		values := info.Table().GetMainTranslation()
		if info.FileVersion != want || info.ProductVersion != want ||
			values[version.FileVersion] != release || values[version.ProductVersion] != release {
			t.Errorf("version information lost the release stamp %q", release)
		}
	}
}

package packaging_test

import (
	"archive/zip"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Exercise the real post-package gate on tiny ROM-free archives. This cannot
// replace release-file inspection, but keeps the new dependency/leak checks
// effective even when ordinary CI does not package an entire compiler SDK.
func TestLocalizationPackageGate(t *testing.T) {
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("CMake required for the distribution gate")
	}
	template, err := os.ReadFile("check_package.cmake.in")
	if err != nil {
		t.Fatal(err)
	}
	files := []string{
		"utils/game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf",
		"utils/game-assets/fonts/noto/NotoSansJP-Bold.otf", "utils/game-assets/fonts/noto/NotoSansJP-OFL.txt",
		"utils/LICENSE", "utils/LICENSE_SCOPE.md", "utils/ATTRIBUTION.md", "utils/THIRD_PARTY_NOTICES.md",
		"utils/game-assets/fonts/noto/OFL.txt", "utils/snesrecomp-go/runtime/LICENSE", "utils/snesrecomp-go/runtime/NOTICE.md", "utils/snesrecomp-go/runtime/PROVENANCE.md", "utils/snesrecomp-go/runtime/licenses/Snaggletooth-LICENSE.txt",
		"utils/runtime.a", "utils/tools/sdl3/lib/SDL3.dll", "utils/tools/sdl3/lib/SDL3_ttf.dll", "utils/tools/sdl3/lib/SDL3_ttf.lib", "utils/tools/sdl3/include/SDL3_ttf/SDL_ttf.h", "utils/tools/sdl3/licenses/SDL3_ttf/LICENSE.txt",
		"utils/licenses/SDL3_ttf/LICENSE.txt", "utils/licenses/SDL3_ttf/FreeType-FTL.txt", "utils/licenses/SDL3_ttf/HarfBuzz-COPYING.txt", "utils/licenses/SDL3_ttf/PlutoSVG-LICENSE.txt", "utils/licenses/SDL3_ttf/PlutoVG-LICENSE.txt",
	}
	configured := strings.NewReplacer(
		"@SNESBUILD_LEAK_PATTERNS@", "/invented/build/machine",
		"@SNESBUILD_ALLOWED_TOPLEVEL@", "utils",
		"@SNESBUILD_SCAN_EXEMPT_DIRS@", "utils/tools",
		"@SNESBUILD_RUNTIME_ARCHIVE_RELATIVE@", "utils/runtime.a",
		"@SNESBUILD_RUNTIME_SOURCE_PATHS@", "utils/snesrecomp-go/runtime/src",
		"@SNESBUILD_REQUIRED_SDL_RUNTIME@", "utils/tools/sdl3/lib/SDL3.dll",
		"@SNESBUILD_REQUIRED_TTF_RUNTIME@", "utils/tools/sdl3/lib/SDL3_ttf.dll",
		"@SNESBUILD_REQUIRED_TTF_LINK@", "utils/tools/sdl3/lib/SDL3_ttf.lib",
	).Replace(string(template))
	for _, tc := range []struct{ name, omit, add string }{
		{name: "valid"},
		{name: "missing Japanese UI font", omit: "utils/game-assets/fonts/noto/NotoSansJP-Bold.otf"},
		{name: "missing Japanese UI notice", omit: "utils/game-assets/fonts/noto/NotoSansJP-OFL.txt"},
		{name: "missing runtime", omit: "utils/tools/sdl3/lib/SDL3_ttf.dll"},
		{name: "missing link", omit: "utils/tools/sdl3/lib/SDL3_ttf.lib"},
		{name: "missing header", omit: "utils/tools/sdl3/include/SDL3_ttf/SDL_ttf.h"},
		{name: "missing persistent notice", omit: "utils/licenses/SDL3_ttf/HarfBuzz-COPYING.txt"},
		{name: "uppercase ROM", add: "utils/local.SFC"},
		{name: "source metadata", add: "utils/game-assets/languages/sources/metadata.json"},
		{name: "extracted scenery", add: "utils/game-assets/workshop/us-scene-v1.json"},
		{name: "private backup", add: "utils/work.arproject"},
		{name: "private notes", add: "utils/development/tasks.md"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			archive := filepath.Join(dir, "fixture.zip")
			f, err := os.Create(archive)
			if err != nil {
				t.Fatal(err)
			}
			w := zip.NewWriter(f)
			for _, name := range append(append([]string(nil), files...), tc.add) {
				if name == "" || name == tc.omit {
					continue
				}
				entry, err := w.Create("fixture/" + name)
				if err != nil {
					t.Fatal(err)
				}
				if _, err := entry.Write([]byte("invented fixture")); err != nil {
					t.Fatal(err)
				}
			}
			if err := w.Close(); err != nil {
				t.Fatal(err)
			}
			if err := f.Close(); err != nil {
				t.Fatal(err)
			}
			script := filepath.Join(dir, "gate.cmake")
			if err := os.WriteFile(script, []byte("cmake_minimum_required(VERSION 3.25)\n"+configured), 0644); err != nil {
				t.Fatal(err)
			}
			output, err := exec.Command(cmake, "-DCPACK_PACKAGE_FILES="+archive, "-P", script).CombinedOutput()
			if tc.name == "valid" {
				if err != nil {
					t.Fatalf("valid rejected: %s", output)
				}
			} else if err == nil {
				t.Fatal("invalid package accepted")
			} else if !strings.Contains(string(output), "SDL3_ttf package check") && !strings.Contains(string(output), "localization distribution check") && !strings.Contains(string(output), "interface font check") && !strings.Contains(string(output), "license check") {
				t.Fatalf("wrong rejection: %s", output)
			}
		})
	}
}

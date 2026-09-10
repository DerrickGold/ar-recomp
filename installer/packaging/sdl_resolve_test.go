package packaging_test

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Exercise the actual CMake integration with offline lock input, without
// downloading or extracting any SDK. This catches stale-cache regressions.
func TestSDLResolutionInvalidatesWholeSDK(t *testing.T) {
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("CMake required")
	}
	goexe, err := exec.LookPath("go")
	if err != nil {
		t.Skip("Go required")
	}
	root, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	tmp := filepath.ToSlash(t.TempDir())
	root = filepath.ToSlash(root)
	goexe = filepath.ToSlash(goexe)
	lockFile := tmp + "/input.json"
	writeLock := func(version string) {
		t.Helper()
		component := func(repo, name, version string) map[string]any {
			archive := name + "-devel-" + version + "-mingw.tar.gz"
			return map[string]any{"version": version, "kind": "mingw", "archives": []map[string]string{{
				"archive": archive, "sha256": strings.Repeat("a", 64),
				"url": "https://github.com/libsdl-org/" + repo + "/releases/download/release-" + version + "/" + archive,
			}}}
		}
		data, err := json.Marshal(map[string]any{"schema": 1, "goos": "windows", "goarch": "amd64",
			"sdl": component("SDL", "SDL3", version), "ttf": component("SDL_ttf", "SDL3_ttf", "3.2.2")})
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(lockFile, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	writeLock("3.4.12")
	preamble := fmt.Sprintf(`cmake_minimum_required(VERSION 3.25)
set(CMAKE_BINARY_DIR "%s")
set(SNESBUILD_GOOS windows)
set(SNESBUILD_GOARCH amd64)
set(SNESBUILD_SDL3_VERSION 3)
set(SNESBUILD_SDL3_TTF_VERSION 3)
set(SNESBUILD_SDL_LOCKFILE "%s")
set(GO_EXECUTABLE "%s")
set(SNESBUILD_MODULE_DIR "%s/snesrecomp-go")
set(_go_cache "%s/go-cache")
include("%s/installer/packaging/sdl-resolve.cmake")
`, tmp, lockFile, goexe, root, tmp, root)
	run := func(body string) {
		t.Helper()
		script := tmp + "/check.cmake"
		if err := os.WriteFile(script, []byte(preamble+body), 0600); err != nil {
			t.Fatal(err)
		}
		cmd := exec.Command(cmake, "-P", script)
		// The lock path must not depend on network availability.
		cmd.Env = append(os.Environ(), "HTTPS_PROXY=http://127.0.0.1:1", "HTTP_PROXY=http://127.0.0.1:1")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%v\n%s", err, out)
		}
	}
	run(`file(MAKE_DIRECTORY "${_sdl_stage}/include/SDL3" "${_sdl_stage}/include/SDL3_ttf")
file(WRITE "${_sdl_stage}/include/SDL3/stale.h" "old SDL")
file(WRITE "${_sdl_stage}/include/SDL3_ttf/stale.h" "old TTF")
file(WRITE "${_sdl_stage}/sdk.pin" "${_sdl_expected}")
`)
	run(`if(NOT EXISTS "${_sdl_stage}/include/SDL3/stale.h")
  message(FATAL_ERROR "unchanged lock discarded SDK cache")
endif()
`)
	writeLock("3.4.14")
	run(`if(EXISTS "${_sdl_stage}")
  message(FATAL_ERROR "new lock retained stale SDK files")
endif()
if(NOT _sdl_version STREQUAL "3.4.14")
  message(FATAL_ERROR "wrong resolved version")
endif()
`)
	data, err := os.ReadFile(tmp + "/sdl-sdk.lock.json")
	if err != nil || !strings.Contains(string(data), "3.4.14") {
		t.Fatalf("lock output: %s %v", data, err)
	}
}

package toolchain

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

const testSDLSHA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

func TestStableSDLSelectors(t *testing.T) {
	for _, v := range []string{"3", "3.x", "3.X", "3.4.0", "3.10.12"} {
		if err := validateSDLSelector(v, "SDL3"); err != nil {
			t.Errorf("%s: %v", v, err)
		}
	}
	for _, v := range []string{"", "latest", "4", "4.0.0", "3.2.10", "3.5.0", "3.4.1", "3.4.0-rc1", "3.04.0", "3.4"} {
		if err := validateSDLSelector(v, "SDL3"); err == nil {
			t.Errorf("accepted %s", v)
		}
	}
	if !sdlVersionMatches("3.2.2", "3", "SDL3_ttf") || sdlVersionMatches("3.2.0", "3", "SDL3_ttf") {
		t.Fatal("TTF minimum")
	}
	if !newerSDL("3.10.0", "3.8.12") || !newerSDL("3.4.12", "3.4.8") {
		t.Fatal("versions must sort numerically")
	}
}

func releaseFixture(component, os, arch, version string) githubSDLRelease {
	name, _ := officialSDLAsset(component, os, arch, version)
	repo := "SDL"
	if component == "SDL3_ttf" {
		repo = "SDL_ttf"
	}
	r := githubSDLRelease{Tag: "release-" + version}
	// JSON keeps the production asset type private to the resolver.
	data := fmt.Sprintf(`[{"name":%q,"browser_download_url":%q,"digest":%q}]`, name,
		"https://github.com/libsdl-org/"+repo+"/releases/download/"+r.Tag+"/"+name, "sha256:"+testSDLSHA)
	if err := json.Unmarshal([]byte(data), &r.Assets); err != nil {
		panic(err)
	}
	return r
}

func TestOfficialSDLNewestStableAndPagination(t *testing.T) {
	first := make([]githubSDLRelease, 100)
	first[0] = releaseFixture("SDL3", "windows", "amd64", "3.4.12")
	first[1] = releaseFixture("SDL3", "windows", "amd64", "3.11.0")
	first[2] = releaseFixture("SDL3", "windows", "amd64", "4.0.0")
	first[3] = releaseFixture("SDL3", "windows", "amd64", "3.12.0")
	first[3].Prerelease = true
	first[4] = releaseFixture("SDL3", "windows", "amd64", "3.14.0")
	first[4].Draft = true
	first[5] = releaseFixture("SDL3", "windows", "amd64", "3.10.1")
	calls := 0
	r, err := resolveOfficialSDL("SDL3", "windows", "amd64", "3", func(url string) ([]byte, error) {
		calls++
		if strings.HasSuffix(url, "page=1") {
			return json.Marshal(first)
		}
		if strings.HasSuffix(url, "page=2") {
			return json.Marshal([]githubSDLRelease{releaseFixture("SDL3", "windows", "amd64", "3.10.0")})
		}
		t.Fatalf("unexpected URL %s", url)
		return nil, nil
	})
	if err != nil || r.Version != "3.10.0" || calls != 2 {
		t.Fatalf("%+v %v calls=%d", r, err, calls)
	}
}

func TestOfficialSDLExactAssetsAndChecksums(t *testing.T) {
	for _, os := range []string{"darwin", "windows"} {
		for _, arch := range []string{"amd64", "arm64"} {
			t.Run(os+"/"+arch, func(t *testing.T) {
				r := releaseFixture("SDL3", os, arch, "3.4.14")
				fetch := func(url string) ([]byte, error) {
					if !strings.HasSuffix(url, "/releases/tags/release-3.4.14") {
						t.Fatalf("not exact lookup: %s", url)
					}
					return json.Marshal(r)
				}
				if _, err := resolveOfficialSDL("SDL3", os, arch, "3.4.14", fetch); err != nil {
					t.Fatal(err)
				}
				r.Assets[0].Digest = ""
				if _, err := resolveOfficialSDL("SDL3", os, arch, "3.4.14", fetch); err == nil {
					t.Fatal("accepted unknown checksum")
				}
				r.Assets[0].Digest = "sha256:" + testSDLSHA
				r.Assets[0].URL = "https://example.org/untrusted"
				if _, err := resolveOfficialSDL("SDL3", os, arch, "3.4.14", fetch); err == nil {
					t.Fatal("accepted foreign URL")
				}
				legacy := releaseFixture("SDL3_ttf", os, arch, "3.2.2")
				legacy.Assets[0].Digest = ""
				got, err := resolveOfficialSDL("SDL3_ttf", os, arch, "3", func(string) ([]byte, error) { return json.Marshal([]githubSDLRelease{legacy}) })
				_, sha, _, _, _ := SDL3TtfPin(os, arch)
				if err != nil || got.Archives[0].SHA256 != sha {
					t.Fatalf("legacy checksum: %+v %v", got, err)
				}
			})
		}
	}
}

func gzipSDLFixture(t *testing.T, data string) []byte {
	t.Helper()
	var b bytes.Buffer
	z := gzip.NewWriter(&b)
	if _, err := z.Write([]byte(data)); err != nil {
		t.Fatal(err)
	}
	if err := z.Close(); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func linuxSDLFixture(name, version, arch string) string {
	source := "libsdl3"
	if strings.Contains(name, "ttf") {
		source = "libsdl3-ttf"
	}
	return fmt.Sprintf("Package: %s\nArchitecture: %s\nVersion: %s\nFilename: pool/main/libs/%s/%s_%s_%s.deb\nSHA256: %s\nDepends: libc6 (>= 2.38),\n libfreetype6\n\n", name, arch, version, source, name, version, arch, testSDLSHA)
}

func TestLinuxSDLMatchingPairsAndSharedMetadata(t *testing.T) {
	var data string
	for _, version := range []string{"3.4.14+ds-1", "3.10.0+ds-1", "3.10.0+ds-10", "3.11.0+ds-1", "4.0.0-1"} {
		for _, name := range []string{"libsdl3-dev", "libsdl3-0"} {
			data += linuxSDLFixture(name, version, "amd64")
		}
	}
	// Newer incomplete or wrong-architecture pairs must not mix with old files.
	data += linuxSDLFixture("libsdl3-dev", "3.12.0-1", "amd64")
	data += linuxSDLFixture("libsdl3-0", "3.12.0-2", "amd64")
	for _, name := range []string{"libsdl3-dev", "libsdl3-0"} {
		data += linuxSDLFixture(name, "3.14.0-1", "arm64")
	}
	for _, name := range []string{"libsdl3-ttf-dev", "libsdl3-ttf0"} {
		data += linuxSDLFixture(name, "3.2.2+ds-1", "amd64")
	}
	compressed := gzipSDLFixture(t, data)
	calls := 0
	lock, err := resolveSDLSDK("linux", "amd64", "3", "3", func(url string) ([]byte, error) {
		calls++
		if !strings.Contains(url, "steamrt-sniper/dists/sniper/main/binary-amd64") {
			t.Fatalf("wrong index: %s", url)
		}
		return compressed, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if calls != 1 || lock.SDL.Version != "3.10.0" || !strings.Contains(lock.SDL.Archives[0].Archive, "+ds-10_") {
		t.Fatalf("%+v calls=%d", lock, calls)
	}
	if lock.SDL.Dependencies != "libc6 (>= 2.38), libfreetype6" {
		t.Fatal(lock.SDL.Dependencies)
	}
	if _, err := resolveSDLSDK("linux", "amd64", "3.12.0", "3", func(string) ([]byte, error) { return compressed, nil }); err == nil {
		t.Fatal("accepted mismatched pair")
	}
	// A lock is reusable only for its original platform, selectors and inputs.
	for _, change := range []func(*SDLSDKLock){
		func(l *SDLSDKLock) { l.Schema = 2 },
		func(l *SDLSDKLock) { l.GOARCH = "arm64" },
		func(l *SDLSDKLock) { l.SDL.Version = "4.0.0" },
		func(l *SDLSDKLock) { l.SDL.Version = "3.12.0" },
		func(l *SDLSDKLock) { l.SDL.Archives = l.SDL.Archives[:1] },
		func(l *SDLSDKLock) { l.SDL.Archives[0].SHA256 = "bad" },
		func(l *SDLSDKLock) { l.SDL.Archives[0].URL = "https://example.org/" + l.SDL.Archives[0].Archive },
		func(l *SDLSDKLock) { l.SDL.Archives[0] = l.SDL.Archives[1] },
		func(l *SDLSDKLock) { l.SDL.Archives[0] = l.TTF.Archives[0] },
	} {
		blob, _ := json.Marshal(lock)
		var copy SDLSDKLock
		json.Unmarshal(blob, &copy)
		change(&copy)
		if err := ValidateSDLSDKLock(copy, "linux", "amd64", "3", "3"); err == nil {
			t.Fatalf("invalid lock accepted: %+v", copy)
		}
	}
}

func TestSDLMetadataFailures(t *testing.T) {
	for _, platform := range [][2]string{{"linux", "386"}, {"freebsd", "amd64"}} {
		lock, err := resolveSDLSDK(platform[0], platform[1], "3", "3", func(string) ([]byte, error) { t.Fatal("unexpected fetch"); return nil, nil })
		if err == nil {
			t.Fatal("unsupported platform resolved")
		}
		if err := ValidateSDLSDKLock(lock, platform[0], platform[1], "3", "3"); err == nil {
			t.Fatal("unsupported lock")
		}
	}
	for _, data := range [][]byte{[]byte("not gzip"), gzipSDLFixture(t, ""), gzipSDLFixture(t, strings.ReplaceAll(linuxSDLFixture("libsdl3-dev", "3.4.14-1", "amd64")+linuxSDLFixture("libsdl3-0", "3.4.14-1", "amd64"), testSDLSHA, "bad"))} {
		if _, err := resolveLinuxSDL("SDL3", "amd64", "3", func(string) ([]byte, error) { return data, nil }); err == nil {
			t.Fatal("invalid metadata accepted")
		}
	}
}

func TestDebianSDLRevisionOrder(t *testing.T) {
	for _, pair := range [][2]string{{"3.4.14-1", "3.4.14-10"}, {"3.4.14-1~steamrt1", "3.4.14-1"}, {"3.4.14-1", "3.4.14-1+bsrt1"}, {"3.4.14-99", "1:3.4.14-1"}} {
		if debianRevisionCompare(pair[0], pair[1]) >= 0 || debianRevisionCompare(pair[1], pair[0]) <= 0 {
			t.Fatal(pair)
		}
	}
	if debianRevisionCompare("3.4.14-01", "0:3.4.14-1") != 0 {
		t.Fatal("leading zeros / default epoch")
	}
}

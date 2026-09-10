package toolchain

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"path"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// Resolution happens on the release packager, never on a player's machine.
// A lock records the exact verified inputs selected from the stable 3.x line.
type SDLSDKLock struct {
	Schema int         `json:"schema"`
	GOOS   string      `json:"goos"`
	GOARCH string      `json:"goarch"`
	SDL    SDLResolved `json:"sdl"`
	TTF    SDLResolved `json:"ttf"`
}

type SDLResolved struct {
	Version      string       `json:"version"`
	Kind         string       `json:"kind"`
	Archives     []SDLArchive `json:"archives"`
	Dependencies string       `json:"dependencies,omitempty"`
}

type SDLArchive struct {
	URL     string `json:"url"`
	SHA256  string `json:"sha256"`
	Archive string `json:"archive"`
}

var stableSDLPattern = regexp.MustCompile(`^3\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$`)

func stableSDLVersion(value string) (minor, patch int, okay bool) {
	parts := stableSDLPattern.FindStringSubmatch(value)
	if parts == nil {
		return 0, 0, false
	}
	minor, e1 := strconv.Atoi(parts[1])
	patch, e2 := strconv.Atoi(parts[2])
	return minor, patch, e1 == nil && e2 == nil && minor%2 == 0 && patch%2 == 0
}

func sdlVersionMatches(version, selector, component string) bool {
	m, p, okay := stableSDLVersion(version)
	if !okay || (component == "SDL3" && m < 4) || (component == "SDL3_ttf" && (m < 2 || m == 2 && p < 2)) {
		return false
	}
	return selector == "3" || selector == "3.x" || selector == "3.X" || selector == version
}

func newerSDL(a, b string) bool {
	am, ap, _ := stableSDLVersion(a)
	bm, bp, _ := stableSDLVersion(b)
	return b == "" || am > bm || am == bm && ap > bp
}

func validateSDLSelector(value, component string) error {
	if value == "3" || value == "3.x" || value == "3.X" || sdlVersionMatches(value, value, component) {
		return nil
	}
	return fmt.Errorf("%s version must be 3 (latest stable 3.x) or an exact supported stable 3.x.y release", component)
}

// ResolveSDLSDK chooses the newest stable major-3 SDK available from each
// platform's publisher. Linux dev/runtime packages must have identical full
// package versions, not merely the same SONAME or upstream major version.
func ResolveSDLSDK(goos, goarch, sdlVersion, ttfVersion string) (SDLSDKLock, error) {
	client := &http.Client{Timeout: 45 * time.Second}
	return resolveSDLSDK(goos, goarch, sdlVersion, ttfVersion, func(url string) ([]byte, error) {
		response, err := client.Get(url)
		if err != nil {
			return nil, err
		}
		defer response.Body.Close()
		if response.StatusCode != http.StatusOK {
			return nil, fmt.Errorf("SDK metadata %s: HTTP %d", url, response.StatusCode)
		}
		data, err := io.ReadAll(io.LimitReader(response.Body, (32<<20)+1))
		if len(data) > 32<<20 {
			return nil, fmt.Errorf("SDK metadata exceeds 32 MiB")
		}
		return data, err
	})
}

type sdlMetadataFetch func(string) ([]byte, error)

func resolveSDLSDK(goos, goarch, sdlVersion, ttfVersion string, fetch sdlMetadataFetch) (SDLSDKLock, error) {
	lock := SDLSDKLock{Schema: 1, GOOS: goos, GOARCH: goarch}
	if goarch != "amd64" && goarch != "arm64" || goos != "darwin" && goos != "windows" && goos != "linux" {
		return lock, fmt.Errorf("unsupported SDK platform %s/%s", goos, goarch)
	}
	if err := validateSDLSelector(sdlVersion, "SDL3"); err != nil {
		return lock, err
	}
	if err := validateSDLSelector(ttfVersion, "SDL3_ttf"); err != nil {
		return lock, err
	}
	// Both x86_64 SDL packages come from the same index. Fetch it only once.
	cache := map[string][]byte{}
	get := func(url string) ([]byte, error) {
		if data, okay := cache[url]; okay {
			return data, nil
		}
		data, err := fetch(url)
		if err == nil {
			cache[url] = data
		}
		return data, err
	}
	for _, item := range []struct {
		component, selector string
		result              *SDLResolved
	}{{"SDL3", sdlVersion, &lock.SDL}, {"SDL3_ttf", ttfVersion, &lock.TTF}} {
		var err error
		if goos == "linux" {
			*item.result, err = resolveLinuxSDL(item.component, goarch, item.selector, get)
		} else {
			*item.result, err = resolveOfficialSDL(item.component, goos, goarch, item.selector, get)
		}
		if err != nil {
			return lock, fmt.Errorf("resolve %s for %s/%s: %w", item.component, goos, goarch, err)
		}
	}
	return lock, ValidateSDLSDKLock(lock, goos, goarch, sdlVersion, ttfVersion)
}

type githubSDLRelease struct {
	Tag        string `json:"tag_name"`
	Draft      bool   `json:"draft"`
	Prerelease bool   `json:"prerelease"`
	Assets     []struct {
		Name   string `json:"name"`
		URL    string `json:"browser_download_url"`
		Digest string `json:"digest"`
	} `json:"assets"`
}

func officialSDLAsset(component, goos, goarch, version string) (name, kind string) {
	if goos == "darwin" {
		return component + "-" + version + ".dmg", "dmg"
	}
	if goarch == "arm64" {
		return component + "-devel-" + version + "-VC.zip", "vc"
	}
	return component + "-devel-" + version + "-mingw.tar.gz", "mingw"
}

func resolveOfficialSDL(component, goos, goarch, selector string, fetch sdlMetadataFetch) (SDLResolved, error) {
	repo := "SDL"
	if component == "SDL3_ttf" {
		repo = "SDL_ttf"
	}
	var selected githubSDLRelease
	version := ""
	for page := 1; page <= 10; page++ {
		url := fmt.Sprintf("https://api.github.com/repos/libsdl-org/%s/releases?per_page=100&page=%d", repo, page)
		exact := selector != "3" && selector != "3.x" && selector != "3.X"
		if exact {
			url = "https://api.github.com/repos/libsdl-org/" + repo + "/releases/tags/release-" + selector
		}
		data, err := fetch(url)
		if err != nil {
			return SDLResolved{}, err
		}
		var releases []githubSDLRelease
		if exact {
			var release githubSDLRelease
			if err := json.Unmarshal(data, &release); err != nil {
				return SDLResolved{}, err
			}
			releases = []githubSDLRelease{release}
		} else {
			if err := json.Unmarshal(data, &releases); err != nil {
				return SDLResolved{}, err
			}
		}
		for _, release := range releases {
			v := strings.TrimPrefix(release.Tag, "release-")
			if !strings.HasPrefix(release.Tag, "release-") || release.Draft || release.Prerelease || !sdlVersionMatches(v, selector, component) {
				continue
			}
			if newerSDL(v, version) {
				selected, version = release, v
			}
		}
		if len(releases) < 100 {
			break
		}
		if page == 10 {
			return SDLResolved{}, fmt.Errorf("too many release pages; select an exact SDK release")
		}
	}
	if version == "" {
		return SDLResolved{}, fmt.Errorf("no supported stable %s release matches %q", component, selector)
	}
	name, kind := officialSDLAsset(component, goos, goarch, version)
	url := "https://github.com/libsdl-org/" + repo + "/releases/download/release-" + version + "/" + name
	for _, asset := range selected.Assets {
		if asset.Name != name {
			continue
		}
		if asset.URL != url {
			return SDLResolved{}, fmt.Errorf("unexpected publisher URL for %s", name)
		}
		sha := ""
		if strings.HasPrefix(asset.Digest, "sha256:") {
			sha = strings.TrimPrefix(asset.Digest, "sha256:")
		}
		// GitHub added asset digests after SDL_ttf 3.2.2 was published. Retain
		// the existing reviewed checksum only for that exact legacy asset.
		if asset.Digest == "" {
			var knownURL, knownSHA, knownName string
			if component == "SDL3" {
				knownURL, knownSHA, knownName, _, _ = SDL3Pin(goos, goarch)
			} else {
				knownURL, knownSHA, knownName, _, _ = SDL3TtfPin(goos, goarch)
			}
			if knownURL == url && knownName == name {
				sha = knownSHA
			}
		}
		if !validSDLSHA(sha) {
			return SDLResolved{}, fmt.Errorf("publisher supplies no SHA-256 for %s; add a reviewed checksum before using this asset", name)
		}
		return SDLResolved{Version: version, Kind: kind, Archives: []SDLArchive{{url, strings.ToLower(sha), name}}}, nil
	}
	return SDLResolved{}, fmt.Errorf("latest matching release %s has no %s artifact", version, name)
}

func linuxSDLRepository(component, arch string) (base, index string) {
	if arch == "amd64" {
		base = steamRuntimeSniperBaseURL
		index = base + "/dists/sniper/main/binary-amd64/Packages.gz"
	} else if component == "SDL3" {
		base = "https://repo.steampowered.com/steamrt4/apt"
		index = base + "/dists/steamrt4/main/binary-arm64/Packages.gz"
	} else {
		base = "https://deb.debian.org/debian"
		index = base + "/dists/trixie/main/binary-arm64/Packages.gz"
	}
	return
}

var upstreamSDLPackageVersion = regexp.MustCompile(`^(?:[0-9]+:)?(3\.[0-9]+\.[0-9]+)(?:\+[^~-]*)?-`)

func resolveLinuxSDL(component, arch, selector string, fetch sdlMetadataFetch) (SDLResolved, error) {
	base, index := linuxSDLRepository(component, arch)
	data, err := fetch(index)
	if err != nil {
		return SDLResolved{}, err
	}
	z, err := gzip.NewReader(bytes.NewReader(data))
	if err != nil {
		return SDLResolved{}, err
	}
	defer z.Close()
	devName, runtimeName := "libsdl3-dev", "libsdl3-0"
	if component == "SDL3_ttf" {
		devName, runtimeName = "libsdl3-ttf-dev", "libsdl3-ttf0"
	}
	packages := map[string]map[string]map[string]string{devName: {}, runtimeName: {}}
	limited := &io.LimitedReader{R: z, N: (128 << 20) + 1}
	scanner := bufio.NewScanner(limited)
	scanner.Buffer(make([]byte, 64<<10), 1<<20)
	paragraph := map[string]string{}
	lastKey := ""
	consume := func() {
		name, version := paragraph["Package"], paragraph["Version"]
		if packages[name] != nil && paragraph["Architecture"] == arch {
			packages[name][version] = paragraph
		}
		paragraph = map[string]string{}
		lastKey = ""
	}
	for scanner.Scan() {
		line := scanner.Text()
		if line == "" {
			consume()
			continue
		}
		if strings.HasPrefix(line, " ") || strings.HasPrefix(line, "\t") {
			if lastKey != "" {
				paragraph[lastKey] += " " + strings.TrimSpace(line)
			}
			continue
		}
		if key, value, okay := strings.Cut(line, ": "); okay {
			paragraph[key] = value
			lastKey = key
		}
	}
	consume()
	if err := scanner.Err(); err != nil {
		return SDLResolved{}, err
	}
	if limited.N == 0 {
		return SDLResolved{}, fmt.Errorf("uncompressed SDK metadata exceeds 128 MiB")
	}
	selected, upstream := "", ""
	for version := range packages[devName] {
		match := upstreamSDLPackageVersion.FindStringSubmatch(version)
		if match == nil || packages[runtimeName][version] == nil || !sdlVersionMatches(match[1], selector, component) {
			continue
		}
		if newerSDL(match[1], upstream) || match[1] == upstream && debianRevisionCompare(version, selected) > 0 {
			selected, upstream = version, match[1]
		}
	}
	if selected == "" {
		return SDLResolved{}, fmt.Errorf("no matching stable %s dev/runtime pair for %s in %s", selector, arch, index)
	}
	result := SDLResolved{Version: upstream, Kind: "deb", Dependencies: packages[runtimeName][selected]["Depends"]}
	for _, name := range []string{devName, runtimeName} {
		p := packages[name][selected]
		file := p["Filename"]
		archive := path.Base(file)
		if !strings.HasPrefix(file, "pool/main/libs/") || strings.Contains(file, "..") || strings.ContainsAny(file, "\\\r\n?#") || !strings.HasSuffix(archive, "_"+arch+".deb") || !validSDLSHA(p["SHA256"]) {
			return SDLResolved{}, fmt.Errorf("invalid publisher package metadata for %s", name)
		}
		result.Archives = append(result.Archives, SDLArchive{base + "/" + file, strings.ToLower(p["SHA256"]), archive})
	}
	return result, nil
}

// Debian orders '~' before end-of-string, letters before punctuation, and
// digit runs numerically. This matters when two rebuilds share an SDL version.
func debianRevisionCompare(a, b string) int {
	split := func(s string) (epoch, upstream, revision string) {
		epoch = "0"
		if e, rest, found := strings.Cut(s, ":"); found {
			epoch, s = e, rest
		}
		upstream, revision = s, "0"
		if i := strings.LastIndex(s, "-"); i >= 0 {
			upstream, revision = s[:i], s[i+1:]
		}
		return
	}
	ae, au, ar := split(a)
	be, bu, br := split(b)
	for _, pair := range [][2]string{{ae, be}, {au, bu}, {ar, br}} {
		if d := compareDebianPart(pair[0], pair[1]); d != 0 {
			return d
		}
	}
	return 0
}

func compareDebianPart(a, b string) int {
	order := func(c byte) int {
		if c == '~' {
			return -1
		}
		if c == 0 || c >= '0' && c <= '9' {
			return 0
		}
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' {
			return int(c)
		}
		return int(c) + 256
	}
	digit := func(s string) bool { return len(s) > 0 && s[0] >= '0' && s[0] <= '9' }
	for a != "" || b != "" {
		for a != "" && !digit(a) || b != "" && !digit(b) {
			var ac, bc byte
			if a != "" {
				ac = a[0]
			}
			if b != "" {
				bc = b[0]
			}
			if d := order(ac) - order(bc); d != 0 {
				return d
			}
			if a != "" {
				a = a[1:]
			}
			if b != "" {
				b = b[1:]
			}
		}
		for strings.HasPrefix(a, "0") {
			a = a[1:]
		}
		for strings.HasPrefix(b, "0") {
			b = b[1:]
		}
		first := 0
		for digit(a) && digit(b) {
			if first == 0 {
				first = int(a[0]) - int(b[0])
			}
			a = a[1:]
			b = b[1:]
		}
		if digit(a) {
			return 1
		}
		if digit(b) {
			return -1
		}
		if first != 0 {
			return first
		}
	}
	return 0
}

func validSDLSHA(sha string) bool {
	decoded, err := hex.DecodeString(sha)
	return err == nil && len(decoded) == 32
}

func ValidateSDLSDKLock(lock SDLSDKLock, goos, goarch, sdlVersion, ttfVersion string) error {
	if goarch != "amd64" && goarch != "arm64" || goos != "darwin" && goos != "windows" && goos != "linux" {
		return fmt.Errorf("unsupported SDK platform %s/%s", goos, goarch)
	}
	if lock.Schema != 1 || lock.GOOS != goos || lock.GOARCH != goarch {
		return fmt.Errorf("SDL SDK lock does not match %s/%s", goos, goarch)
	}
	for _, item := range []struct {
		component, selector string
		sdk                 SDLResolved
	}{{"SDL3", sdlVersion, lock.SDL}, {"SDL3_ttf", ttfVersion, lock.TTF}} {
		if !sdlVersionMatches(item.sdk.Version, item.selector, item.component) {
			return fmt.Errorf("locked %s %s does not match %s", item.component, item.sdk.Version, item.selector)
		}
		count := 1
		if goos == "linux" {
			count = 2
		}
		if len(item.sdk.Archives) != count {
			return fmt.Errorf("incomplete locked %s SDK", item.component)
		}
		for i, archive := range item.sdk.Archives {
			if !validSDLSHA(archive.SHA256) || archive.Archive == "" || path.Base(archive.Archive) != archive.Archive || strings.ContainsAny(archive.Archive, "\\\r\n;\" ") || !strings.HasSuffix(archive.URL, "/"+archive.Archive) {
				return fmt.Errorf("invalid locked %s archive", item.component)
			}
			if goos == "linux" {
				base, _ := linuxSDLRepository(item.component, goarch)
				source := "libsdl3"
				names := []string{"libsdl3-dev", "libsdl3-0"}
				if item.component == "SDL3_ttf" {
					source = "libsdl3-ttf"
					names = []string{"libsdl3-ttf-dev", "libsdl3-ttf0"}
				}
				parts := strings.Split(archive.Archive, "_")
				if len(parts) != 3 || parts[0] != names[i] {
					return fmt.Errorf("unexpected locked %s package name", item.component)
				}
				upstream := upstreamSDLPackageVersion.FindStringSubmatch(parts[1])
				if upstream == nil || upstream[1] != item.sdk.Version {
					return fmt.Errorf("locked %s archive version differs from SDK version", item.component)
				}
				if item.sdk.Kind != "deb" || archive.URL != base+"/pool/main/libs/"+source+"/"+archive.Archive || strings.Contains(archive.URL, "..") || strings.ContainsAny(archive.URL, "?#") || !strings.HasSuffix(archive.Archive, "_"+goarch+".deb") {
					return fmt.Errorf("unexpected locked Linux SDK publisher or architecture")
				}
			} else {
				name, kind := officialSDLAsset(item.component, goos, goarch, item.sdk.Version)
				repo := "SDL"
				if item.component == "SDL3_ttf" {
					repo = "SDL_ttf"
				}
				wantURL := "https://github.com/libsdl-org/" + repo + "/releases/download/release-" + item.sdk.Version + "/" + name
				if item.sdk.Kind != kind || archive.Archive != name || archive.URL != wantURL {
					return fmt.Errorf("unexpected locked SDL release artifact")
				}
			}
		}
		if goos == "linux" {
			parts := func(a string) string { _, rest, _ := strings.Cut(a, "_"); return rest }
			if parts(item.sdk.Archives[0].Archive) != parts(item.sdk.Archives[1].Archive) {
				return fmt.Errorf("locked %s headers/runtime versions differ", item.component)
			}
		}
	}
	return nil
}

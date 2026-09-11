// Package linuxsdk provisions a Linux SDK as data on the maintainer host.
// It never installs Debian packages or executes their programs/maintainer scripts.
package linuxsdk

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

type Package struct {
	Name         string `json:"name"`
	Version      string `json:"version"`
	Architecture string `json:"architecture"`
	URL          string `json:"url"`
	SHA256       string `json:"sha256"`
	Size         int64  `json:"size"`
	Source       string `json:"source"`
	Depends      string `json:"depends,omitempty"`
	Provides     string `json:"provides,omitempty"`
}
type Lock struct {
	Schema       int       `json:"schema"`
	Distribution string    `json:"distribution"`
	Arch         string    `json:"arch"`
	Roots        []string  `json:"roots"`
	Packages     []Package `json:"packages"`
}

var roots = []string{"libwebkit2gtk-4.1-dev", "libgtk-3-dev", "libc6-dev", "linux-libc-dev", "gstreamer1.0-plugins-base", "gstreamer1.0-plugins-good", "libgdk-pixbuf2.0-bin", "libgtk-3-bin", "librsvg2-common", "glib-networking", "adwaita-icon-theme", "shared-mime-info", "bubblewrap", "xdg-dbus-proxy"}
var client = &http.Client{Timeout: 5 * time.Minute}

// Keep the whole runtime closure on this baseline, not just the Go/CGO shell.
// Explicit lock refreshes continue to pick up this suite's security updates.
const Distribution = "debian-bookworm"
const GLIBCBaseline = "2.36"

// Resolve is an explicit maintainer update, not part of ordinary release builds.
// Normal builds consume a reviewed lock and check every downloaded package hash.
func Resolve(arch string) (Lock, error) {
	lock := Lock{Schema: 1, Distribution: Distribution, Arch: arch, Roots: roots}
	if arch != "amd64" && arch != "arm64" {
		return lock, fmt.Errorf("unsupported SDK architecture %s", arch)
	}
	var packages []Package
	for _, repo := range [][2]string{{"https://deb.debian.org/debian", "bookworm"}, {"https://deb.debian.org/debian", "bookworm-updates"}, {"https://security.debian.org/debian-security", "bookworm-security"}} {
		url := repo[0] + "/dists/" + repo[1] + "/main/binary-" + arch + "/Packages.xz"
		response, err := client.Get(url)
		if err != nil {
			return lock, err
		}
		if response.StatusCode != 200 {
			response.Body.Close()
			return lock, fmt.Errorf("%s: HTTP %d", url, response.StatusCode)
		}
		command := exec.Command("xz", "-dc")
		command.Stdin = io.LimitReader(response.Body, 64<<20)
		z, err := command.StdoutPipe()
		if err != nil {
			response.Body.Close()
			return lock, err
		}
		if err = command.Start(); err != nil {
			response.Body.Close()
			return lock, err
		}
		bounded := &io.LimitedReader{R: z, N: 256 << 20}
		list, err := readIndex(bounded, repo[0], arch)
		if bounded.N == 0 {
			err = fmt.Errorf("Debian index exceeds size limit")
		}
		if err != nil {
			command.Process.Kill()
		}
		waitErr := command.Wait()
		response.Body.Close()
		if err != nil {
			return lock, err
		}
		if waitErr != nil {
			return lock, waitErr
		}
		packages = append(packages, list...)
	}
	selected, err := selectPackages(packages, roots)
	lock.Packages = selected
	return lock, err
}

func readIndex(r io.Reader, base, arch string) ([]Package, error) {
	var result []Package
	s := bufio.NewScanner(r)
	s.Buffer(make([]byte, 4096), 2<<20)
	fields := map[string]string{}
	last := ""
	flush := func() error {
		if fields["Package"] == "" {
			return nil
		}
		if fields["Architecture"] != arch && fields["Architecture"] != "all" {
			return nil
		}
		size, err := strconv.ParseInt(fields["Size"], 10, 64)
		if err != nil {
			return err
		}
		p := Package{Name: fields["Package"], Version: fields["Version"], Architecture: fields["Architecture"], URL: base + "/" + fields["Filename"], SHA256: fields["SHA256"], Size: size, Source: fields["Source"], Provides: fields["Provides"]}
		p.Depends = strings.Trim(fields["Pre-Depends"]+", "+fields["Depends"], ", ")
		if p.Source == "" {
			p.Source = p.Name
		}
		// Validate size/URL/hash limits only for the selected SDK closure, not
		// unrelated packages (the archive also indexes multi-GB game data).
		result = append(result, p)
		return nil
	}
	for s.Scan() {
		line := s.Text()
		if line == "" {
			if err := flush(); err != nil {
				return nil, err
			}
			fields = map[string]string{}
			last = ""
			continue
		}
		if strings.HasPrefix(line, " ") || strings.HasPrefix(line, "\t") {
			if last != "" {
				fields[last] += " " + strings.TrimSpace(line)
			}
			continue
		}
		key, value, ok := strings.Cut(line, ":")
		last = ""
		if !ok {
			return nil, fmt.Errorf("malformed Debian index")
		}
		switch key {
		case "Package", "Version", "Architecture", "Filename", "SHA256", "Size", "Source", "Depends", "Pre-Depends", "Provides":
			fields[key] = strings.TrimSpace(value)
			last = key
		}
	}
	if err := s.Err(); err != nil {
		return nil, err
	}
	if err := flush(); err != nil {
		return nil, err
	}
	return result, nil
}

// A foreign explicit architecture needs multiarch resolution, not qualifier
// stripping. Refuse it until that behavior is implemented and tested.
var dependency = regexp.MustCompile(`^([a-z0-9][a-z0-9+.-]*)(?::(?:any|native))?(?:\s+\((>=|<=|=|>>|<<)\s+([^()]+)\))?$`)
var validPackageName = regexp.MustCompile(`^[a-z0-9][a-z0-9+.-]+$`)

func parseDependency(value string) (name, operator, version string, err error) {
	m := dependency.FindStringSubmatch(strings.TrimSpace(value))
	if m == nil {
		return "", "", "", fmt.Errorf("unsupported binary dependency %q", value)
	}
	return m[1], m[2], m[3], nil
}
func matches(actual, operator, wanted string) bool {
	c := compareVersion(actual, wanted)
	switch operator {
	case "":
		return true
	case "=":
		return c == 0
	case ">=":
		return c >= 0
	case "<=":
		return c <= 0
	case ">>":
		return c > 0
	case "<<":
		return c < 0
	}
	return false
}
func selectPackages(packages []Package, rootNames []string) ([]Package, error) {
	byName := map[string][]Package{}
	providers := map[string][]Package{}
	for _, p := range packages {
		byName[p.Name] = append(byName[p.Name], p)
		for _, provided := range strings.Split(p.Provides, ",") {
			if provided == "" {
				continue
			}
			name, _, _, err := parseDependency(provided)
			if err != nil {
				return nil, err
			}
			providers[name] = append(providers[name], p)
		}
	}
	for _, list := range byName {
		sort.SliceStable(list, func(i, j int) bool { return compareVersion(list[i].Version, list[j].Version) > 0 })
	}
	for _, list := range providers {
		sort.SliceStable(list, func(i, j int) bool {
			if list[i].Name == list[j].Name {
				return compareVersion(list[i].Version, list[j].Version) > 0
			}
			return list[i].Name < list[j].Name
		})
	}
	chosen := map[string]Package{}
	queue := append([]string(nil), rootNames...)
	for len(queue) > 0 {
		group := queue[0]
		queue = queue[1:]
		found := false
		for _, alternative := range strings.Split(group, "|") {
			name, op, version, err := parseDependency(alternative)
			if err != nil {
				return nil, err
			}
			candidates := append(append([]Package(nil), byName[name]...), providers[name]...)
			for _, p := range candidates {
				actual := p.Version
				if p.Name != name {
					actual = ""
					for _, provided := range strings.Split(p.Provides, ",") {
						n, _, v, _ := parseDependency(provided)
						if n == name {
							actual = v
							break
						}
					}
					if op != "" && actual == "" {
						continue
					}
				}
				if !matches(actual, op, version) {
					continue
				}
				if old, ok := chosen[p.Name]; ok {
					if old.Version != p.Version {
						continue
					}
					found = true
					break
				}
				chosen[p.Name] = p
				found = true
				if p.Depends != "" {
					queue = append(queue, strings.Split(p.Depends, ",")...)
				}
				break
			}
			if found {
				break
			}
		}
		if !found {
			return nil, fmt.Errorf("cannot resolve compatible package for %s (refresh matching package family; resolver never ignores constraints)", group)
		}
	}
	result := make([]Package, 0, len(chosen))
	for _, p := range chosen {
		result = append(result, p)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].Name < result[j].Name })
	return result, nil
}

func validatePackage(p Package, arch string) error {
	source := strings.Fields(p.Source)
	if !validPackageName.MatchString(p.Name) || len(source) == 0 || !validPackageName.MatchString(source[0]) {
		return fmt.Errorf("invalid package/source name %s", p.Name)
	}
	h, err := hex.DecodeString(p.SHA256)
	if err != nil || len(h) != 32 || p.Size <= 0 || p.Size > 512<<20 || p.Name == "" || p.Version == "" || (p.Architecture != arch && p.Architecture != "all") {
		return fmt.Errorf("invalid SDK package %s", p.Name)
	}
	if (!strings.HasPrefix(p.URL, "https://deb.debian.org/debian/pool/") && !strings.HasPrefix(p.URL, "https://security.debian.org/debian-security/pool/")) || !strings.HasSuffix(p.URL, ".deb") || strings.ContainsAny(p.URL, "\n\r?#") || strings.Contains(p.URL, "/../") {
		return fmt.Errorf("invalid Debian package URL %s", p.URL)
	}
	return nil
}
func (l Lock) Validate() error {
	if l.Schema != 1 || l.Distribution != Distribution || (l.Arch != "amd64" && l.Arch != "arm64") || len(l.Packages) == 0 || len(l.Packages) > 2000 {
		return fmt.Errorf("unsupported Linux SDK lock")
	}
	seen := map[string]bool{}
	for _, p := range l.Packages {
		if seen[p.Name] {
			return fmt.Errorf("duplicate SDK package %s", p.Name)
		}
		seen[p.Name] = true
		if err := validatePackage(p, l.Arch); err != nil {
			return err
		}
	}
	for _, root := range roots {
		if !seen[root] {
			return fmt.Errorf("SDK missing required package %s", root)
		}
	}
	_, err := selectPackages(l.Packages, roots)
	return err
}

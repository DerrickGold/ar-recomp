package linuxsdk

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"testing"
)

type archiveTransport func(*http.Request) (*http.Response, error)

func (transport archiveTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	return transport(request)
}

func TestLockedSDKDownloadsRecoverFromTheOfficialArchive(t *testing.T) {
	for _, mode := range []string{"mirror", "missing", "gone", "multiple-builds", "wrong-version", "wrong-architecture", "bad-identity", "bad-checksum", "bad-size", "archive-unavailable", "mirror-error"} {
		t.Run(mode, func(t *testing.T) {
			content := []byte("locked Debian package")
			p := Package{Name: "fixture", Version: "1:2.0-1+deb12u3", Architecture: "amd64",
				URL:    "https://deb.debian.org/debian/pool/main/f/fixture/fixture.deb",
				SHA256: fmt.Sprintf("%x", sha256.Sum256(content)), Size: int64(len(content))}
			requests := []string{}
			previous := client
			client = &http.Client{Transport: archiveTransport(func(request *http.Request) (*http.Response, error) {
				requests = append(requests, request.URL.String())
				status := http.StatusOK
				body := content
				switch {
				case request.URL.String() == p.URL:
					if mode != "mirror" {
						status = http.StatusNotFound
					}
					if mode == "gone" {
						status = http.StatusGone
					} else if mode == "mirror-error" {
						status = http.StatusInternalServerError
					}
				case request.URL.Host == "snapshot.debian.org" && strings.HasPrefix(request.URL.Path, "/mr/binary/"):
					if !strings.Contains(request.URL.Path, p.Name+"/"+p.Version+"/binfiles") {
						t.Error("archive lookup changed the locked package/version", request.URL)
					}
					version := p.Version
					arch := p.Architecture
					hash := strings.Repeat("b", 40)
					if mode == "wrong-version" {
						version = "newer-unlocked-version"
					} else if mode == "wrong-architecture" {
						arch = "arm64"
					} else if mode == "bad-identity" {
						hash = "../../untrusted"
					} else if mode == "archive-unavailable" {
						status = http.StatusNotFound
					}
					files := []map[string]string{
						{"architecture": "arm64", "hash": strings.Repeat("a", 40)},
						{"architecture": arch, "hash": hash},
					}
					if mode == "multiple-builds" {
						files = append(files, map[string]string{"architecture": arch, "hash": strings.Repeat("c", 40)})
					}
					body, _ = json.Marshal(map[string]any{"binary": p.Name, "binary_version": version, "result": files})
				case request.URL.Host == "snapshot.debian.org" && strings.HasPrefix(request.URL.Path, "/file/"):
					if strings.HasSuffix(request.URL.Path, strings.Repeat("a", 40)) {
						t.Error("downloaded an archive package for the wrong architecture")
					}
					if mode == "bad-checksum" || (mode == "multiple-builds" && strings.HasSuffix(request.URL.Path, strings.Repeat("b", 40))) {
						body = bytes.Repeat([]byte("x"), len(content))
					} else if mode == "bad-size" {
						body = append(append([]byte(nil), content...), 'x')
					}
				default:
					t.Error("unexpected download URL", request.URL)
					status = http.StatusNotFound
				}
				return &http.Response{StatusCode: status, Body: io.NopCloser(bytes.NewReader(body)),
					Header: make(http.Header), Request: request}, nil
			})}
			t.Cleanup(func() { client = previous })
			cache := t.TempDir()
			name, err := download(p, cache)
			valid := mode == "mirror" || mode == "missing" || mode == "gone" || mode == "multiple-builds"
			if (err == nil) != valid {
				t.Fatal("unexpected recovery result", name, err, requests)
			}
			if valid {
				data, err := os.ReadFile(name)
				if err != nil || !bytes.Equal(data, content) {
					t.Fatal("unverified archive package published", err)
				}
				count := len(requests)
				if _, err := download(p, cache); err != nil || len(requests) != count {
					t.Fatal("verified cache did not avoid network requests", err)
				}
			}
			entries, err := os.ReadDir(cache)
			if err != nil || len(entries) != map[bool]int{true: 1, false: 0}[valid] {
				t.Fatal("failed download or partial file retained", entries, err)
			}
			if (mode == "mirror" || mode == "mirror-error") && len(requests) != 1 {
				t.Fatal("archive lookup attempted without a missing package", requests)
			}
		})
	}
}

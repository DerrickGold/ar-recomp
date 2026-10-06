package linuxsdk

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
)

// Debian removes superseded packages from live mirrors. The official archive
// locates the exact locked version; download still enforces its SHA-256/size.
func archivedPackageURLs(p Package) ([]string, error) {
	endpoint := "https://snapshot.debian.org/mr/binary/" + url.PathEscape(p.Name) +
		"/" + url.PathEscape(p.Version) + "/binfiles"
	response, err := client.Get(endpoint)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("Debian archive lookup for %s %s: HTTP %d", p.Name, p.Version, response.StatusCode)
	}
	var record struct {
		Binary  string `json:"binary"`
		Version string `json:"binary_version"`
		Files   []struct {
			Architecture string `json:"architecture"`
			Hash         string `json:"hash"`
		} `json:"result"`
	}
	if err := json.NewDecoder(io.LimitReader(response.Body, 1<<20)).Decode(&record); err != nil {
		return nil, fmt.Errorf("Debian archive metadata: %w", err)
	}
	if record.Binary != p.Name || record.Version != p.Version {
		return nil, fmt.Errorf("Debian archive returned the wrong package/version for %s %s", p.Name, p.Version)
	}
	var urls []string
	seen := make(map[string]bool)
	for _, file := range record.Files {
		if file.Architecture != p.Architecture || seen[file.Hash] {
			continue
		}
		hash, err := hex.DecodeString(file.Hash)
		if err != nil || len(hash) != 20 {
			return nil, fmt.Errorf("invalid Debian archive file identity for %s", p.Name)
		}
		seen[file.Hash] = true
		urls = append(urls, "https://snapshot.debian.org/file/"+file.Hash)
	}
	if len(urls) == 0 {
		return nil, fmt.Errorf("Debian archive has no locked package %s %s for %s", p.Name, p.Version, p.Architecture)
	}
	return urls, nil
}

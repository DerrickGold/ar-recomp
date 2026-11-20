package builder

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

const (
	modIndexName    = "index.json"
	modIndexVersion = 2 // Bump when archive validation or extracted asset naming changes.
	maxModIndexSize = maxDiscoveredMods * 3 * maxModMetadata
)

// File identity detects replacements that preserve mtime. Change time detects
// in-place edits even when a mod manager restores the original mtime.
type modFileStamp struct {
	Size     int64     `json:"size"`
	Modified int64     `json:"modified"`
	Changed  int64     `json:"changed"`
	Volume   uint64    `json:"volume"`
	ID       [2]uint64 `json:"fileId"`
}

func readModFileStamp(file *os.File, info os.FileInfo) modFileStamp {
	volume, id, changed := modFileIdentity(file, info)
	return modFileStamp{
		Size: info.Size(), Modified: info.ModTime().UnixNano(),
		Changed: changed, Volume: volume, ID: id,
	}
}

func (stamp modFileStamp) usable() bool {
	return stamp.ID != [2]uint64{} && stamp.Changed != 0
}

type modIndexEntry struct {
	modMetadata
	modPayload
	Source     modFileStamp         `json:"source"`
	Digest     string               `json:"digest"`
	AssetSizes []uint64             `json:"assetSizes"`
	Language   *lk.PreparedLanguage `json:"language,omitempty"`
	ManualBook manualAlbumVerdict   `json:"manualBook,omitzero"`
}

type modIndex struct {
	Version  int                       `json:"version"`
	Platform string                    `json:"platform"`
	Entries  map[string]*modIndexEntry `json:"entries"`
}

func readModIndex(cache string) map[string]*modIndexEntry {
	data, absent, err := readBoundedRegularFile(filepath.Join(cache, modIndexName), maxModIndexSize)
	if absent || err != nil {
		return nil
	}
	var index modIndex
	if json.Unmarshal(data, &index) != nil || index.Version != modIndexVersion ||
		index.Platform != runtime.GOOS || len(index.Entries) > maxDiscoveredMods {
		return nil
	}
	return index.Entries
}

func (s *modStore) saveIndex() {
	if !s.indexChanged {
		return
	}
	data, err := json.Marshal(modIndex{Version: modIndexVersion, Platform: runtime.GOOS, Entries: s.index})
	if err == nil && len(data) <= maxModIndexSize {
		// Publication of the active overlay does not depend on this cache.
		_ = writeAtomicFile(filepath.Join(s.cache, modIndexName), data)
	}
}

func (entry *modIndexEntry) matches(cache string, stamp modFileStamp) bool {
	if entry == nil || !stamp.usable() || entry.Source != stamp ||
		validateModMetadata(entry.modMetadata) != nil || entry.modPayload.validate() != nil {
		return false
	}
	if len(entry.Digest) != hex.EncodedLen(sha256.Size) {
		return false
	}
	digest, err := hex.DecodeString(entry.Digest)
	if err != nil || hex.EncodeToString(digest) != entry.Digest {
		return false
	}
	if entry.Type == languageModType {
		_, err := lk.ParseLanguageRelease(entry.Version)
		return err == nil && entry.Language != nil && lk.ValidLanguageModID(entry.ID, entry.Language.ID) &&
			entry.Language.Cached(languagePacksRoot(filepath.Dir(cache)), entry.Digest)
	}
	assetCount := len(entry.Tracks)
	if entry.Type == titleModType {
		assetCount = 1
	}
	if entry.Type == manualModType {
		if !entry.ManualBook.validGeometry() {
			return false
		}
		assetCount = 1
	}
	if len(entry.AssetSizes) != assetCount {
		return false
	}
	for i, size := range entry.AssetSizes {
		if size == 0 || size > maxTrackBytes || (entry.Type == titleModType && size > maxTitlePNGBytes) {
			return false
		}
		if entry.Type == manualModType && size > maxManualBytes {
			return false
		}
		path := filepath.Join(cache, entry.cacheFilename(entry.Digest, i))
		info, err := os.Lstat(path)
		if err != nil || !matchesModAsset(info, size) {
			return false
		}
	}
	return true
}

func matchesModAsset(info os.FileInfo, size uint64) bool {
	return info.Mode().IsRegular() && uint64(info.Size()) == size
}

func (s *modStore) rememberInstalledArchive(path string, entry *modIndexEntry) {
	if !entry.Source.usable() || len(s.index) >= maxDiscoveredMods {
		return
	}
	file, err := openModFile(path)
	if err != nil {
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return
	}
	stamp := readModFileStamp(file, info)
	// Rename can change ctime. Keep the verified metadata if the destination
	// is the same file that was validated in staging.
	if stamp.usable() && stamp.ID == entry.Source.ID && stamp.Volume == entry.Source.Volume &&
		stamp.Size == entry.Source.Size && stamp.Modified == entry.Source.Modified {
		entry.Source = stamp
		s.index[path] = entry
		s.indexChanged = true
	}
}

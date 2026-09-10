package localizationkit

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
)

// NativeSourceMetadata is shared by build and GUI callers. Locale describes
// prose, while SourceProfile and Target describe its game semantic contract.
func (d *Decoder) NativeSourceMetadata() PackMetadata {
	target := "reference-only"
	id := "source." + d.profile.ID
	if d.profile.ID == "us" {
		target, id = "us-runtime", "native-us"
	}
	return PackMetadata{ID: id, Locale: d.profile.Locale, Name: "Native " + d.profile.Label,
		Autonym: d.profile.Label, Author: "Local ROM extraction", License: "Local use only",
		Direction: "auto", Target: target, SourceProfile: d.profile.ID, Fallback: "native-us", Coverage: "complete"}
}

// EnsureNativeUSSource never overwrites an existing pack. The build and editor
// share this entry point; neither invokes Python or guesses a regional profile.
func EnsureNativeUSSource(directory string, rom []byte) (*AuthorPack, error) {
	if pack, err := OpenNativeUSSource(directory); pack != nil || err != nil {
		return pack, err
	}
	d, err := NewDecoder(rom)
	if err != nil {
		return nil, err
	}
	if d.ReleaseID() != "us" {
		return nil, fmt.Errorf("the runtime baseline requires the US ROM")
	}
	pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
	if err != nil {
		return nil, err
	}
	return InstallNativeUSSource(directory, pack)
}

// OpenNativeUSSource validates a generated baseline without extracting or
// writing anything. A missing manifest returns (nil, nil); incompatible or
// incomplete packs return an error. Build reuse and GUI readiness agree here.
func OpenNativeUSSource(directory string) (*AuthorPack, error) {
	if _, err := os.Lstat(filepath.Join(directory, "pack.ini")); err == nil {
		pack, err := OpenAuthorPack(directory)
		if err != nil {
			return nil, err
		}
		m := pack.Manifest().Metadata()
		if m.ID != "native-us" || m.SourceProfile != "us" || m.Target != "us-runtime" || m.Coverage != "complete" {
			return nil, fmt.Errorf("existing native US source is incompatible; move it aside explicitly before extracting")
		}
		return pack, nil
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	return nil, nil
}

// InstallNativeUSSource shares the build's non-overwrite policy with a GUI
// which already holds its validated extraction. No second ROM scan is needed.
func InstallNativeUSSource(directory string, pack *AuthorPack) (*AuthorPack, error) {
	if old, err := OpenNativeUSSource(directory); old != nil || err != nil {
		return old, err
	}
	if pack == nil {
		return nil, fmt.Errorf("native source is required")
	}
	m := pack.Manifest().Metadata()
	if m.ID != "native-us" || m.SourceProfile != "us" || m.Target != "us-runtime" || m.Coverage != "complete" {
		return nil, fmt.Errorf("incompatible native US source")
	}
	p, err := NewSourceProject(pack)
	if err != nil {
		return nil, err
	}
	if _, err = InstallAuthorProject(directory, p, false); err != nil {
		return nil, err
	}
	return pack, nil
}

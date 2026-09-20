package localization

import (
	"fmt"
	"io"
	"path/filepath"
	"strings"
)

// FindInstalledPack matches package identity, including disabled installations
// and archives with a different filename. Ambiguous copies cannot be updated.
func FindInstalledPack(root, id string) (*InstalledPackSummary, error) {
	rows, err := ListInstalledPacks(root)
	if err != nil {
		return nil, err
	}
	var found *InstalledPackSummary
	for i := range rows {
		row := &rows[i]
		if !strings.EqualFold(row.Metadata.ID, id) && row.Key != id {
			continue
		}
		if row.Error != "" {
			return nil, fmt.Errorf("installed package %s: %s", row.Key, row.Error)
		}
		if found != nil {
			return nil, fmt.Errorf("multiple installed copies of package %s; remove the duplicate before updating", id)
		}
		found = row
	}
	return found, nil
}

// UpdateInstalledProject refreshes only the inspected installation. It never
// installs a new pack or restores one uninstalled while the project was saved.
// The archive path is local iteration, not publication: all draft messages are
// retained and the project gains no permission to export a publication.
func UpdateInstalledProject(root string, project *AuthorProject, expected InstalledPackSummary) (string, error) {
	if project == nil || !project.installationReady {
		return "", fmt.Errorf("prepare installation before updating")
	}
	id := project.Pack().Manifest().Metadata().ID
	if expected.Revision == "" || expected.Metadata.ID != id {
		return "", ErrProjectConflict
	}
	unlock, err := authorLock(root)
	if err != nil {
		return "", err
	}
	defer unlock()
	current, err := FindInstalledPack(root, id)
	if err != nil {
		return "", err
	}
	if current == nil || current.Key != expected.Key || current.Revision != expected.Revision {
		return "", ErrProjectConflict
	}
	if current.Archive {
		path := filepath.Join(root, current.Key)
		err := atomicAuthorFile(path, func(w io.Writer) error { return project.writeArchive(w, "publication") })
		return path, err
	}
	dir, err := installedDirectory(root, current.Key)
	if err != nil {
		return "", err
	}
	unlockDirectory, err := authorLock(dir)
	if err != nil {
		return "", err
	}
	defer unlockDirectory()
	// Enable/disable and uninstall use the directory lock independently of the
	// library lock. Recheck after acquiring it so a save cannot undo those actions.
	row, err := readInstalledSummary(root, current.Key)
	if err != nil {
		return "", err
	}
	if row.Revision != expected.Revision || row.Metadata.ID != id {
		return "", ErrProjectConflict
	}
	return writeInstalledProject(dir, project, true, 0)
}

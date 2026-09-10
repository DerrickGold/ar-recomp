package localization

import (
	"archive/zip"
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"strings"
)

// archiveIndex is shared by cheap library discovery and full imports. Directory
// records from ordinary ZIP tools are allowed, but never become file payloads.
// All paths, kinds and declared expansion budgets are checked before reads.
type archiveIndex struct {
	files    map[string]*zip.File
	ordered  []*zip.File
	header   archiveHeader
	manifest *PackManifest
	revision string
}

func readArchiveMember(member *zip.File, maximum uint64) ([]byte, error) {
	if member == nil || member.UncompressedSize64 > maximum {
		return nil, fmt.Errorf("missing or oversized archive member")
	}
	r, err := member.Open()
	if err != nil {
		return nil, err
	}
	data, err := io.ReadAll(io.LimitReader(r, int64(member.UncompressedSize64)+1))
	closeErr := r.Close()
	if err != nil {
		return nil, err
	}
	if closeErr != nil {
		return nil, closeErr
	}
	if uint64(len(data)) != member.UncompressedSize64 {
		return nil, fmt.Errorf("archive member size mismatch: %s", member.Name)
	}
	return data, nil
}

func inspectAuthorArchive(input io.ReaderAt, size int64) (*archiveIndex, error) {
	if size <= 0 || size > MaxAuthorArchiveBytes {
		return nil, fmt.Errorf("archive exceeds size limit")
	}
	z, err := zip.NewReader(input, size)
	if err != nil {
		return nil, fmt.Errorf("invalid language ZIP: %w", err)
	}
	if len(z.File) == 0 || len(z.File) > maxArchiveFiles {
		return nil, fmt.Errorf("invalid archive member count")
	}
	index := &archiveIndex{files: map[string]*zip.File{}}
	kinds := map[string]bool{}
	digest := sha256.New()
	remaining := uint64(MaxAuthorPackBytes)
	for _, member := range z.File {
		dir := member.Mode().IsDir()
		name := member.Name
		if dir {
			name = strings.TrimSuffix(name, "/")
		}
		if !PortablePackPath(name) || member.Flags&1 != 0 || (member.Method != zip.Store && member.Method != zip.Deflate) ||
			(dir && (name == member.Name || member.UncompressedSize64 != 0)) ||
			(!dir && (!member.Mode().IsRegular() || member.Mode()&0111 != 0)) {
			return nil, fmt.Errorf("unsafe archive member %q", member.Name)
		}
		key := strings.ToLower(name)
		if _, exists := kinds[key]; exists {
			return nil, fmt.Errorf("duplicate archive member %s", name)
		}
		for other, otherDir := range kinds {
			if (!otherDir && strings.HasPrefix(key, other+"/")) || (!dir && strings.HasPrefix(other, key+"/")) {
				return nil, fmt.Errorf("conflicting archive paths")
			}
		}
		kinds[key] = dir
		if dir {
			continue
		}
		if member.UncompressedSize64 > remaining {
			return nil, fmt.Errorf("archive expansion exceeds size limit")
		}
		limit := uint64(MaxAuthorScriptBytes)
		switch {
		case name == "pack.ini":
			limit = MaxPackManifestBytes
		case name == "package.json":
			limit = 4096
		case name == "author-project.json":
			limit = 2 << 20
		case strings.HasPrefix(name, "notices/"):
			limit = 1 << 20
		case strings.HasSuffix(strings.ToLower(name), ".ttf") || strings.HasSuffix(strings.ToLower(name), ".otf"):
			limit = MaxPackFontBytes
		}
		if member.UncompressedSize64 > limit {
			return nil, fmt.Errorf("archive member exceeds size limit: %s", name)
		}
		remaining -= member.UncompressedSize64
		index.files[name] = member
		index.ordered = append(index.ordered, member)
		// Revision is a change detector for cooperative UI operations, not an
		// authenticity signature. Cache identity separately hashes all ZIP bytes.
		fmt.Fprintf(digest, "%s\x00%d\x00%d\x00%08x\n", name, member.CompressedSize64, member.UncompressedSize64, member.CRC32)
	}
	header, err := readArchiveMember(index.files["package.json"], 4096)
	if err != nil {
		return nil, fmt.Errorf("package.json: %w", err)
	}
	if err = strictAuthorJSON(header, &index.header); err != nil {
		return nil, fmt.Errorf("invalid package.json: %w", err)
	}
	if index.header.Format != "actraiser-language-archive" || index.header.Version != 1 ||
		(index.header.Kind != "backup" && index.header.Kind != "publication") {
		return nil, fmt.Errorf("unsupported language archive schema")
	}
	data, err := readArchiveMember(index.files["pack.ini"], MaxPackManifestBytes)
	if err != nil {
		return nil, fmt.Errorf("pack.ini: %w", err)
	}
	index.manifest, err = ParsePackManifest(string(data), "pack.ini")
	if err != nil {
		return nil, err
	}
	if err = validateAuthorPackPaths(index.manifest); err != nil {
		return nil, err
	}
	index.revision = fmt.Sprintf("%x", digest.Sum(nil))
	return index, nil
}

// OpenAuthorInput accepts a source directory or a validated publication/backup.
// Runtime discovery uses publication-only inspection, never private backups.
func OpenAuthorInput(path string) (*AuthorProject, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if info.IsDir() {
		return OpenAuthorProjectDirectory(path)
	}
	f, err := openRegularAuthorFile(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return ReadAuthorArchive(f, info.Size())
}

// ReviewedAll is an explicit publisher action for editor-free authored packs.
// It does not grant redistribution rights or remove source-baseline checks.
func (p *AuthorProject) ReviewedAll() (*AuthorProject, error) {
	var progress strings.Builder
	for _, script := range p.pack.workspace.scripts {
		for _, message := range script.messages {
			fmt.Fprintf(&progress, "%s\tdone\n", message.ID)
		}
	}
	w, err := newAuthorWorkspace(p.pack.manifest.metadata.SourceProfile, p.pack.manifest.metadata.Coverage, p.pack.workspace.scripts, progress.String())
	if err != nil {
		return nil, err
	}
	next := p.clone()
	next.pack, err = p.pack.withWorkspace(w)
	return next, err
}

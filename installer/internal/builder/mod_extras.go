package builder

import (
	"archive/zip"
	"crypto/sha256"
	"fmt"
	"io"
)

// Optional root files supported by the ActRaiser archive format, in stable
// export order. Keep their names and contents when revising a published pack.
var modExtraNames = []string{"LICENSE", "LICENSE.md", "LICENSE.txt", "README.md", "icon.png"}

type modExtra struct {
	name string
	open func() (io.ReadCloser, error)
}

func (pack modArchive) extras() []modExtra {
	var extras []modExtra
	for _, name := range modExtraNames {
		if member := pack.files[name]; member != nil {
			extras = append(extras, modExtra{name: name, open: member.Open})
		}
	}
	return extras
}

func (extra modExtra) copyTo(out io.Writer, limit int64) (int64, error) {
	in, err := extra.open()
	if err != nil {
		return 0, err
	}
	defer in.Close()
	if limit > maxTrackBytes {
		limit = maxTrackBytes
	}
	n, err := io.Copy(out, io.LimitReader(in, limit+1))
	if n > limit {
		return n, fmt.Errorf("%s exceeds the pack size limit", extra.name)
	}
	return n, err
}

func writeModExtras(archive *zip.Writer, extras []modExtra, remaining int64, digests []string) ([]string, error) {
	for _, extra := range extras {
		member, err := archive.Create(extra.name)
		if err != nil {
			return nil, err
		}
		digest := sha256.New()
		n, err := extra.copyTo(io.MultiWriter(member, digest), remaining)
		if err != nil {
			return nil, err
		}
		remaining -= n
		digests = append(digests, fmt.Sprintf("%s:%x", extra.name, digest.Sum(nil)))
	}
	return digests, nil
}

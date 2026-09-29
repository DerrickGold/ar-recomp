package appicons

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/tc-hib/winres"
)

// WriteEXE preserves existing version/manifest resources and adds a real PE
// icon. It works on all packaging hosts, and refuses already signed inputs.
// Run BEFORE appending the Builder's payload and BEFORE signing.
func (icon Icon) WriteEXE(dst io.Writer, src io.ReadSeeker) error {
	resources, err := winres.LoadFromEXE(src)
	if err != nil && !errors.Is(err, winres.ErrNoResources) {
		return err
	}
	i, err := winres.LoadICO(bytes.NewReader(icon.ICO()))
	if err != nil {
		return err
	}
	id := winres.ID(1) // SDL's default application icon.
	if icon == Builder {
		id = winres.ID(3)
	} // Wails/winc.AppIconID.
	if err := resources.SetIcon(id, i); err != nil {
		return err
	}
	return resources.WriteToEXE(dst, src)
}

// ApplyEXE updates a fresh, unsigned build artifact, never an installed/running
// game. Staging beside it ensures failure leaves the original untouched.
func (icon Icon) ApplyEXE(path string) error {
	in, err := os.Open(path)
	if err != nil {
		return err
	}
	defer in.Close()
	info, err := in.Stat()
	if err != nil {
		return err
	}
	out, err := os.CreateTemp(filepath.Dir(path), ".app-icon-*.exe")
	if err != nil {
		return err
	}
	defer os.Remove(out.Name())
	defer out.Close()
	if err = icon.WriteEXE(out, in); err != nil {
		return fmt.Errorf("embed %s icon: %w", icon.Name(), err)
	}
	if err = out.Chmod(info.Mode().Perm()); err != nil {
		return err
	}
	if err = out.Close(); err != nil {
		return err
	}
	if err = in.Close(); err != nil {
		return err
	}
	return os.Rename(out.Name(), path)
}

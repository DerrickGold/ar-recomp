package main

import (
	"bytes"
	"debug/pe"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
	"github.com/DerrickGold/ar-recomp/installer/internal/appicons"
	"github.com/tc-hib/winres"
	"github.com/tc-hib/winres/version"
)

// shellManifest is deliberately minimal: run as the invoking user and declare
// Windows 10+ (Go itself requires Windows 10). Wails sets DPI awareness at
// startup and aborts if that call fails, so DPI stays out of the manifest; the
// native startup window is also unchanged without Common Controls 6.
const shellManifest = `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
</assembly>
`

// releaseNumbers maps compact tags (v0900 = 0.9.0.0) and dotted tags (v1.0)
// onto the four numeric version fields. Other strings, such as an untagged
// development build, keep 0.0.0.0; the version strings still carry the
// complete git describe text.
func releaseNumbers(release string) [4]uint16 {
	var numbers [4]uint16
	digits, ok := strings.CutPrefix(release, "v")
	if !ok {
		return numbers
	}
	core, _, _ := strings.Cut(digits, "-")
	core, _, _ = strings.Cut(core, "+")
	if strings.Contains(core, ".") {
		parts := strings.Split(core, ".")
		if len(parts) > len(numbers) {
			return numbers
		}
		for i, part := range parts {
			number, err := strconv.ParseUint(part, 10, 16)
			if err != nil {
				return [4]uint16{}
			}
			numbers[i] = uint16(number)
		}
		return numbers
	}
	if end := strings.IndexFunc(digits, func(r rune) bool { return r < '0' || r > '9' }); end >= 0 {
		digits = digits[:end]
	}
	if len(digits) < 2 || len(digits) > len(numbers) {
		return numbers
	}
	for i, digit := range digits {
		numbers[i] = uint16(digit - '0')
	}
	return numbers
}

func builderVersionInfo(release, description, internalName string) (version.Info, error) {
	numbers := releaseNumbers(release)
	vi := version.Info{FileVersion: numbers, ProductVersion: numbers}
	for key, value := range map[string]string{
		version.CompanyName:      "ActRaiser Recomp",
		version.FileDescription:  description,
		version.FileVersion:      release,
		version.InternalName:     internalName,
		version.LegalCopyright:   "Copyright (c) 2026 Derrick Gold", // As in LICENSE.
		version.OriginalFilename: internalName + ".exe",
		version.ProductName:      "ActRaiser Recomp Builder",
		version.ProductVersion:   release,
	} {
		if err := vi.Set(version.LangDefault, key, value); err != nil {
			return vi, err
		}
	}
	return vi, nil
}

// writeShellResources adds version information, the manifest and the
// Explorer/window icon before the self-contained ZIP is appended. The input
// shell stays untouched; sign only the finished package.
func writeShellResources(source, destination, release string) error {
	if bundle, err := winbundle.Open(source); err == nil {
		bundle.Close()
		return fmt.Errorf("resource input is already packaged")
	} else if err != winbundle.ErrNoBundle {
		return err
	}
	return writeBuilderResources(source, destination, release, pe.IMAGE_SUBSYSTEM_WINDOWS_GUI)
}

func writeBuilderResources(source, destination, release string, subsystem uint16) error {
	if release == "" {
		return errors.New("the Builder release version is required")
	}
	in, err := os.Open(source)
	if err != nil {
		return err
	}
	defer in.Close()
	info, err := in.Stat()
	if err != nil {
		return err
	}
	_, actualSubsystem, end, err := winbundle.PEInfo(in, info.Size())
	if err != nil {
		return err
	}
	if actualSubsystem != subsystem || end != info.Size() {
		return fmt.Errorf("expected an unsigned Builder executable with PE subsystem %d", subsystem)
	}
	resources, err := winres.LoadFromEXE(in)
	if err != nil && !errors.Is(err, winres.ErrNoResources) {
		return err
	}
	description, name := "ActRaiser Recomp Builder", host.Name
	if subsystem == pe.IMAGE_SUBSYSTEM_WINDOWS_CUI {
		description, name = "ActRaiser Recomp Builder CLI", "actraiser-builder"
	}
	vi, err := builderVersionInfo(release, description, name)
	if err != nil {
		return err
	}
	resources.SetVersionInfo(vi)
	if subsystem == pe.IMAGE_SUBSYSTEM_WINDOWS_GUI {
		if err = resources.Set(winres.RT_MANIFEST, winres.ID(1), winres.LCIDDefault, []byte(shellManifest)); err != nil {
			return err
		}
	}
	var described bytes.Buffer
	if err = resources.WriteToEXE(&described, in); err != nil {
		return err
	}
	// A repeated build can replace the previous helper only after the new PE is complete.
	out, err := os.CreateTemp(filepath.Dir(destination), ".builder-resources-*")
	if err != nil {
		return err
	}
	defer os.Remove(out.Name())
	defer out.Close()
	// appicons owns the icon IDs Wails expects and keeps the resources above.
	if err = appicons.Builder.WriteEXE(out, bytes.NewReader(described.Bytes())); err != nil {
		return err
	}
	if err = out.Chmod(0755); err != nil {
		return err
	}
	if err = out.Close(); err != nil {
		return err
	}
	return os.Rename(out.Name(), destination)
}

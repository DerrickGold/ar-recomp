package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
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

// releaseNumbers maps this project's tags (v049 = 0.4.9, v0801 = 0.8.0.1) onto
// the four numeric version fields. Other strings, such as an untagged
// development build, keep 0.0.0.0; the version strings still carry the
// complete git describe text.
func releaseNumbers(release string) [4]uint16 {
	var numbers [4]uint16
	digits, ok := strings.CutPrefix(release, "v")
	if !ok {
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

func shellVersionInfo(release string) (version.Info, error) {
	numbers := releaseNumbers(release)
	vi := version.Info{FileVersion: numbers, ProductVersion: numbers}
	for key, value := range map[string]string{
		version.CompanyName:      "ActRaiser Recomp",
		version.FileDescription:  "ActRaiser Recomp Builder",
		version.FileVersion:      release,
		version.InternalName:     host.Name,
		version.LegalCopyright:   "Copyright (c) 2026 Derrick Gold", // As in LICENSE.
		version.OriginalFilename: host.Name + ".exe",
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
	if release == "" {
		return errors.New("the Builder release version is required")
	}
	if bundle, err := winbundle.Open(source); err == nil {
		bundle.Close()
		return fmt.Errorf("resource input is already packaged")
	} else if err != winbundle.ErrNoBundle {
		return err
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
	_, _, end, err := winbundle.PEInfo(in, info.Size())
	if err != nil {
		return err
	}
	if end != info.Size() {
		return fmt.Errorf("resource input must be an unsigned, unpackaged shell")
	}
	resources, err := winres.LoadFromEXE(in)
	if err != nil && !errors.Is(err, winres.ErrNoResources) {
		return err
	}
	vi, err := shellVersionInfo(release)
	if err != nil {
		return err
	}
	resources.SetVersionInfo(vi)
	if err = resources.Set(winres.RT_MANIFEST, winres.ID(1), winres.LCIDDefault, []byte(shellManifest)); err != nil {
		return err
	}
	var described bytes.Buffer
	if err = resources.WriteToEXE(&described, in); err != nil {
		return err
	}
	out, err := os.OpenFile(destination, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0755)
	if err != nil {
		return err
	}
	defer out.Close()
	// appicons owns the icon IDs Wails expects and keeps the resources above.
	if err = appicons.Builder.WriteEXE(out, bytes.NewReader(described.Bytes())); err != nil {
		return err
	}
	return out.Close()
}

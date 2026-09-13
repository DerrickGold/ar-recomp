package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"golang.org/x/sys/windows"
)

// The bootstrap accepts only absolute local-drive paths. Give native ACL APIs
// the extended-length form explicitly; unlike Go's os functions, these wrappers
// do not add it automatically. This needs no system-wide long-path setting.
func localSecurityPath(directory string) (string, error) {
	directory = filepath.Clean(directory)
	volume := filepath.VolumeName(directory)
	if !filepath.IsAbs(directory) || len(volume) != 2 || volume[1] != ':' {
		return "", errors.New("runtime permissions require an absolute local-drive path")
	}
	return `\\?\` + directory, nil
}

// Microsoft requires package/restricted-package read+execute access for
// unpackaged Fixed WebView2 on Windows 10. Grant only this new staging tree,
// preserving its inherited current-user/SYSTEM permissions. SetNamedSecurityInfo
// propagates inheritable ACEs to existing children; new children inherit too.
// Do not run icacls /T: its recursive traversal fails on deeply nested runtime
// files even when Go has successfully extracted and verified those same files.
func grantWebviewReadAccess(directory string) error {
	path, err := localSecurityPath(directory)
	if err != nil {
		return err
	}
	info, err := os.Lstat(directory)
	if err != nil {
		return fmt.Errorf("inspect new WebView2 runtime: %w", err)
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("WebView2 permissions require a real runtime directory")
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read WebView2 runtime permissions: %w", err)
	}
	if sd == nil {
		return errors.New("WebView2 runtime has no security descriptor; use a local ACL-capable filesystem such as NTFS")
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("read WebView2 runtime access list: %w", err)
	}
	if dacl == nil {
		return errors.New("WebView2 runtime is missing its private access list")
	}
	var entries []windows.EXPLICIT_ACCESS
	var sids []*windows.SID
	for _, text := range []string{"S-1-15-2-2", "S-1-15-2-1"} {
		sid, err := windows.StringToSid(text)
		if err != nil {
			return err
		}
		sids = append(sids, sid)
		entries = append(entries, windows.EXPLICIT_ACCESS{
			AccessPermissions: windows.FILE_GENERIC_READ | windows.FILE_GENERIC_EXECUTE,
			AccessMode:        windows.GRANT_ACCESS,
			Inheritance:       windows.SUB_CONTAINERS_AND_OBJECTS_INHERIT,
			Trustee: windows.TRUSTEE{
				TrusteeForm:  windows.TRUSTEE_IS_SID,
				TrusteeType:  windows.TRUSTEE_IS_WELL_KNOWN_GROUP,
				TrusteeValue: windows.TrusteeValueFromSID(sid),
			},
		})
	}
	merged, err := windows.ACLFromEntries(entries, dacl)
	// TRUSTEE stores SIDs as uintptrs, which are not GC roots.
	runtime.KeepAlive(sids)
	runtime.KeepAlive(sd)
	if err != nil {
		return fmt.Errorf("prepare WebView2 sandbox access list: %w", err)
	}
	if err := windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION, nil, nil, merged, nil); err != nil {
		return fmt.Errorf("grant WebView2 sandbox read permissions: %w", err)
	}
	return nil
}

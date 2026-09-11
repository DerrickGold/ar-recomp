package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"unsafe"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
	"golang.org/x/sys/windows"
)

func prepareEmbedded(payload, workspace, browser *string) error {
	if *payload != "" {
		return nil
	} // Explicit developer mode retains the existing path.
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	a, err := winbundle.Open(executable)
	if errors.Is(err, winbundle.ErrNoBundle) {
		return errors.New("This development shell has no embedded payload. Use --payload or build the packaged Windows executable.")
	}
	if err != nil {
		return err
	}
	defer a.Close()
	if a.Manifest.Arch != runtime.GOARCH {
		return errors.New("embedded Windows architecture does not match this shell")
	}
	work, err := host.DefaultWorkspace(executable, *workspace)
	if err != nil {
		return err
	}
	volume := filepath.VolumeName(work)
	if volume == "" || strings.HasPrefix(volume, `\\`) {
		return errors.New("the Windows Builder workspace must be on a local drive, not a UNC/network location")
	}
	drive, err := windows.UTF16PtrFromString(volume + `\`)
	if err != nil {
		return err
	}
	if windows.GetDriveType(drive) == windows.DRIVE_REMOTE {
		return errors.New("Fixed WebView2 cannot run from a mapped network drive; choose a local workspace")
	}
	cache := host.AuxiliaryDirectory(work, "runtime")
	if err = host.ValidateWorkspace(cache, executable); err != nil {
		return err
	}
	if err = ensureRuntimeCache(cache); err != nil {
		return err
	}
	release, err := host.Lock(cache)
	if err != nil {
		return err
	}
	defer release()
	dir := filepath.Join(cache, a.ID)
	if _, err = os.Lstat(dir); os.IsNotExist(err) {
		if err = a.Extract(dir, grantWebviewReadAccess); err != nil {
			return fmt.Errorf("prepare bundled tools/WebView2: %w", err)
		}
	} else if err != nil {
		return err
	}
	if err = a.VerifyDirectory(dir); err != nil {
		return fmt.Errorf("runtime cache is incomplete or modified; choose a new workspace (existing files were not changed): %w", err)
	}
	*payload = filepath.Join(dir, "payload")
	*workspace = work
	*browser = filepath.Join(dir, "webview")
	// WebView2 environment variables can supersede the API's explicit paths.
	// Do not inherit debugging/runtime-selection overrides into this package.
	for _, entry := range os.Environ() {
		key, _, _ := strings.Cut(entry, "=")
		if strings.HasPrefix(strings.ToUpper(key), "WEBVIEW2_") {
			if err = os.Unsetenv(key); err != nil {
				return err
			}
		}
	}
	if err = os.Setenv("WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", *browser); err != nil {
		return err
	}
	return os.Setenv("WEBVIEW2_USER_DATA_FOLDER", host.AuxiliaryDirectory(work, "webview"))
}

const cacheMarker = "ActRaiserRecompBuilder runtime cache v1\n"

func ensureRuntimeCache(directory string) error {
	info, err := os.Lstat(directory)
	if err == nil {
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return errors.New("runtime cache must be a real directory")
		}
		marker, err := os.ReadFile(filepath.Join(directory, ".builder-runtime-cache"))
		if err != nil || string(marker) != cacheMarker {
			return errors.New("runtime cache directory already exists but is not managed by this Builder; choose a new workspace")
		}
		return nil
	}
	if !os.IsNotExist(err) {
		return err
	}
	if err = os.MkdirAll(filepath.Dir(directory), 0700); err != nil {
		return err
	}
	if err = os.Mkdir(directory, 0700); err != nil {
		return err
	}
	// Windows ignores Unix mode bits. Protect our newly created executable
	// cache explicitly; never change ACLs on an existing unmanaged directory.
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return err
	}
	sd, err := windows.SecurityDescriptorFromString("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + user.User.Sid.String() + ")")
	if err != nil {
		return err
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return err
	}
	if err = windows.SetNamedSecurityInfo(directory, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION, nil, nil, dacl, nil); err != nil {
		return fmt.Errorf("runtime cache needs a local filesystem supporting Windows ACLs: %w", err)
	}
	return os.WriteFile(filepath.Join(directory, ".builder-runtime-cache"), []byte(cacheMarker), 0600)
}

// Microsoft requires package/ restricted-package read+execute access for
// unpackaged Fixed WebView2 on Windows 10. Grant only this new runtime tree,
// not the build workspace, ROM, source SDK or arbitrary parent directories.
func grantWebviewReadAccess(directory string) error {
	system, err := windows.GetSystemDirectory()
	if err != nil {
		return err
	}
	cmd := exec.Command(filepath.Join(system, "icacls.exe"), directory, "/grant", "*S-1-15-2-2:(OI)(CI)(RX)", "*S-1-15-2-1:(OI)(CI)(RX)", "/T", "/Q")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: windows.CREATE_NO_WINDOW}
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("grant WebView2 sandbox read permissions: %w: %s", err, out)
	}
	return nil
}

func showStartupError(err error) {
	fmt.Fprintln(os.Stderr, err)
	message, _ := windows.UTF16PtrFromString(err.Error())
	title, _ := windows.UTF16PtrFromString("ActRaiser Recomp Builder could not start")
	windows.NewLazySystemDLL("user32.dll").NewProc("MessageBoxW").Call(0, uintptr(unsafe.Pointer(message)), uintptr(unsafe.Pointer(title)), 0x10)
}

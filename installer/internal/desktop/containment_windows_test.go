package desktop

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Optional physical-volume acceptance test; the lexical C:/D: checks always
// run on Windows. Set this to a writable folder on another disk, not a device
// root. Only this test's newly created child directory is removed.
func TestWindowsOtherDriveOutputAndImport(t *testing.T) {
	other := os.Getenv("AR_WINDOWS_TEST_OTHER_DRIVE")
	if other == "" {
		t.Skip("set AR_WINDOWS_TEST_OTHER_DRIVE for a real cross-volume output/import test")
	}
	source := t.TempDir()
	other, err := filepath.Abs(other)
	if err != nil {
		t.Fatal(err)
	}
	if strings.EqualFold(filepath.VolumeName(source), filepath.VolumeName(other)) {
		t.Fatal("AR_WINDOWS_TEST_OTHER_DRIVE must be on another volume than TEMP")
	}
	destination, err := os.MkdirTemp(other, "actraiser-cross-drive-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(destination) })
	nativePackageFixture(t, source)
	if err := PreparePortableData(source, destination); err != nil {
		t.Fatal(err)
	}
	put(t, filepath.Join(source, "saves", "slot1.srm"), "synthetic save")
	plan, err := PreviewInstallImport(context.Background(), source, destination)
	if err != nil || plan.Copy < 1 {
		t.Fatalf("cross-drive import = %+v, %v", plan, err)
	}
}

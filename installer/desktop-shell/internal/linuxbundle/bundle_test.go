package linuxbundle

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRelocationIsNarrowAndPreservesELFOffsets(t *testing.T) {
	path := "/usr/lib/aarch64-linux-gnu/webkit2gtk-4.1"
	input := []byte("header\x00" + path + "\x00" + path + "/WebKitWebProcess\x00/usr/share/unrelated\x00" + path + "-different\x00embedded:" + path + "\x00")
	out, counts, err := Relocate(input, []string{path})
	if err != nil {
		t.Fatal(err)
	}
	if len(input) != len(out) || counts[path] != 2 {
		t.Fatalf("changed offsets or wrong replacement count: %v", counts)
	}
	if !bytes.Contains(out, []byte("/usr/share/unrelated")) || !bytes.Contains(out, []byte(path+"-different")) || !bytes.Contains(out, []byte("embedded:"+path)) {
		t.Fatal("rewrote unrelated data")
	}
	if !bytes.Contains(out, []byte("././/lib/aarch64-linux-gnu/webkit2gtk-4.1/WebKitWebProcess")) {
		t.Fatal("missing relative helper path")
	}
	if !bytes.Contains(input, []byte("header\x00"+path+"\x00")) {
		t.Fatal("mutated source")
	}
	if _, _, err := Relocate(input, []string{"/home/path"}); err == nil {
		t.Fatal("accepted arbitrary prefix")
	}
}

func TestDependencyClosureKeepsWebviewAndExcludesSystemABI(t *testing.T) {
	output := "linux-vdso.so.1 (0xffff)\nlibc.so.6 => /lib/libc.so.6 (0xffff)\nlibEGL.so.1 => /lib/libEGL.so.1 (0xffff)\nlibwebkit2gtk-4.1.so.0 => /lib/libwebkit2gtk-4.1.so.0 (0xffff)\nlibstdc++.so.6 => /lib/libstdc++.so.6 (0xffff)\n/lib/ld-linux-aarch64.so.1 (0xffff)\n"
	paths, err := Dependencies(output)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(paths, ",") != "/lib/libwebkit2gtk-4.1.so.0" {
		t.Fatal(paths)
	}
	if paths, err := Dependencies("libwayland-client.so.0 => /lib/libwayland-client.so.0 (0xffff)"); err != nil || len(paths) != 0 {
		t.Fatal("bundled Wayland client can break host Mesa", paths, err)
	}
	for _, system := range []string{"libstdc++.so.6", "libgcc_s.so.1", "libwayland-client.so.0"} {
		if !HostLibrary(system) {
			t.Fatalf("must not shadow graphics-driver runtime: %s", system)
		}
	}
	if _, err := Dependencies("libmissing.so.1 => not found"); err == nil {
		t.Fatal("accepted missing library")
	}
	if _, err := Dependencies("libbad.so.1 => relative/file (0xff)"); err == nil {
		t.Fatal("accepted relative dependency")
	}
	for _, keep := range []string{"libgtk-3.so.0", "libwayland-cursor.so.0", "libwayland-egl.so.1", "libjavascriptcoregtk-4.1.so.0", "libglib-2.0.so.0"} {
		if HostLibrary(keep) {
			t.Fatalf("unexpected external runtime: %s", keep)
		}
	}
}

func TestFinishedGUITreeCannotShadowHostLibraries(t *testing.T) {
	for _, leaf := range []string{"libwayland-client.so.0", "private/libwayland-client.so.0", "libstdc++.so.6", "libEGL.so.1", "libgtk-3.so.0"} {
		t.Run(leaf, func(t *testing.T) {
			root := t.TempDir()
			path := filepath.Join(root, "usr/lib", leaf)
			if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(path, nil, 0644); err != nil {
				t.Fatal(err)
			}
			err := validateHostLibraries(root)
			if (err != nil) != (leaf != "libgtk-3.so.0") {
				t.Fatal("unexpected host-library audit result", err)
			}
		})
	}
}

package linuxsdk

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// BuildShell runs host Go/pkg-config/Zig with explicit Linux target paths.
// No command is looked up in the target SDK's bin directories.
func (s *SDK) BuildShell(source, output, zig string, smoke bool) error {
	var err error
	source, err = filepath.Abs(source)
	if err != nil {
		return err
	}
	output, err = filepath.Abs(output)
	if err != nil {
		return err
	}
	zig, err = filepath.Abs(zig)
	if err != nil {
		return err
	}
	if _, err = os.Lstat(output); !os.IsNotExist(err) {
		return fmt.Errorf("shell output exists: %s", output)
	}
	if _, err = exec.LookPath("pkg-config"); err != nil {
		return err
	}
	lib := s.Path("/usr/lib/" + s.Triple())
	compiler := func(lang string) string {
		// pkg-config already produces absolute SDK paths. Zig's --sysroot
		// would prepend the SDK AGAIN to absolute -L paths. Use its target
		// libc plus explicit target include/library paths instead.
		return strconv.Quote(zig) + " " + lang + " -target " + s.Triple() + "." + GLIBCBaseline
	}
	env := map[string]string{
		"GOOS": "linux", "GOARCH": s.Lock.Arch, "CGO_ENABLED": "1",
		"CC": compiler("cc"), "CXX": compiler("c++"),
		"PKG_CONFIG_SYSROOT_DIR": s.Root, "PKG_CONFIG_LIBDIR": lib + "/pkgconfig:" + s.Path("/usr/share/pkgconfig"), "PKG_CONFIG_PATH": "",
		"PKG_CONFIG_ALLOW_SYSTEM_LIBS": "1", "PKG_CONFIG_ALLOW_SYSTEM_CFLAGS": "1",
		"CGO_CFLAGS": "-O2 -g", "CGO_CPPFLAGS": "", "CGO_CXXFLAGS": "-O2 -g",
		"CGO_LDFLAGS": strconv.Quote("-L" + lib),
	}
	tags := "production,webkit2_41"
	if smoke {
		tags += ",smoketest"
	}
	cmd := exec.Command("go", "-C", source, "build", "-tags", tags, "-trimpath", "-ldflags", "-s -w", "-o", output, ".")
	for _, v := range os.Environ() {
		key, _, _ := strings.Cut(v, "=")
		if _, replace := env[key]; !replace {
			cmd.Env = append(cmd.Env, v)
		}
	}
	for key, value := range env {
		cmd.Env = append(cmd.Env, key+"="+value)
	}
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err = cmd.Run(); err != nil {
		return err
	}
	arch, err := ELFArchitecture(output)
	if err != nil {
		return err
	}
	if arch != s.Lock.Arch {
		return fmt.Errorf("cross-built shell architecture mismatch")
	}
	return nil
}

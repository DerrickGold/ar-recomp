// payload creates a manifest for a fresh CMake install tree, including when
// cross-staging a payload to test in a native Linux VM. Never use a live install.
package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
)

func main() {
	root := flag.String("root", "", "fresh ROM-free CMake install tree")
	goos := flag.String("os", runtime.GOOS, "payload target OS")
	arch := flag.String("arch", runtime.GOARCH, "payload target architecture")
	flag.Parse()
	if *root == "" {
		fmt.Fprintln(os.Stderr, "--root is required")
		os.Exit(1)
	}
	if err := host.WriteManifest(*root, *goos, *arch); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

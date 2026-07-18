// Package project provides cross-platform orchestration for a per-game
// snesrecomp project. It deliberately avoids shell utilities so the same code
// can be shipped as a native tool on macOS, Linux, and Windows.
package project

import (
	"fmt"
	"path/filepath"
)

type Paths struct {
	Root         string
	ROM          string
	ConfigDir    string
	GeneratedDir string
	FuncsHeader  string
	Metadata     string
	RTSReport    string
	RTSPrevious  string
	ToolchainDir string
	BuildDir     string
}

func DefaultPaths(root string) Paths {
	return Paths{
		Root: root, ROM: "game.sfc", ConfigDir: "recomp", GeneratedDir: "src/gen",
		FuncsHeader: "recomp/funcs.h", Metadata: "saves/gen_meta.json",
		RTSReport: "saves/rts_webs.txt", RTSPrevious: "saves/rts_webs.prev.txt",
		ToolchainDir: "snesrecomp-go", BuildDir: "build",
	}
}

func (paths Paths) Resolve() (Paths, error) {
	root := paths.Root
	if root == "" {
		root = "."
	}
	absoluteRoot, err := filepath.Abs(root)
	if err != nil {
		return Paths{}, fmt.Errorf("resolve project root: %w", err)
	}
	absoluteRoot = filepath.Clean(absoluteRoot)
	paths.Root = absoluteRoot
	paths.ROM = resolveUnder(absoluteRoot, paths.ROM)
	paths.ConfigDir = resolveUnder(absoluteRoot, paths.ConfigDir)
	paths.GeneratedDir = resolveUnder(absoluteRoot, paths.GeneratedDir)
	paths.FuncsHeader = resolveUnder(absoluteRoot, paths.FuncsHeader)
	paths.Metadata = resolveUnder(absoluteRoot, paths.Metadata)
	paths.RTSReport = resolveUnder(absoluteRoot, paths.RTSReport)
	paths.RTSPrevious = resolveUnder(absoluteRoot, paths.RTSPrevious)
	paths.ToolchainDir = resolveUnder(absoluteRoot, paths.ToolchainDir)
	paths.BuildDir = resolveUnder(absoluteRoot, paths.BuildDir)
	return paths, nil
}

func resolveUnder(root, path string) string {
	if path == "" {
		return ""
	}
	if filepath.IsAbs(path) {
		return filepath.Clean(path)
	}
	return filepath.Join(root, filepath.FromSlash(path))
}

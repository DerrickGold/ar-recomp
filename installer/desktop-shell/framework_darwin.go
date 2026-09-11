package main

// Wails' CLI normally adds this framework. Keep plain `go build` equivalent
// for our CMake packaging entry point (Wails v2.15 uses UTType in file dialogs).

// #cgo LDFLAGS: -framework UniformTypeIdentifiers
import "C"

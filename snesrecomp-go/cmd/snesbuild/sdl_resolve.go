package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"runtime"

	"github.com/DerrickGold/snesrecomp-go/internal/toolchain"
)

func runSDLResolve(args []string) error {
	flags := flag.NewFlagSet("sdl resolve", flag.ContinueOnError)
	goos := flags.String("goos", runtime.GOOS, "installer target OS")
	goarch := flags.String("goarch", runtime.GOARCH, "installer target architecture")
	sdl := flags.String("sdl-version", "3", "latest stable major 3, or exact SDL3 release")
	ttf := flags.String("ttf-version", "3", "latest stable major 3, or exact SDL3_ttf release")
	locked := flags.String("lock", "", "reuse a previously resolved SDK lock without network access")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("sdl resolve accepts flags only")
	}
	var lock toolchain.SDLSDKLock
	if *locked != "" {
		data, err := os.ReadFile(*locked)
		if err != nil {
			return err
		}
		if err := json.Unmarshal(data, &lock); err != nil {
			return err
		}
		if err := toolchain.ValidateSDLSDKLock(lock, *goos, *goarch, *sdl, *ttf); err != nil {
			return err
		}
	} else {
		var err error
		lock, err = toolchain.ResolveSDLSDK(*goos, *goarch, *sdl, *ttf)
		if err != nil {
			return err
		}
	}
	data, err := json.MarshalIndent(lock, "", "  ")
	if err != nil {
		return err
	}
	fmt.Println(string(data))
	return nil
}

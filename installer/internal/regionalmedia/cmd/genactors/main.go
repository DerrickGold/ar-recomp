// genactors emits reviewed native source identities, not executable/pixel data.
package main

import (
	"bytes"
	"fmt"
	"os"

	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

func main() {
	if len(os.Args) != 2 {
		panic("genactors needs an output path")
	}
	var b bytes.Buffer
	fmt.Fprintln(&b, "/* Generated from installer/internal/regionalmedia/actor-native.json. */")
	fmt.Fprintln(&b, "static const ArRegionalActorArtBinding kActorBindings[] = {")
	for _, r := range regionalmedia.NativeActorBindings() {
		fmt.Fprintf(&b, "  {0x%04x,%d,%d,0x%06x,%d,0x%04x,%d},\n", r.Scene, r.Kind, r.Slot, r.Source, r.Size, r.Table, r.Pictures)
	}
	fmt.Fprintln(&b, "};")
	if err := os.WriteFile(os.Args[1], b.Bytes(), 0644); err != nil {
		panic(err)
	}
}

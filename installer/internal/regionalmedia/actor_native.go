package regionalmedia

import (
	_ "embed"
	"encoding/json"
)

// Reviewed US declaration identities and bounds, not ROM program/pixel bytes.
//
//go:embed actor-native.json
var actorNativeJSON []byte

// NativeActorBinding joins an actual US asset declaration to a donor key.
// Size is the native decoded size (not the converted picture-table size).
type NativeActorBinding struct {
	Scene    uint16 `json:"scene"`
	Kind     byte   `json:"kind"`
	Slot     byte   `json:"slot"`
	Source   uint32 `json:"source"`
	Size     uint16 `json:"size"`
	Table    uint16 `json:"table,omitempty"`
	Pictures uint16 `json:"pictures,omitempty"`
}

func NativeActorBindings() []NativeActorBinding {
	var result []NativeActorBinding
	if err := json.Unmarshal(actorNativeJSON, &result); err != nil {
		panic(err)
	}
	return result
}

//go:generate go run ./cmd/genactors ../../../src/regional/regional_actor_art_native.inc

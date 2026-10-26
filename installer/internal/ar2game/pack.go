// Package ar2game imports the ROM data used by the optional native AR2 module.
// Unlike the Master-only pack, this pack retains the complete normalized ROM:
// native scene loaders, scripts and the SPC driver still address its data.
package ar2game

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2rom"
	"github.com/DerrickGold/ar-recomp/installer/internal/importstore"
)

const (
	Filename     = "ar2-world.argame"
	HeaderBytes  = 48
	MaximumBytes = HeaderBytes + ar2rom.Size
)

var storage = importstore.Pack{Filename: Filename, MaximumBytes: MaximumBytes, Validate: Validate}

func Extract(rom []byte) ([]byte, error) {
	rom, err := ar2rom.Normalize(rom)
	if err != nil {
		return nil, err
	}
	data := make([]byte, HeaderBytes, MaximumBytes)
	copy(data, "AR2WORLD")
	binary.LittleEndian.PutUint32(data[8:], 1)
	binary.LittleEndian.PutUint32(data[12:], ar2rom.Size)
	copy(data[16:], ar2rom.Digest[:])
	return append(data, rom...), nil
}

func Validate(data []byte) error {
	if len(data) != MaximumBytes || string(data[:8]) != "AR2WORLD" ||
		binary.LittleEndian.Uint32(data[8:]) != 1 ||
		binary.LittleEndian.Uint32(data[12:]) != ar2rom.Size ||
		!bytes.Equal(data[16:HeaderBytes], ar2rom.Digest[:]) {
		return fmt.Errorf("invalid AR2 world pack header")
	}
	_, err := ar2rom.Normalize(data[HeaderBytes:])
	return err
}

func ReadROM(root string) ([]byte, error) {
	data, err := storage.Read(root)
	if err != nil {
		return nil, err
	}
	return data[HeaderBytes:], nil
}

func Status(root string) string                      { return storage.Status(root) }
func Install(root string, data []byte) (bool, error) { return storage.Install(root, data) }

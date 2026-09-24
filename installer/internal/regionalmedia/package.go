package regionalmedia

import (
	"bytes"
	"crypto/sha256"
	_ "embed"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"

	"github.com/DerrickGold/ar-recomp/installer/internal/gamerom"
)

//go:generate go run ./cmd/gencatalog ../../../src/regional/media/regional_media_catalog.inc
//go:embed catalog.json
var catalogJSON []byte

const Version = 1
const MaximumBytes = 2 << 20
const headerBytes = 48
const entryBytes = 12

var magic = [8]byte{'A', 'R', 'M', 'E', 'D', 'I', 'A', 0}

type Definition struct {
	ID     uint32            `json:"id"`
	Key    string            `json:"key"`
	Size   uint32            `json:"size"`
	Hashes map[string]string `json:"hashes"`
}

// Definitions returns detached schema/identity facts, never ROM-derived bytes.
func Definitions() []Definition {
	var definitions []Definition
	if err := json.Unmarshal(catalogJSON, &definitions); err != nil {
		panic(err)
	}
	return definitions
}

// Pack produces a canonical, data-only package. Even caller-built Extractions
// must match the reviewed catalog; unsupported or incomplete bundles fail.
func Pack(extraction *Extraction) ([]byte, error) {
	if extraction == nil {
		return nil, fmt.Errorf("missing regional media extraction")
	}
	releases := gamerom.Releases()
	index := -1
	for i, r := range releases {
		if r == extraction.Release {
			index = i
		}
	}
	if index < 0 {
		return nil, fmt.Errorf("unknown donor identity")
	}
	definitions := Definitions()
	var expected []Definition
	for _, d := range definitions {
		if d.Hashes[extraction.Release.ID] != "" {
			expected = append(expected, d)
		}
	}
	if len(expected) != len(extraction.Resources) {
		return nil, fmt.Errorf("incomplete regional media extraction")
	}
	size := headerBytes + entryBytes*len(expected)
	for i, d := range expected {
		r := extraction.Resources[i]
		if r.ID != d.ID || len(r.Bytes) != int(d.Size) || fmt.Sprintf("%x", sha256.Sum256(r.Bytes)) != d.Hashes[extraction.Release.ID] {
			return nil, fmt.Errorf("regional resource %s does not match its reviewed donor data", d.Key)
		}
		size += len(r.Bytes)
	}
	if size > MaximumBytes || len(expected) > 16 {
		return nil, fmt.Errorf("regional media capacity exceeded")
	}
	data := make([]byte, size)
	copy(data, magic[:])
	binary.LittleEndian.PutUint16(data[8:], Version)
	data[10], data[11] = byte(index+1), byte(len(expected))
	binary.LittleEndian.PutUint32(data[12:], uint32(size))
	digest, _ := hex.DecodeString(extraction.Release.SHA256)
	copy(data[16:48], digest)
	cursor := headerBytes + entryBytes*len(expected)
	for i, r := range extraction.Resources {
		header := data[headerBytes+i*entryBytes:]
		binary.LittleEndian.PutUint32(header, r.ID)
		binary.LittleEndian.PutUint32(header[4:], uint32(cursor))
		binary.LittleEndian.PutUint32(header[8:], uint32(len(r.Bytes)))
		copy(data[cursor:], r.Bytes)
		cursor += len(r.Bytes)
	}
	return data, nil
}

// Unpack accepts only canonical reviewed bundles, with no paths, executable
// payloads, overlaps, gaps or extra data. Returned resources own their bytes.
func Unpack(data []byte) (*Extraction, error) {
	if len(data) < headerBytes || len(data) > MaximumBytes || !bytes.Equal(data[:8], magic[:]) ||
		binary.LittleEndian.Uint16(data[8:]) != Version || int(binary.LittleEndian.Uint32(data[12:])) != len(data) {
		return nil, fmt.Errorf("invalid regional media header")
	}
	releases := gamerom.Releases()
	release := int(data[10]) - 1
	count := int(data[11])
	if release < 0 || release >= len(releases) || count == 0 || count > 16 || len(data) < headerBytes+count*entryBytes {
		return nil, fmt.Errorf("invalid regional media directory")
	}
	out := &Extraction{Release: releases[release]}
	cursor := headerBytes + count*entryBytes
	for i := 0; i < count; i++ {
		h := data[headerBytes+i*entryBytes:]
		id, offset, size := binary.LittleEndian.Uint32(h), uint64(binary.LittleEndian.Uint32(h[4:])), uint64(binary.LittleEndian.Uint32(h[8:]))
		if offset != uint64(cursor) || size > uint64(len(data)-cursor) {
			return nil, fmt.Errorf("invalid regional media span")
		}
		out.Resources = append(out.Resources, Resource{id, append([]byte(nil), data[cursor:cursor+int(size)]...)})
		cursor += int(size)
	}
	canonical, err := Pack(out)
	if err != nil {
		return nil, err
	}
	if !bytes.Equal(canonical, data) {
		return nil, fmt.Errorf("noncanonical regional media package")
	}
	return out, nil
}

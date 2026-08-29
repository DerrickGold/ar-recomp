package spcaudio

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// wavHeaderBytes is the size of the canonical header writeWAV emits. Every
// cache entry is written by writeWAV, so the layout is fixed and can be parsed
// positionally rather than by walking arbitrary RIFF chunks.
const wavHeaderBytes = 44

// validWAVFile reports whether a cache entry is a COMPLETE preview of exactly
// the expected shape. The previous check accepted anything beginning "RIFF"
// followed by "WAVE", which meant a truncated file, a leftover from a different
// preview duration, or a WAV written by an earlier renderer was reused forever
// -- the entry can only ever be replaced by a cache-key change, and a stale
// entry sounds like a corrupt one. Anything that fails here is re-rendered.
func validWAVFile(path string, expectedFrames int) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	dataBytes := int64(expectedFrames) * 4
	if expectedFrames <= 0 || info.Size() != wavHeaderBytes+dataBytes {
		return false
	}
	header := make([]byte, wavHeaderBytes)
	if _, err := io.ReadFull(file, header); err != nil {
		return false
	}
	if string(header[0:4]) != "RIFF" || string(header[8:12]) != "WAVE" ||
		string(header[12:16]) != "fmt " || string(header[36:40]) != "data" {
		return false
	}
	u32 := func(at int) uint32 { return binary.LittleEndian.Uint32(header[at : at+4]) }
	u16 := func(at int) uint16 { return binary.LittleEndian.Uint16(header[at : at+2]) }
	return u32(4) == uint32(36+dataBytes) && u32(16) == 16 && u16(20) == 1 &&
		u16(22) == 2 && u32(24) == SampleRate && u16(34) == 16 &&
		u32(40) == uint32(dataBytes)
}

func writeWAV(writer io.Writer, samples []int16) error {
	if len(samples)&1 != 0 {
		return fmt.Errorf("stereo PCM has an odd sample count")
	}
	dataBytes := uint64(len(samples)) * 2
	if dataBytes > uint64(^uint32(0))-36 {
		return fmt.Errorf("PCM is too large for a RIFF/WAV file")
	}
	header := struct {
		RIFF       [4]byte
		Size       uint32
		WAVE       [4]byte
		FMT        [4]byte
		FMTSize    uint32
		Format     uint16
		Channels   uint16
		SampleRate uint32
		ByteRate   uint32
		BlockAlign uint16
		Bits       uint16
		Data       [4]byte
		DataSize   uint32
	}{
		RIFF: [4]byte{'R', 'I', 'F', 'F'}, Size: uint32(36 + dataBytes),
		WAVE: [4]byte{'W', 'A', 'V', 'E'}, FMT: [4]byte{'f', 'm', 't', ' '},
		FMTSize: 16, Format: 1, Channels: 2, SampleRate: SampleRate,
		ByteRate: SampleRate * 4, BlockAlign: 4, Bits: 16,
		Data: [4]byte{'d', 'a', 't', 'a'}, DataSize: uint32(dataBytes),
	}
	if err := binary.Write(writer, binary.LittleEndian, header); err != nil {
		return err
	}
	return binary.Write(writer, binary.LittleEndian, samples)
}

func writeWAVAtomic(path string, samples []int16) error {
	directory := filepath.Dir(path)
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(directory, ".preview-*.wav")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer func() {
		_ = temporary.Close()
		_ = os.Remove(temporaryPath)
	}()
	if err := temporary.Chmod(0o600); err != nil {
		return err
	}
	if err := writeWAV(temporary, samples); err != nil {
		return err
	}
	if err := temporary.Sync(); err != nil {
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	if err := os.Rename(temporaryPath, path); err == nil {
		return nil
	}
	// Windows does not replace an existing file. Cache entries are disposable,
	// so removing the old complete entry before retrying is safe.
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return err
	}
	return os.Rename(temporaryPath, path)
}

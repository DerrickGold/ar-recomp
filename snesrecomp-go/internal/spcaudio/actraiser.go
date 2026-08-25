package spcaudio

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"regexp"
	"time"
)

const (
	actRaiserROMSize   = 1 << 20
	actRaiserCRC32     = 0xeac3358d
	bootImageSource    = 0x029acd
	commonImageSource  = 0x06ac00
	brrPoolSource      = 0x088000
	bootEntry          = 0x0400
	bootIdle0          = 0x0460
	bootIdle1          = 0x0462
	bootCycleLimit     = 131072
	previewCacheFormat = 1
)

// Track identifies one uploaded ActRaiser song image and the command used to
// start it. The current ROM song banks use command 1; the explicit field keeps
// the renderer ready for an identified multi-song image later.
type Track struct {
	ID     string
	Name   string
	Source uint32
	Song   byte
}

type Progress struct {
	Completed int
	Total     int
	Track     Track
	Reused    bool
}

var safeTrackID = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{0,63}$`)

func loROMOffset(address uint32) (int, error) {
	bank := byte(address >> 16)
	word := uint16(address)
	if word < 0x8000 {
		return 0, fmt.Errorf("$%02X:%04X is not a LoROM ROM address", bank, word)
	}
	return int(bank&0x7f)*0x8000 + int(word&0x7fff), nil
}

func normalizeActRaiserROM(content []byte) ([]byte, error) {
	if len(content) == actRaiserROMSize+512 {
		content = content[512:]
	}
	if len(content) != actRaiserROMSize {
		return nil, fmt.Errorf("ActRaiser ROM must be %d bytes (or %d with a copier header); got %d",
			actRaiserROMSize, actRaiserROMSize+512, len(content))
	}
	if checksum := crc32.ChecksumIEEE(content); checksum != actRaiserCRC32 {
		return nil, fmt.Errorf("ROM CRC32 is %08X; expected the US ActRaiser ROM (%08X)",
			checksum, actRaiserCRC32)
	}
	return content, nil
}

func loadActRaiserROM(path string) ([]byte, string, error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, "", fmt.Errorf("inspect ROM: %w", err)
	}
	if !info.Mode().IsRegular() {
		return nil, "", fmt.Errorf("ROM is not a regular file")
	}
	if info.Size() > actRaiserROMSize+512 {
		return nil, "", fmt.Errorf("ROM is larger than an ActRaiser cartridge image")
	}
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, "", fmt.Errorf("read ROM: %w", err)
	}
	content, err = normalizeActRaiserROM(content)
	if err != nil {
		return nil, "", err
	}
	digest := sha256.Sum256(content)
	return content, hex.EncodeToString(digest[:12]), nil
}

// ROMFingerprint validates the ROM and returns a stable cache key without
// exposing or retaining any of its bytes.
func ROMFingerprint(path string) (string, error) {
	_, fingerprint, err := loadActRaiserROM(path)
	return fingerprint, err
}

type imageResult struct {
	finalWord uint16
	nextBRR   uint16
}

func applyImage(a *apu, rom []byte, source uint32, streamBRR bool,
	brrDestination uint16) (imageResult, error) {
	offset, err := loROMOffset(source)
	if err != nil {
		return imageResult{}, err
	}
	position := offset
	blocks := 0
	var final uint16
	for {
		if position+4 > len(rom) {
			return imageResult{}, fmt.Errorf("SPC image $%06X runs past the ROM", source)
		}
		length := int(rom[position]) | int(rom[position+1])<<8
		target := uint16(rom[position+2]) | uint16(rom[position+3])<<8
		position += 4
		if length == 0 {
			final = target
			break
		}
		if position+length > len(rom) {
			return imageResult{}, fmt.Errorf("SPC image $%06X has a truncated block", source)
		}
		if length > 65536 {
			return imageResult{}, fmt.Errorf("SPC image $%06X has an invalid block length", source)
		}
		for index := 0; index < length; index++ {
			a.ram[uint16(uint32(target)+uint32(index))] = rom[position+index]
		}
		position += length
		blocks++
		if blocks > 512 {
			return imageResult{}, fmt.Errorf("SPC image $%06X has too many blocks", source)
		}
	}

	next := brrDestination
	if !streamBRR || final&0xff == 0 {
		return imageResult{finalWord: final, nextBRR: next}, nil
	}
	// The terminator target doubles as the stage-two script: low byte count,
	// high byte first chunk index, followed by the remaining indices.
	script := position - 1
	pool, err := loROMOffset(brrPoolSource)
	if err != nil {
		return imageResult{}, err
	}
	for segment := 0; segment < int(final&0xff); segment++ {
		if script+segment >= len(rom) {
			return imageResult{}, fmt.Errorf("SPC image $%06X has a truncated BRR script", source)
		}
		chunk := int(rom[script+segment])
		chunkPosition := pool
		for index := 0; index < chunk; index++ {
			if chunkPosition+2 > len(rom) {
				return imageResult{}, fmt.Errorf("BRR chunk %d is outside the ROM", chunk)
			}
			length := int(rom[chunkPosition]) | int(rom[chunkPosition+1])<<8
			chunkPosition += 2 + length
		}
		if chunkPosition+2 > len(rom) {
			return imageResult{}, fmt.Errorf("BRR chunk %d is outside the ROM", chunk)
		}
		length := int(rom[chunkPosition]) | int(rom[chunkPosition+1])<<8
		chunkPosition += 2
		if chunkPosition+length > len(rom) {
			return imageResult{}, fmt.Errorf("BRR chunk %d is truncated", chunk)
		}
		for index := 0; index < length; index++ {
			a.ram[uint16(uint32(next)+uint32(index))] = rom[chunkPosition+index]
		}
		next = uint16(uint32(next) + uint32(length))
	}
	return imageResult{finalWord: final, nextBRR: next}, nil
}

func prepareBaseAPU(rom []byte) (*apu, uint16, error) {
	a := newAPU()
	boot, err := applyImage(a, rom, bootImageSource, false, 0)
	if err != nil {
		return nil, 0, fmt.Errorf("load SPC bootstrap: %w", err)
	}
	if boot.finalWord != bootEntry {
		return nil, 0, fmt.Errorf("unexpected SPC bootstrap entry $%04X", boot.finalWord)
	}
	a.cpu.pc = boot.finalWord
	var discard []int16
	for elapsed := 0; elapsed < bootCycleLimit && a.cpu.pc != bootIdle0 && a.cpu.pc != bootIdle1; {
		before := a.cpu.cycles
		if err := a.step(&discard); err != nil {
			return nil, 0, fmt.Errorf("run SPC bootstrap: %w", err)
		}
		elapsed += int(a.cpu.cycles - before)
		discard = discard[:0]
	}
	if a.cpu.pc != bootIdle0 && a.cpu.pc != bootIdle1 {
		return nil, 0, fmt.Errorf("SPC bootstrap did not reach its idle loop (PC=$%04X)", a.cpu.pc)
	}
	common, err := applyImage(a, rom, commonImageSource, true, 0x3000)
	if err != nil {
		return nil, 0, fmt.Errorf("load common SPC bank: %w", err)
	}
	return a, common.nextBRR, nil
}

func renderTrack(ctx context.Context, base *apu, nextBRR uint16, rom []byte,
	track Track, duration time.Duration) ([]int16, error) {
	if duration <= 0 || duration > 10*time.Minute {
		return nil, fmt.Errorf("preview duration must be between zero and 10 minutes")
	}
	a := base.clone()
	if _, err := applyImage(a, rom, track.Source, true, nextBRR); err != nil {
		return nil, fmt.Errorf("load %s song image: %w", track.Name, err)
	}
	// The retail boot sequence plays command 1 after the common and selected
	// song images have been installed. Port writes remain authentic APU input;
	// only the 65816-side upload handshake is replaced by the direct loader.
	a.inPorts[0] = track.Song
	frames := int(duration.Seconds() * SampleRate)
	result := make([]int16, 0, frames*2)
	for len(result) < frames*2 {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		default:
		}
		remaining := frames - len(result)/2
		chunk := 4096
		if remaining < chunk {
			chunk = remaining
		}
		samples, err := a.renderFrames(chunk)
		if err != nil {
			return nil, fmt.Errorf("render %s: %w", track.Name, err)
		}
		result = append(result, samples...)
	}
	// A short output-only fade avoids a click at the artificial preview cut.
	fadeFrames := SampleRate / 4
	if fadeFrames > frames {
		fadeFrames = frames
	}
	for index := 0; index < fadeFrames; index++ {
		gain := fadeFrames - index
		frame := frames - fadeFrames + index
		result[frame*2] = int16(int(result[frame*2]) * gain / fadeFrames)
		result[frame*2+1] = int16(int(result[frame*2+1]) * gain / fadeFrames)
	}
	return result, nil
}

// RenderActRaiserPreviews validates a local ROM and renders one stereo WAV per
// requested upload identity. Existing valid cache files are reused. Output is
// extracted copyrighted game content and must remain local to the ROM owner.
func RenderActRaiserPreviews(ctx context.Context, romPath, outputDirectory string,
	tracks []Track, duration time.Duration, progress func(Progress)) (map[string]string, string, error) {
	rom, fingerprint, err := loadActRaiserROM(romPath)
	if err != nil {
		return nil, "", err
	}
	if len(tracks) == 0 {
		return nil, fingerprint, errors.New("no audio tracks were requested")
	}
	for _, track := range tracks {
		if !safeTrackID.MatchString(track.ID) {
			return nil, fingerprint, fmt.Errorf("unsafe track id %q", track.ID)
		}
		if track.Source == 0 {
			return nil, fingerprint, fmt.Errorf("track %s has no ROM source", track.Name)
		}
	}
	// Include both the requested duration and an explicit renderer-format
	// generation. A WAV from an older approximation or a shorter CLI smoke test
	// must never masquerade as the current GUI's 30-second preview.
	cacheVariant := fmt.Sprintf("v%d-%dms", previewCacheFormat, duration.Milliseconds())
	cacheDirectory := filepath.Join(outputDirectory, fingerprint, cacheVariant)
	if err := os.MkdirAll(cacheDirectory, 0o700); err != nil {
		return nil, fingerprint, fmt.Errorf("create audio preview cache: %w", err)
	}
	base, nextBRR, err := prepareBaseAPU(rom)
	if err != nil {
		return nil, fingerprint, err
	}
	paths := make(map[string]string, len(tracks))
	for index, track := range tracks {
		select {
		case <-ctx.Done():
			return nil, fingerprint, ctx.Err()
		default:
		}
		path := filepath.Join(cacheDirectory, track.ID+".wav")
		reused := validWAVFile(path)
		if !reused {
			samples, renderErr := renderTrack(ctx, base, nextBRR, rom, track, duration)
			if renderErr != nil {
				return nil, fingerprint, renderErr
			}
			if writeErr := writeWAVAtomic(path, samples); writeErr != nil {
				return nil, fingerprint, fmt.Errorf("write %s preview: %w", track.Name, writeErr)
			}
		}
		paths[track.ID] = path
		if progress != nil {
			progress(Progress{Completed: index + 1, Total: len(tracks), Track: track, Reused: reused})
		}
	}
	return paths, fingerprint, nil
}

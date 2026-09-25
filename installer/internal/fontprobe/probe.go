package fontprobe

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/subprocess"
)

// Callers can distinguish a worker failure from invalid pack contents without
// parsing diagnostic text. Wrapped OS/context errors remain available too.
var (
	ErrProtocol    = errors.New("invalid game font-check response")
	ErrUnavailable = errors.New("game font checker is unavailable")
	ErrFailed      = errors.New("game font check failed")
)

// Do not buffer unlimited child output, even if an incompatible executable
// emits logs instead of the protocol. A write error ends its pipe transfer.
type fontProbeOutput struct {
	bytes.Buffer
	remaining int
}

func (w *fontProbeOutput) Write(data []byte) (int, error) {
	if len(data) > w.remaining {
		return 0, fmt.Errorf("%w: output exceeds its protocol limit", ErrProtocol)
	}
	w.remaining -= len(data)
	return w.Buffer.Write(data)
}

// Run checks immutable font snapshots through an explicitly trusted game executable.
func Run(ctx context.Context, binary, builtin string, fonts []lk.FontCoverageSource, scalars []rune) (lk.FontCoverageProbeResult, error) {
	result := lk.FontCoverageProbeResult{}
	if !filepath.IsAbs(binary) || !filepath.IsAbs(builtin) || len(fonts) == 0 || len(fonts) > 9 || len(scalars) > lk.MaxFontCoverageScalars {
		return result, fmt.Errorf("invalid font-backend request")
	}
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	dir, err := os.MkdirTemp("", "actraiser-font-check-")
	if err != nil {
		return result, err
	}
	defer os.RemoveAll(dir) // Exact private directory created by this request.
	args := []string{"--font-coverage-v1"}
	for i, font := range fonts {
		if err := ctx.Err(); err != nil {
			return result, err
		}
		var input io.ReadCloser
		size := font.Size
		if font.Reference == "builtin:actraiser-sans" {
			f, err := os.Open(builtin)
			if err != nil {
				return result, fmt.Errorf("bundled font unavailable: %w", err)
			}
			stat, err := f.Stat()
			if err != nil {
				f.Close()
				return result, fmt.Errorf("bundled font unavailable: %w", err)
			}
			if !stat.Mode().IsRegular() {
				f.Close()
				return result, fmt.Errorf("bundled font is not a readable regular file")
			}
			input, size = f, stat.Size()
		} else if strings.HasPrefix(font.Reference, "builtin:") || font.Open == nil {
			return result, fmt.Errorf("unsupported font dependency %q", font.Reference)
		} else {
			input = font.Open()
		}
		if size <= 0 || size > lk.MaxPackFontBytes || input == nil {
			if input != nil {
				input.Close()
			}
			return result, fmt.Errorf("font %q is empty or exceeds its size limit", font.Reference)
		}
		// Numbered private filenames prevent path escapes, and copying built-ins
		// too binds the reported digest to the exact bytes the runtime opens.
		path := filepath.Join(dir, fmt.Sprintf("font-%d.bin", i))
		target, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			input.Close()
			return result, err
		}
		digest := sha256.New()
		copied, copyErr := io.Copy(io.MultiWriter(target, digest), io.LimitReader(input, size+1))
		inputErr, closeErr := input.Close(), target.Close()
		if err := errors.Join(copyErr, inputErr, closeErr); err != nil {
			return result, fmt.Errorf("could not snapshot font dependency %q: %w", font.Reference, err)
		}
		if copied != size {
			return result, fmt.Errorf("could not snapshot font dependency %q", font.Reference)
		}
		result.Fonts = append(result.Fonts, lk.FontCoverageIdentity{Reference: font.Reference, SHA256: fmt.Sprintf("%x", digest.Sum(nil))})
		args = append(args, path)
	}
	var query strings.Builder
	for _, scalar := range scalars {
		if scalar < 0 || scalar > 0x10ffff || scalar >= 0xd800 && scalar <= 0xdfff {
			return result, fmt.Errorf("invalid Unicode scalar in font request")
		}
		fmt.Fprintf(&query, "%04X\n", scalar)
	}
	command := exec.CommandContext(ctx, binary, args...)
	subprocess.Configure(command)
	command.Dir = dir
	command.Stdin = strings.NewReader(query.String())
	stdout := &fontProbeOutput{remaining: len(scalars)*12 + 32}
	stderr := &fontProbeOutput{remaining: 16 << 10}
	command.Stdout, command.Stderr = stdout, stderr
	command.WaitDelay = 2 * time.Second
	if err := command.Run(); err != nil {
		if ctx.Err() != nil {
			return result, fmt.Errorf("font check cancelled or timed out: %w", ctx.Err())
		}
		if errors.Is(err, ErrProtocol) {
			return result, err
		}
		var startError *os.PathError
		if errors.As(err, &startError) {
			return result, fmt.Errorf("%w: %w", ErrUnavailable, err)
		}
		return result, fmt.Errorf("%w: %s: %w", ErrFailed, strings.TrimSpace(stderr.String()), err)
	}
	result.Provided, err = parseResponse(stdout.String(), scalars)
	return result, err
}

func parseResponse(output string, scalars []rune) ([]bool, error) {
	lines := strings.SplitAfter(output, "\n")
	if lines[len(lines)-1] == "" {
		lines = lines[:len(lines)-1]
	}
	if len(lines) != len(scalars) {
		return nil, fmt.Errorf("%w: expected %d records, received %d", ErrProtocol, len(scalars), len(lines))
	}
	provided := make([]bool, 0, len(scalars))
	for i, line := range lines {
		// Windows text-mode stdout emits CRLF. Remove only the line ending;
		// spaces, extra fields and stray carriage returns remain invalid.
		if strings.HasSuffix(line, "\n") {
			line = strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r")
		}
		hex, covered, found := strings.Cut(line, "\t")
		value, err := strconv.ParseUint(hex, 16, 32)
		if !found || err != nil || rune(value) != scalars[i] || (covered != "0" && covered != "1") {
			return nil, fmt.Errorf("%w: record %d for U+%04X contains %q", ErrProtocol, i+1, scalars[i], line[:min(len(line), 80)])
		}
		provided = append(provided, covered == "1")
	}
	return provided, nil
}

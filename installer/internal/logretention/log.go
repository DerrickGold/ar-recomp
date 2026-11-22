// Package logretention owns the temporary logs created by desktop launchers.
package logretention

import (
	"errors"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	keepLogs  = 8
	byteLimit = 128 * 1024 * 1024
	lifetime  = 7 * 24 * time.Hour
)

var errBusy = errors.New("log rotation is busy")

// Create allocates an ordinary file suitable for passing directly to a
// detached child. The inherited file handle protects it until the child exits.
func Create(directory, prefix string) (*os.File, error) {
	if err := os.MkdirAll(directory, 0700); err != nil {
		return nil, err
	}
	guard, err := lockRotation(filepath.Join(directory, ".ar-log-rotation.lock"))
	if err != nil {
		return nil, err
	}
	defer guard.Close()

	managed := prefix + "run-"
	prune(directory, managed, keepLogs-1, byteLimit, time.Now().Add(-lifetime))
	log, err := os.CreateTemp(directory, managed+"*.log")
	if err != nil {
		return nil, err
	}
	if err := holdOpen(log); err != nil {
		log.Close()
		os.Remove(log.Name())
		return nil, err
	}
	return log, nil
}

// CaptureGame uses inherited files so the game can outlive the launcher.
// Unavailable logging leaves Cmd's default, independent standard streams.
func CaptureGame(command *exec.Cmd, directory, prefix string) (*os.File, error) {
	log, err := Create(directory, prefix)
	if err != nil {
		return nil, err
	}
	command.Stdout, command.Stderr = log, log
	command.Env = append(command.Environ(), "AR_MANAGED_LAUNCH_LOG=1")
	return log, nil
}

func prune(directory, prefix string, keep int, budget int64, cutoff time.Time) {
	type entry struct {
		name string
		info fs.FileInfo
	}
	items, err := os.ReadDir(directory)
	if err != nil {
		return
	}
	var logs []entry
	for _, item := range items {
		number, havePrefix := strings.CutPrefix(item.Name(), prefix)
		number, haveSuffix := strings.CutSuffix(number, ".log")
		if !havePrefix || !haveSuffix {
			continue
		}
		if _, err := strconv.ParseUint(number, 10, 64); err != nil {
			continue
		}
		info, err := item.Info()
		if err == nil && info.Mode().IsRegular() {
			logs = append(logs, entry{item.Name(), info})
		}
	}
	sort.Slice(logs, func(i, j int) bool {
		return logs[i].info.ModTime().After(logs[j].info.ModTime())
	})
	for _, log := range logs {
		path := filepath.Join(directory, log.name)
		if log.info.ModTime().Before(cutoff) || keep <= 0 || log.info.Size() > budget {
			if removeInactive(path) {
				continue
			}
		}
		keep--
		budget -= log.info.Size()
	}
}

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/logretention"
)

// Keep a separate log per attempt. Direct file handles (not parent-owned pipes)
// let a detached game continue writing even after the Builder has exited.
func startGameProcess(command *exec.Cmd, outputDir string) (<-chan error, string, error) {
	logDir := filepath.Join(outputDir, "logs")
	log, err := logretention.CaptureGame(command, logDir, "game-")
	name := ""
	if log != nil {
		name = log.Name()
		fmt.Fprintf(log, "Game launch: %s\nExecutable: %s\nWorking directory: %s\n", time.Now().Format(time.RFC3339), command.Path, command.Dir)
	} else {
		fmt.Fprintf(os.Stderr, "Game startup log unavailable: %v\n", err)
	}
	if err := command.Start(); err != nil {
		if log != nil {
			fmt.Fprintf(log, "Could not start game: %v\n", err)
			log.Close()
			return nil, "", fmt.Errorf("could not start the game: %w. Details: %s", err, name)
		}
		return nil, "", fmt.Errorf("could not start the game: %w", err)
	}
	done := make(chan error, 1)
	go func() {
		err := command.Wait()
		if log != nil {
			fmt.Fprintf(log, "\nGame exited: %v\n", err)
			log.Close()
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "Game exited unexpectedly: %v\n", gameStartupError(err, name))
		}
		done <- err
	}()
	return done, name, nil
}

func gameStartupError(err error, name string) error {
	if name == "" {
		return err
	}
	return fmt.Errorf("%w. Share this log for help: %s", err, name)
}

func startLoggedGame(command *exec.Cmd, outputDir string) error {
	done, name, err := startGameProcess(command, outputDir)
	if err != nil {
		return err
	}
	// Catch loader/SDL/ROM initialization failures without tying game lifetime
	// to this request. Later failures remain available in the same game log.
	timer := time.NewTimer(2 * time.Second)
	defer timer.Stop()
	select {
	case err := <-done:
		if err != nil {
			return fmt.Errorf("the game stopped during startup: %w", gameStartupError(err, name))
		}
	case <-timer.C:
	}
	return nil
}

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

// Keep a separate log per attempt. Direct file handles (not parent-owned pipes)
// let a detached game continue writing even after the Builder has exited.
func startLoggedGame(command *exec.Cmd, outputDir string) error {
	logDir := filepath.Join(outputDir, "logs")
	if err := os.MkdirAll(logDir, 0700); err != nil {
		return fmt.Errorf("create game log directory: %w", err)
	}
	log, err := os.CreateTemp(logDir, "game-*.log")
	if err != nil {
		return fmt.Errorf("create game startup log: %w", err)
	}
	name := log.Name()
	fmt.Fprintf(log, "Game launch: %s\nExecutable: %s\nWorking directory: %s\n", time.Now().Format(time.RFC3339), command.Path, command.Dir)
	command.Stdout, command.Stderr = log, log
	if err := command.Start(); err != nil {
		fmt.Fprintf(log, "Could not start game: %v\n", err)
		log.Close()
		return fmt.Errorf("could not start the game: %w. Details: %s", err, name)
	}
	done := make(chan error, 1)
	go func() {
		err := command.Wait()
		fmt.Fprintf(log, "\nGame exited: %v\n", err)
		log.Close()
		if err != nil {
			fmt.Fprintf(os.Stderr, "Game exited unexpectedly: %v; see %s\n", err, name)
		}
		done <- err
	}()
	// Catch loader/SDL/ROM initialization failures without tying game lifetime
	// to this request. Later failures remain available in the same game log.
	timer := time.NewTimer(2 * time.Second)
	defer timer.Stop()
	select {
	case err := <-done:
		if err != nil {
			return fmt.Errorf("the game stopped during startup (%v). Share this log for help: %s", err, name)
		}
	case <-timer.C:
	}
	return nil
}

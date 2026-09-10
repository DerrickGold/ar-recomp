package main

import (
	"encoding/json"
	"fmt"
	"io"
	"path/filepath"
	"strings"
	"sync"
)

const (
	eventSchema  = "snesbuild-event"
	eventVersion = 1
)

// machineEvent is the stable, line-delimited process contract for external
// project orchestrators. New optional fields may be added within version 1;
// incompatible changes require a new version.
type machineEvent struct {
	Schema    string `json:"schema"`
	Version   int    `json:"version"`
	Type      string `json:"type"`
	Phase     string `json:"phase,omitempty"`
	Message   string `json:"message,omitempty"`
	Completed int    `json:"completed,omitempty"`
	Total     int    `json:"total,omitempty"`
	Kind      string `json:"kind,omitempty"`
	Path      string `json:"path,omitempty"`
	Severity  string `json:"severity,omitempty"`
}

type eventSink struct {
	mu      sync.Mutex
	encoder *json.Encoder
}

func newEventSink(format string, output io.Writer) (*eventSink, error) {
	switch strings.TrimSpace(format) {
	case "", "human":
		return nil, nil
	case "jsonl":
		return &eventSink{encoder: json.NewEncoder(output)}, nil
	default:
		return nil, fmt.Errorf("unsupported --event-format %q (expected jsonl)", format)
	}
}

func (sink *eventSink) emit(event machineEvent) {
	if sink == nil {
		return
	}
	event.Schema = eventSchema
	event.Version = eventVersion
	sink.mu.Lock()
	_ = sink.encoder.Encode(event)
	sink.mu.Unlock()
}

func (sink *eventSink) phase(id, message string) {
	sink.emit(machineEvent{Type: "phase", Phase: id, Message: message})
}

func (sink *eventSink) progress(id string, completed, total int) {
	sink.emit(machineEvent{Type: "progress", Phase: id, Completed: completed, Total: total})
}

func (sink *eventSink) artifact(kind, path string) {
	if absolute, err := filepath.Abs(path); err == nil {
		path = absolute
	}
	sink.emit(machineEvent{Type: "artifact", Kind: kind, Path: path})
}

func (sink *eventSink) diagnostic(severity, message string) {
	message = strings.TrimSpace(message)
	if message != "" {
		sink.emit(machineEvent{Type: "diagnostic", Severity: severity, Message: message})
	}
}

func (sink *eventSink) fail(phase string, err error) error {
	if err != nil {
		sink.emit(machineEvent{Type: "diagnostic", Phase: phase, Severity: "error", Message: err.Error()})
	}
	return err
}

// eventLogWriter safely transports human/tool diagnostics as JSON strings so
// raw compiler output can never corrupt stdout's one-object-per-line stream.
type eventLogWriter struct {
	sink     *eventSink
	severity string
}

func (writer eventLogWriter) Write(data []byte) (int, error) {
	writer.sink.diagnostic(writer.severity, string(data))
	return len(data), nil
}

func commandWriters(sink *eventSink) (io.Writer, io.Writer) {
	if sink == nil {
		return io.Writer(nil), io.Writer(nil)
	}
	return eventLogWriter{sink: sink, severity: "info"},
		eventLogWriter{sink: sink, severity: "error"}
}

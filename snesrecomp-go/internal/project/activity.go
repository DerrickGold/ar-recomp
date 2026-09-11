package project

import "time"

// buildActivity makes long, healthy compiler/linker operations visibly alive.
// The caller still emits real unit progress; this never invents a percentage.
// Stop waits for the writer so it cannot race the caller's next phase/return.
func buildActivity(log *toolLog, enabled bool, interval time.Duration, status func() string) func() {
	if !enabled {
		return func() {}
	}
	stop, done := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(done)
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for {
			select {
			case <-stop:
				return
			case <-ticker.C:
				log.printf("hermetic: %s\n", status())
			}
		}
	}()
	return func() { close(stop); <-done }
}

package host

import (
	"fmt"
	"io"
	"net/http"
	"os"
)

// BeforeClose follows Wails' convention: true keeps the window open. The
// renderer owns the draft state and calls the normal close endpoint when ready.
func (h *Bridge) BeforeClose(b *Backend) bool {
	if b == nil || b.exited() {
		return false
	}
	h.mu.RLock()
	requests := h.closeRequests
	h.mu.RUnlock()
	if requests != nil {
		select {
		case requests <- struct{}{}:
		default: // Coalesce repeated native close requests.
		}
		return !b.exited()
	}
	// During startup there is no Workshop to confirm with. Still let the
	// backend reject the close if it has already started an automatic build.
	if err := b.RequestClose(); err != nil && !b.exited() {
		fmt.Fprintln(os.Stderr, err)
		return true
	}
	return false
}

func (h *Bridge) serveCloseRequests(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	h.mu.Lock()
	if h.closeRequests == nil {
		// Keep a single pending request across a renderer reconnect.
		h.closeRequests = make(chan struct{}, 1)
	}
	requests := h.closeRequests
	h.mu.Unlock()

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-store")
	stream := http.NewResponseController(w)
	if err := stream.Flush(); err != nil {
		return
	}
	for {
		select {
		case <-r.Context().Done():
			return
		case <-requests:
			if _, err := io.WriteString(w, "event: close-request\ndata: {}\n\n"); err != nil {
				return
			}
			if err := stream.Flush(); err != nil {
				return
			}
		}
	}
}

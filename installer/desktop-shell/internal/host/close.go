package host

import "sync"

// Wails' native Linux/Windows question dialogs return these strings regardless
// of custom button labels. macOS is configured with the same labels by the host.
const (
	CloseAffirmative = "Yes"
	CloseNegative    = "No"
)

// CloseGuard prevents repeated window/menu close requests from opening competing
// native dialogs (Wails Linux uses a single shared dialog-result channel).
type CloseGuard struct {
	mu       sync.Mutex
	pending  bool
	approved bool
}

// BeforeClose follows Wails' convention: true vetoes closing the window.
func (g *CloseGuard) BeforeClose(b *Backend, confirm func() (string, error), report func(error)) (prevent bool) {
	g.mu.Lock()
	if g.approved {
		g.mu.Unlock()
		return false
	}
	if g.pending {
		g.mu.Unlock()
		return true
	}
	g.pending = true
	g.mu.Unlock()
	defer func() {
		g.mu.Lock()
		defer g.mu.Unlock()
		// A quit from the backend-exit watcher may have been suppressed while
		// either dialog was open. Do not lose it when the dialog returns.
		if b != nil && b.exited() {
			prevent = false
		}
		g.approved = !prevent
		g.pending = false
	}()
	if b == nil || b.exited() {
		return false
	}
	answer, err := confirm()
	// The backend-exit watcher may have requested Quit while the dialog was
	// open. Complete that request here instead of losing it to the guard.
	if b.exited() {
		return false
	}
	if err != nil {
		report(err)
		return true
	}
	if answer != CloseAffirmative {
		return true
	}
	if err := b.RequestClose(); err != nil {
		if b.exited() {
			return false
		}
		report(err)
		return true
	}
	return false
}

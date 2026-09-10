package builder

import "path/filepath"

// Both audio previews and decorative scenery use the explicit builder root.
// A packaged builder lives in <bundle>/utils; the installed ROM can also be
// beside the game one level up. Regional/research filenames are never guessed.
// Consumers independently validate the exact supported ROM before decoding.
func findWorkshopROM(root string) string {
	candidates := []string{filepath.Join(root, "user-rom.sfc"), filepath.Join(root, "game.sfc")}
	if parent := filepath.Dir(root); parent != root {
		candidates = append(candidates, filepath.Join(parent, "user-rom.sfc"), filepath.Join(parent, "game.sfc"))
	}
	for _, candidate := range candidates {
		if regularPath(candidate) {
			return candidate
		}
	}
	return ""
}

package buildgui

import (
	"strings"
	"sync"

	"github.com/DerrickGold/snesrecomp-go/internal/uicatalog"
)

// Search captions once per process, not once per route on every keystroke.
// This index contains built-in UI text only, never author drafts or ROM prose.
var navigationSearch = sync.OnceValue(func() map[string]string {
	entries, err := uicatalog.Entries()
	if err != nil {
		return nil
	} // Bootstrap reports invalid catalogs.
	index := make(map[string]string)
	for _, entry := range entries {
		if key, ok := strings.CutPrefix(entry.Key, "builder.navigation."); ok {
			index[key] = strings.Join(entry.Text, " ")
		}
	}
	return index
})

func navigationSearchText(key string) string { return navigationSearch()[key] }

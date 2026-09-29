# Application icon tooling

The original temple and sword artwork is generated for this project, not
extracted from the game ROM. Source images, generation prompts and derived
formats are in `installer/internal/appicons`.

PE resource embedding uses github.com/tc-hib/winres v0.3.1 (0BSD), with
github.com/nfnt/resize (ISC) and golang.org/x/image (BSD-3-Clause).
Their license texts are distributed here. Exact Go versions are recorded in
the installer and desktop-shell go.mod/go.sum files.

package emitter

import (
	"fmt"
	"regexp"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
)

// Generated translation units are self-contained. Each declares exactly the
// generated symbols it uses, with the signature of that symbol's own
// definition, and none of them includes the project's funcs.h: that header is
// the declaration surface for authored game code (sync-funcs), so editing it
// no longer invalidates generated objects. The authored symbols generated
// bodies call -- HLE helpers and predicates -- are declared at block scope by
// those bodies with their actual signatures, and runtime symbols come from the
// public runtime headers every unit includes.

// definitionRE matches a top-level function definition as the emitter writes
// it: linkage, return type, name and parameter list on one line, then '{'.
var definitionRE = regexp.MustCompile(`(?m)^((?:static inline )?(?:RecompReturn|void) ([A-Za-z_][A-Za-z0-9_]*)\([^()]*\)) \{`)

// SymbolTable maps each generated symbol to its exact C declaration.
type SymbolTable struct {
	declarations map[string]string
}

func NewSymbolTable() *SymbolTable {
	return &SymbolTable{declarations: make(map[string]string)}
}

// Declare records name's declaration. A second, different declaration of the
// same symbol is an ABI conflict: it fails rather than silently picking one.
func (table *SymbolTable) Declare(name, declaration string) error {
	if previous, found := table.declarations[name]; found && previous != declaration {
		return fmt.Errorf("generated symbol %s has conflicting declarations %q and %q",
			name, previous, declaration)
	}
	table.declarations[name] = declaration
	return nil
}

// DeclareVariant records one compiled M/X variant body.
func (table *SymbolTable) DeclareVariant(name string) error {
	return table.Declare(name, "RecompReturn "+name+"(CpuState *cpu);")
}

// DeclareBankEntries records the variant every entry defines and the void
// alias of every named entry, exactly as ComposeBank emits them.
func (table *SymbolTable) DeclareBankEntries(bank byte, entries []config.Entry) error {
	for _, entry := range entries {
		if err := table.DeclareVariant(fmt.Sprintf("%s_M%dX%d", entryBaseName(bank, entry),
			entry.EntryMX.M&1, entry.EntryMX.X&1)); err != nil {
			return err
		}
		if entry.Name != "" {
			if err := table.Declare(entry.Name, "void "+entry.Name+"(CpuState *cpu);"); err != nil {
				return err
			}
		}
	}
	return nil
}

// DeclareDefinitions records every top-level function source defines, taking
// each declaration from the definition's own declarator. This is what gives
// shared-region helpers their exact linkage and parameter list.
func (table *SymbolTable) DeclareDefinitions(source string) error {
	for _, match := range definitionRE.FindAllStringSubmatch(source, -1) {
		if err := table.Declare(match[2], match[1]+";"); err != nil {
			return err
		}
	}
	return nil
}

// Declarations returns, one per line, the declaration of every table symbol
// source uses outside comments and literals. A definition's own name is not a
// use, but any other occurrence is, even when source also defines the symbol,
// so a use may precede its definition. The order is fixed: compiled variants,
// void aliases, private helpers, then other external helpers, each sorted by
// name.
func (table *SymbolTable) Declarations(source string) string {
	defining := make(map[int]struct{})
	for _, match := range definitionRE.FindAllStringSubmatchIndex(source, -1) {
		defining[match[4]] = struct{}{}
	}
	used := make(map[string]struct{})
	scanIdentifiers(source, func(offset int, identifier string) {
		if _, definition := defining[offset]; definition {
			return
		}
		if _, found := table.declarations[identifier]; found {
			used[identifier] = struct{}{}
		}
	})
	names := make([]string, 0, len(used))
	for name := range used {
		names = append(names, name)
	}
	sort.Slice(names, func(i, j int) bool {
		left, right := declarationRank(table.declarations[names[i]]), declarationRank(table.declarations[names[j]])
		if left != right {
			return left < right
		}
		return names[i] < names[j]
	})
	var builder strings.Builder
	for _, name := range names {
		builder.WriteString(table.declarations[name])
		builder.WriteByte('\n')
	}
	return builder.String()
}

func declarationRank(declaration string) int {
	switch {
	case strings.HasPrefix(declaration, "RecompReturn ") && strings.HasSuffix(declaration, "(CpuState *cpu);"):
		return 0
	case strings.HasPrefix(declaration, "void "):
		return 1
	case strings.HasPrefix(declaration, "static "):
		return 2
	default:
		return 3
	}
}

// scanIdentifiers reports every C identifier in source, with its byte offset,
// outside comments and string or character literals. Numbers are skipped
// whole, so a suffix such as the "u" of 0x8000u is never taken for an
// identifier.
func scanIdentifiers(source string, report func(offset int, identifier string)) {
	for i := 0; i < len(source); {
		c := source[i]
		switch {
		case c == '/' && i+1 < len(source) && source[i+1] == '/':
			end := strings.IndexByte(source[i:], '\n')
			if end < 0 {
				return
			}
			i += end
		case c == '/' && i+1 < len(source) && source[i+1] == '*':
			end := strings.Index(source[i+2:], "*/")
			if end < 0 {
				return
			}
			i += end + 4
		case c == '"' || c == '\'':
			i = skipLiteral(source, i)
		case isIdentifierStart(c):
			start := i
			for i < len(source) && isIdentifierPart(source[i]) {
				i++
			}
			report(start, source[start:i])
		case c >= '0' && c <= '9':
			for i < len(source) && (isIdentifierPart(source[i]) || source[i] == '.') {
				i++
			}
		default:
			i++
		}
	}
}

// skipLiteral returns the index just past the literal opened at start. An
// unterminated literal ends at its line, as the compiler would diagnose it.
func skipLiteral(source string, start int) int {
	quote := source[start]
	for i := start + 1; i < len(source); i++ {
		switch source[i] {
		case '\\':
			i++
		case quote:
			return i + 1
		case '\n':
			return i
		}
	}
	return len(source)
}

func isIdentifierStart(c byte) bool {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}

func isIdentifierPart(c byte) bool {
	return isIdentifierStart(c) || (c >= '0' && c <= '9')
}

// entryBaseName is the symbol stem of an entry's variants.
func entryBaseName(bank byte, entry config.Entry) string {
	if entry.Name != "" {
		return entry.Name
	}
	return fmt.Sprintf("bank_%02X_%04X", bank, entry.Start)
}

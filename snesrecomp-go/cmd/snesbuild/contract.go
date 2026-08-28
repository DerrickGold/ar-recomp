package main

import (
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
)

// The game project owes the runner two kinds of C symbol, and a missing one is
// otherwise reported only by the linker: hooks named by cfg directives and
// runner-required functions marked in required_symbols.h. Doctor deliberately
// uses the real cfg parser and the installed runtime header so these lists
// cannot drift into a second grammar or hard-coded contract.

var requiredSymbolRE = regexp.MustCompile(
	`\bSR_GAME_PROVIDES\b[^;{}]*?\b([A-Za-z_]\w*)\s*\(`)
var bankContractConfigRE = regexp.MustCompile(`(?i)^bank[0-9a-f]+\.cfg$`)

type contractObligation struct {
	symbol    string
	directive string
	source    string
}

type symbolEvidence uint8

const (
	symbolAbsent symbolEvidence = iota
	symbolMentioned
	symbolDefined
)

func collectHleObligations(configDir string) ([]contractObligation, error) {
	entries, err := os.ReadDir(configDir)
	if err != nil {
		return nil, err
	}
	seen := make(map[string]struct{})
	var obligations []contractObligation
	appendObligation := func(symbol, directive, source string) {
		if symbol == "" {
			return
		}
		key := symbol + "\x00" + source
		if _, duplicate := seen[key]; duplicate {
			return
		}
		seen[key] = struct{}{}
		obligations = append(obligations, contractObligation{
			symbol: symbol, directive: directive, source: source,
		})
	}
	for _, entry := range entries {
		if entry.IsDir() || !bankContractConfigRE.MatchString(entry.Name()) {
			continue
		}
		cfg, loadErr := config.Load(filepath.Join(configDir, entry.Name()))
		if loadErr != nil {
			return nil, loadErr
		}
		for _, symbol := range cfg.HLEFunctions {
			appendObligation(symbol, "hle_func", entry.Name())
		}
		for _, conditional := range cfg.HLEFunctionsIf {
			appendObligation(conditional.Function, "hle_func_if", entry.Name())
			appendObligation(conditional.Predicate, "hle_func_if predicate",
				entry.Name())
		}
		for _, symbol := range cfg.HLEDispatch {
			appendObligation(symbol, "hle_dispatch", entry.Name())
		}
	}
	sort.Slice(obligations, func(i, j int) bool {
		if obligations[i].symbol != obligations[j].symbol {
			return obligations[i].symbol < obligations[j].symbol
		}
		return obligations[i].source < obligations[j].source
	})
	return obligations, nil
}

func collectRequiredRunnerSymbols(headerPath string) ([]string, error) {
	data, err := os.ReadFile(headerPath)
	if err != nil {
		return nil, err
	}
	source := sanitizeCSource(string(data))
	seen := make(map[string]struct{})
	var symbols []string
	for _, match := range requiredSymbolRE.FindAllStringSubmatch(source, -1) {
		if _, duplicate := seen[match[1]]; duplicate {
			continue
		}
		seen[match[1]] = struct{}{}
		symbols = append(symbols, match[1])
	}
	sort.Strings(symbols)
	if len(symbols) == 0 {
		return nil, fmt.Errorf("%s declares no SR_GAME_PROVIDES symbols",
			headerPath)
	}
	return symbols, nil
}

func authoredSourcePaths(root, generatedDir string,
	manifestSources []string) ([]string, error) {
	if len(manifestSources) != 0 {
		paths := make([]string, 0, len(manifestSources))
		for _, source := range manifestSources {
			path := source
			if !filepath.IsAbs(path) {
				path = filepath.Join(root, path)
			}
			path = filepath.Clean(path)
			if !isAuthoredTranslationUnit(path) ||
				generatedDir != "" && pathWithinDir(path, generatedDir) {
				continue
			}
			paths = append(paths, path)
		}
		return paths, nil
	}

	skip := map[string]struct{}{
		"build": {}, ".git": {}, "snesrecomp-go": {}, "release": {},
	}
	var paths []string
	err := filepath.WalkDir(root,
		func(path string, entry fs.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			if entry.IsDir() {
				base := filepath.Base(path)
				if _, skipped := skip[base]; skipped ||
					strings.HasPrefix(base, "build-") {
					return filepath.SkipDir
				}
				if generatedDir != "" && sameDir(path, generatedDir) {
					return filepath.SkipDir
				}
				return nil
			}
			if isAuthoredTranslationUnit(path) {
				paths = append(paths, path)
			}
			return nil
		})
	sort.Strings(paths)
	return paths, err
}

func isAuthoredTranslationUnit(path string) bool {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".c", ".cc", ".cpp", ".cxx", ".m", ".mm":
		return true
	default:
		return false
	}
}

func collectSymbolEvidence(root, generatedDir string, manifestSources,
	symbols []string) (map[string]symbolEvidence, error) {
	paths, err := authoredSourcePaths(root, generatedDir, manifestSources)
	if err != nil {
		return nil, err
	}
	evidence := make(map[string]symbolEvidence, len(symbols))
	for _, path := range paths {
		data, readErr := os.ReadFile(path)
		if readErr != nil {
			return nil, readErr
		}
		source := sanitizeCSource(string(data))
		for _, symbol := range symbols {
			if evidence[symbol] == symbolDefined {
				continue
			}
			candidate := symbolEvidenceInSource(source, symbol)
			if candidate > evidence[symbol] {
				evidence[symbol] = candidate
			}
		}
	}
	return evidence, nil
}

// symbolEvidenceInSource recognizes ordinary C/C++ definitions, including a
// brace on a later line and declaration attributes. If an identifier is
// present but the lightweight parser cannot prove it is a definition, doctor
// reports it as unverified instead of rejecting a potentially macro-authored
// project. The linker remains the signature/definition authority.
func symbolEvidenceInSource(source, symbol string) symbolEvidence {
	mentioned := false
	for offset := 0; offset < len(source); {
		index := strings.Index(source[offset:], symbol)
		if index < 0 {
			break
		}
		index += offset
		offset = index + len(symbol)
		if (index > 0 && isIdentifierByte(source[index-1])) ||
			(offset < len(source) && isIdentifierByte(source[offset])) {
			continue
		}
		mentioned = true
		cursor := skipCWhitespace(source, offset)
		if cursor >= len(source) || source[cursor] != '(' {
			continue
		}
		cursor = skipBalancedParentheses(source, cursor)
		if cursor < 0 {
			continue
		}
		for cursor < len(source) {
			cursor = skipCWhitespace(source, cursor)
			if cursor >= len(source) {
				break
			}
			switch source[cursor] {
			case '{':
				return symbolDefined
			case ';', '=', ',', ')':
				cursor = len(source)
			case '(':
				cursor = skipBalancedParentheses(source, cursor)
				if cursor < 0 {
					cursor = len(source)
				}
			default:
				cursor++
			}
		}
	}
	if mentioned {
		return symbolMentioned
	}
	return symbolAbsent
}

func isIdentifierByte(value byte) bool {
	return value == '_' || value >= '0' && value <= '9' ||
		value >= 'A' && value <= 'Z' || value >= 'a' && value <= 'z'
}

func skipCWhitespace(source string, offset int) int {
	for offset < len(source) {
		switch source[offset] {
		case ' ', '\t', '\r', '\n':
			offset++
		default:
			return offset
		}
	}
	return offset
}

func skipBalancedParentheses(source string, offset int) int {
	if offset >= len(source) || source[offset] != '(' {
		return -1
	}
	depth := 0
	for ; offset < len(source); offset++ {
		switch source[offset] {
		case '(':
			depth++
		case ')':
			depth--
			if depth == 0 {
				return offset + 1
			}
		}
	}
	return -1
}

// sanitizeCSource removes comments, literals, and preprocessor directives
// while preserving byte positions and line breaks needed by the recognizer.
func sanitizeCSource(source string) string {
	result := []byte(source)
	const (
		code = iota
		lineComment
		blockComment
		stringLiteral
		charLiteral
	)
	state := code
	escaped := false
	for index := 0; index < len(result); index++ {
		current := result[index]
		next := byte(0)
		if index+1 < len(result) {
			next = result[index+1]
		}
		switch state {
		case code:
			switch {
			case current == '/' && next == '/':
				result[index], result[index+1] = ' ', ' '
				index++
				state = lineComment
			case current == '/' && next == '*':
				result[index], result[index+1] = ' ', ' '
				index++
				state = blockComment
			case current == '"':
				result[index] = ' '
				state = stringLiteral
				escaped = false
			case current == '\'':
				result[index] = ' '
				state = charLiteral
				escaped = false
			}
		case lineComment:
			if current == '\n' {
				state = code
			} else {
				result[index] = ' '
			}
		case blockComment:
			if current == '*' && next == '/' {
				result[index], result[index+1] = ' ', ' '
				index++
				state = code
			} else if current != '\n' {
				result[index] = ' '
			}
		case stringLiteral, charLiteral:
			if current == '\n' {
				state = code
				escaped = false
				continue
			}
			result[index] = ' '
			if escaped {
				escaped = false
			} else if current == '\\' {
				escaped = true
			} else if state == stringLiteral && current == '"' ||
				state == charLiteral && current == '\'' {
				state = code
			}
		}
	}
	lines := strings.Split(string(result), "\n")
	inDirective := false
	for index, line := range lines {
		trimmed := strings.TrimLeft(line, " \t")
		if inDirective || strings.HasPrefix(trimmed, "#") {
			inDirective = strings.HasSuffix(
				strings.TrimRight(line, " \t\r"), "\\")
			lines[index] = strings.Repeat(" ", len(line))
		}
	}
	return strings.Join(lines, "\n")
}

func sameDir(left, right string) bool {
	leftAbs, leftErr := filepath.Abs(left)
	rightAbs, rightErr := filepath.Abs(right)
	if leftErr != nil || rightErr != nil {
		return false
	}
	return leftAbs == rightAbs
}

func pathWithinDir(path, directory string) bool {
	pathAbs, pathErr := filepath.Abs(path)
	directoryAbs, directoryErr := filepath.Abs(directory)
	if pathErr != nil || directoryErr != nil {
		return false
	}
	relative, err := filepath.Rel(directoryAbs, pathAbs)
	if err != nil {
		return false
	}
	return relative != ".." &&
		!strings.HasPrefix(relative, ".."+string(filepath.Separator))
}

func reportGameContract(output io.Writer, root, configDir, generatedDir,
	requiredHeader string, manifestSources []string) (bool, error) {
	obligations, err := collectHleObligations(configDir)
	if err != nil {
		return false, err
	}
	required, err := collectRequiredRunnerSymbols(requiredHeader)
	if err != nil {
		return false, err
	}
	symbolSet := make(map[string]struct{})
	for _, obligation := range obligations {
		symbolSet[obligation.symbol] = struct{}{}
	}
	for _, symbol := range required {
		symbolSet[symbol] = struct{}{}
	}
	symbols := make([]string, 0, len(symbolSet))
	for symbol := range symbolSet {
		symbols = append(symbols, symbol)
	}
	sort.Strings(symbols)
	evidence, err := collectSymbolEvidence(
		root, generatedDir, manifestSources, symbols)
	if err != nil {
		return false, err
	}

	var missing, unverified []string
	classify := func(symbol, description string) {
		switch evidence[symbol] {
		case symbolDefined:
		case symbolMentioned:
			unverified = append(unverified, symbol+" "+description)
		default:
			missing = append(missing, symbol+" "+description)
		}
	}
	for _, obligation := range obligations {
		classify(obligation.symbol, fmt.Sprintf("(%s in %s)",
			obligation.directive, obligation.source))
	}
	for _, symbol := range required {
		classify(symbol, "(required by the runner)")
	}
	if len(unverified) != 0 {
		fmt.Fprintf(output,
			"%-15s WARNING %d symbol(s) are referenced but their definitions "+
				"could not be proven statically:\n", "game contract", len(unverified))
		for _, item := range unverified {
			fmt.Fprintf(output, "                  %s\n", item)
		}
		fmt.Fprintln(output,
			"                  the native linker will validate these symbols")
	}
	if len(missing) != 0 {
		fmt.Fprintf(output,
			"%-15s MISSING %d symbol(s) absent from authored build sources:\n",
			"game contract", len(missing))
		for _, item := range missing {
			fmt.Fprintf(output, "                  %s\n", item)
		}
		fmt.Fprintln(output,
			"                  see snesrecomp/game/required_symbols.h")
		return true, nil
	}
	if len(unverified) == 0 {
		fmt.Fprintf(output,
			"%-15s ok (%d hle symbol(s), %d required symbol(s))\n",
			"game contract", len(obligations), len(required))
	}
	return false, nil
}

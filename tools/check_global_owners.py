#!/usr/bin/env python3
"""Check that no source under src/ declares another file's global variables.

A global variable is declared once, in the header of the module that defines
it; every reader includes that header. An `extern` for a variable written
into a .c file bypasses that: the compiler cannot check it against the
definition, so a type or size change drifts silently, and nothing records
who reads the global. Declare the variable in its owner's header instead, or
give the owner an accessor.

Function declarations are not covered (hand-written code declares the few
generated bank functions it calls), and neither are generated sources under
src/gen/. `--self-test` proves a variable extern is reported and a function
extern is not.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# POSIX declares environ in no header on some platforms.
ALLOWED = {('src/app/run_dir.c', 'environ')}
EXTERN = re.compile(r'\bextern\b(?!\s*"C")([^;{}]*);')
NAME = re.compile(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$')


def strip_comments_and_strings(text):
    def blank(match):
        return re.sub(r'[^\n]', ' ', match.group(0))
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\''
    return re.sub(pattern, blank, text, flags=re.S)


def variable_externs(text):
    """Returns [(line, name)] for extern declarations of variables."""
    code = strip_comments_and_strings(text)
    found = []
    for match in EXTERN.finditer(code):
        declaration = ' '.join(match.group(1).split())
        if '(' in declaration and not re.search(r'\(\s*\*', declaration):
            continue  # a function; `(*name)` is a function pointer variable
        line = code.count('\n', 0, match.start()) + 1
        for declarator in declaration.split(','):
            declarator = re.sub(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\).*', r'\1', declarator)
            name = NAME.search(declarator.strip())
            found.append((line, name.group(1) if name else declarator.strip()))
    return found


def violations(files):
    """files: {repo-relative path: text}. Returns [(path, line, name)]."""
    found = []
    for path, text in sorted(files.items()):
        for line, name in variable_externs(text):
            if (path, name) not in ALLOWED:
                found.append((path, line, name))
    return found


def tracked_sources():
    listed = subprocess.run(
        ['git', 'ls-files', '--cached', '--others', '--exclude-standard', 'src'],
        cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    files = {}
    for path in listed:
        if path.startswith('src/gen/') or not path.endswith(('.c', '.cpp', '.inc')):
            continue
        if (ROOT / path).is_file():
            files[path] = (ROOT / path).read_text(errors='replace')
    return files


def self_test():
    sample = {
        'src/a.c': 'extern int g_a;\n'
                   'void f(void) { extern uint8_t *g_b[4]; }\n'
                   'extern void (*g_hook)(int);\n'
                   'extern RecompReturn bank_00_8000_M1X1(CpuState *cpu);\n'
                   '/* extern int g_commented; */\n'
                   'static const char *k = "extern int g_quoted;";\n',
        'src/app/run_dir.c': 'extern char **environ;\n',
    }
    found = violations(sample)
    expected = [('src/a.c', 1, 'g_a'), ('src/a.c', 2, 'g_b'), ('src/a.c', 3, 'g_hook')]
    if found != expected:
        print(f'self-test failed: {found}', file=sys.stderr)
        return 1
    print('Global owner self-test: variable externs are reported; functions, '
          'comments, strings and the environ exception are not')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    files = tracked_sources()
    found = violations(files)
    if found:
        print('Source files declare global variables they do not own:', file=sys.stderr)
        for path, line, name in found:
            print(f'  {path}:{line}: extern {name}', file=sys.stderr)
        print('Include the owning module\'s header instead (declare the variable there '
              'if it is not yet declared).', file=sys.stderr)
        return 1
    print(f'Global owners: {len(files)} sources declare no global variables they do not own')
    return 0


if __name__ == '__main__':
    sys.exit(main())

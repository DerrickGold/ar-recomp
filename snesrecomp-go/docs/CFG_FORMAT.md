# Bank configuration format

`v2regen` reads `bankNN.cfg` files from the directory passed with `--cfg-dir`.
`NN` is hexadecimal and must agree with the file's `bank = NN` declaration.
Blank lines and text after `#` are ignored. Numeric addresses are hexadecimal
unless a directive explicitly describes a decimal count.

## Minimal bank

```text
bank = 00
auto_vectors

func ResetHandler 8000 entry_mx:1,1
func NmiHandler   8520
func IrqHandler   8525
```

Each configured bank needs at least one reachable entry point unless entries
will be promoted into it from cross-bank discovery.

## Directives

### `bank = NN`

Declares the 8-bit logical bank. Required once per file.

### `includes = header1.h header2.h`

Adds project headers to the generated bank translation unit. Use this for C
declarations required by HLE hooks or project-specific helpers.

### `auto_vectors`

Seeds entries from the SNES vector table for the configured bank where
applicable.

### `func NAME START [options...]`

Declares a named entry at a 16-bit PC. `NAME` must be a valid C identifier.
Supported options are:

- `end:PPPP`: exclusive decode boundary;
- `entry_mx:M,X`: entry accumulator/index width flags (`0` = 16-bit,
  `1` = 8-bit);
- `exit_mx:M,X`: asserted exit widths;
- `tail_call:PPPP`: known tail-call PC; and
- `entry_s_offset:N`: signed stack-entry adjustment.

Use canonical `0`/`1` M/X values. Compatibility parsing masks integer inputs
for these two options to their low bit.

Example:

```text
func UpdateObject 8123 end:8234 entry_mx:0,1 exit_mx:1,0
```

### `name BBPPPP NAME`

Assigns a C name to a 24-bit address. An in-bank name also promotes the address
to an entry when no `func` already exists there.

### `exclude_range START END`

Excludes a half-open 16-bit address range from executable decoding.

### `data_region BANK START END`

Marks a half-open range as data so discovery does not treat table bytes as
code.

### `exit_mx_at BBPPPP M X`

Pins the exit M/X state for the function at a 24-bit address. Use `0` or `1`;
the compatibility parser masks integer inputs to their low bit.

### `force_variant_at BBPPPP M X`

Forces a specific M/X variant at a verified dispatch/call site. Duplicate
sites are rejected.

### `indirect_dispatch SITE COUNT idx:REG [options...]`

Describes a `JSR (abs,X)`-style table that static analysis cannot safely infer.
`SITE` is a 16-bit PC, `COUNT` is 1..4096, and `REG` is `A`, `X`, or `Y`.
Optional fields are:

- `tables:BASE[,BASE...]`: one to three 16-bit table bases;
- `ret:PPPP`: known continuation PC; and
- `sep:NN`: SEP mask applied by the dispatch shape.

```text
indirect_dispatch B8C0 26 idx:A tables:B8D0 ret:B8C2 sep:20
```

### `rts_dispatch SITE TARGET...`

Declares the targets of a pushed-address/RTS computed dispatch:

```text
rts_dispatch 9000 9010 9020
```

Use `v2regen rts-webs --suggest` to census candidate continuation patterns,
then verify each shape before adding it.

### `hle_func PC C_FUNCTION`

Replaces the function at a 16-bit PC with a project-provided C function.

### `hle_dispatch PC C_FUNCTION`

Routes a dispatch site to a project-provided C function.

### `hle_spc_upload PC`

Marks a verified SPC upload routine for the bundled upload HLE path.

## Compatibility behavior

Unknown and historical v1-only directives are ignored so cfg files can be
migrated incrementally. Malformed values for strict v2 directives fail with a
file and line number. Run `v2regen inspect --cfg-dir recomp` before a full
regen to validate parsing and inspect the worker plan.

Treat every cfg directive as a hardware/ROM claim that needs evidence. A cfg
can make incorrect code compile cleanly; the stub census, opcode differential
tests, runtime M/X checks, and a reference-emulator comparison are the relevant
validation layers.

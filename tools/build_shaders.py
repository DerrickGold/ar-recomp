#!/usr/bin/env python3
"""Compile src/shaders/*.{vert,frag,comp}.glsl into committed C headers.

DEVELOPER TOOL ONLY. This never runs during a build — not the CMake developer
build, and emphatically not `snesbuild build --hermetic`, whose entire premise
is that a bundle user has a pinned Zig, SDL3 and a ROM, and nothing else. The
generated headers are committed to the repo and both build paths simply compile
C, exactly as they already do for every other source file.

Pipeline (all tools are developer-only and never ship with the game):

    GLSL --glslc--> SPIR-V -----------------------------> Vulkan
                      |--spirv-cross--> MSL ------------> Metal
                      `--spirv-cross--> HLSL --dxc------> D3D12

Why GLSL rather than a runtime SDL_shadercross dependency: the build and game
would otherwise need another native library plus its compiler dependencies.
glslc, spirv-cross, and Microsoft's DXC are offline developer tools with
versioned packages; only their deterministic output is committed and shipped.

DXC is Microsoft's DirectX Shader Compiler. Set the ``DXC`` environment
variable when it is not on PATH. It accepts a command prefix, which is useful
when regenerating on a non-Windows host, for example:

    DXC='wine /path/to/dxc.exe' tools/build_shaders.py

Binding convention (SDL_gpu.h, "Shader Resources") — the authored GLSL must
match it or the shader will compile and then silently misbehave:

    vertex stage: set 0 = sampled textures/read-only storage, set 1 = uniforms
    fragment stage: set 2 = sampled textures/read-only storage, set 3 = uniforms
    compute stage: set 0 = sampled/read-only storage resources,
                   set 1 = writable storage resources, set 2 = uniform buffers

Usage:
    tools/build_shaders.py            # regenerate all shaders
    tools/build_shaders.py rim        # regenerate one
    tools/build_shaders.py --check    # verify committed headers are current

Checks compare MSL and SPIR-V byte-for-byte. DXIL may differ only in DXC's
build identifier and its resulting shader hash; the disassembled program,
bindings, signatures and validation requirements must still match exactly.
"""

import argparse
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADER_DIR = REPO_ROOT / "src" / "shaders"

REQUIRED_TOOLS = {
    "glslc": "brew install shaderc   (Debian/Ubuntu: apt install glslc)",
    "spirv-cross": "brew install spirv-cross   (Debian/Ubuntu: apt install spirv-cross)",
    "dxc": "install Microsoft DirectX Shader Compiler and set DXC=/path/to/dxc",
}

TOOL_COMMANDS = {}


def die(message):
    sys.exit("build_shaders: " + message)


def missing_tools():
    missing = []
    for tool, hint in REQUIRED_TOOLS.items():
        override = os.environ.get(tool.upper().replace("-", "_"))
        command = shlex.split(override) if override else [tool]
        if not command or shutil.which(command[0]) is None:
            missing.append(f"  {tool} — {hint}")
        else:
            TOOL_COMMANDS[tool] = command
    return missing


def check_tools():
    missing = missing_tools()
    if missing:
        die(
            "missing required tools:\n"
            + "\n".join(missing)
            + "\n\nThese are needed only to REGENERATE shaders. Building the game\n"
            "(CMake or hermetic) uses the committed headers and needs none."
        )


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        die(
            "command failed: %s\n%s%s"
            % (" ".join(str(c) for c in command), result.stdout, result.stderr)
        )
    return result.stdout


def run_tool(tool, arguments, **kwargs):
    return run(TOOL_COMMANDS[tool] + list(arguments), **kwargs)


def dxc_path(path):
    """Translate host paths when DXC is being run through Wine."""
    command = pathlib.Path(TOOL_COMMANDS["dxc"][0]).name.lower()
    text = str(path)
    if command.startswith("wine") and pathlib.Path(text).is_absolute():
        return "Z:" + text.replace("/", "\\")
    return text


def base_name(source_path):
    """rim.frag.glsl -> rim.  (Path.stem would leave "rim.frag".)"""
    return source_path.name.split(".")[0]


def shader_stage(source_path):
    """sim3d.vert.glsl -> vert."""
    return source_path.name.split(".")[-2]


def c_identifier(base, stage):
    """rim/frag -> RimFrag, sim3d/vert -> Sim3dVert."""
    stem = "".join(
        part.capitalize()
        for part in re.split(r"[^0-9a-zA-Z]+", base)
        if part
    )
    return stem + stage.capitalize()


def byte_array(data, indent="    "):
    lines = []
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append(indent + " ".join("0x%02x," % byte for byte in chunk))
    return "\n".join(lines)


def shader_source(source_path, ancestors=()):
    """Read resource declarations in shared GLSL headers as well as the entry file.

    SDL's Metal buffer remapping needs the full declaration set. Looking only at
    the entry file can assign storage buffer zero over an included uniform block.
    """
    path = source_path.resolve()
    if path in ancestors:
        die(f"{source_path.name}: cyclic GLSL include")
    source = path.read_text()
    return re.sub(r'^\s*#include\s+"([^"\n]+)"\s*$',
        lambda match: shader_source(path.parent / match[1], (*ancestors, path)),
        source, flags=re.MULTILINE)


def compile_shader(source_path, temp_dir):
    """Return (spirv_bytes, msl_text, dxil_bytes) for one shader source."""
    stage = shader_stage(source_path)
    optimized_spv = temp_dir / (source_path.stem + ".opt.spv")
    readable_spv = temp_dir / (source_path.stem + ".spv")
    hlsl_path = temp_dir / (source_path.stem + ".hlsl")
    dxil_path = temp_dir / (source_path.stem + ".dxil")

    # Shipped SPIR-V is optimized: Vulkan consumes these bytes directly.
    run_tool("glslc", [f"-fshader-stage={stage}", "-O", str(source_path),
                       "-o", str(optimized_spv)])
    # MSL is generated from UNoptimized SPIR-V purely so the emitted Metal
    # keeps the authored identifiers. Metal's own compiler optimizes the source
    # at load time, so this costs nothing at runtime and makes the generated
    # shader debuggable when something goes wrong on a real device.
    run_tool("glslc", [f"-fshader-stage={stage}", str(source_path),
                       "-o", str(readable_spv)])

    msl = run_tool("spirv-cross", ["--msl", str(readable_spv)])
    if stage == "comp":
        # SPIRV-Cross puts storage buffers before uniform buffers; SDL compute
        # requires the reverse. Normalize only named entrypoint attributes.
        msl = verify_compute_bindings(source_path, msl, metal=True, remap=True)
    else:
        msl = verify_graphics_buffers(source_path, msl, metal=True, remap=True)
    msl = verify_msl_samplers(source_path, msl, remap=True)
    verify_msl_bindings(source_path, msl)
    hlsl = run_tool("spirv-cross", ["--hlsl", "--shader-model", "60",
                                    str(readable_spv)])
    verify_hlsl_bindings(source_path, hlsl)
    hlsl_path.write_text(hlsl)
    profile = {"frag": "ps_6_0", "vert": "vs_6_0", "comp": "cs_6_0"}[stage]
    run_tool("dxc", ["-T", profile, "-E", "main", "-O3", "-Fo",
                     dxc_path(dxil_path), dxc_path(hlsl_path)])
    # DXC's dump mode parses and validates the finished container. Keep this
    # offline check beside compilation so corrupt or unsigned DXIL can never
    # become a byte array that only fails later on a Windows machine.
    run_tool("dxc", ["-dumpbin", dxc_path(dxil_path)])
    return optimized_spv.read_bytes(), msl, dxil_path.read_bytes()


def verify_msl_samplers(source_path, msl, remap=False):
    """Keep texture AND sampler names on the GLSL/SDL slot.

    SPIRV-Cross may enumerate resources by first use, even when their explicit
    GLSL bindings are reversed. Merely checking that slots 0 and 1 both exist
    cannot detect endpoints swapped by that enumeration.
    """
    source = shader_source(source_path)
    for match in re.finditer(r"layout\s*\(([^)]*)\)\s*uniform\s+sampler\w+\s+(\w+)\s*;", source):
        layout, name = match.groups()
        binding = re.search(r"\bbinding\s*=\s*(\d+)", layout)
        if not binding:
            die(f"{source_path.name}: missing sampler binding for {name}")
        slot = int(binding[1])
        for resource, attribute in ((name, "texture"), (name + "Smplr", "sampler")):
            pattern = rf"(\b{re.escape(resource)}\s*)\[\[{attribute}\(\d+\)\]\]"
            if remap:
                msl, count = re.subn(pattern, rf"\g<1>[[{attribute}({slot})]]", msl)
                if count != 1:
                    die(f"{source_path.name}: ambiguous Metal binding for {resource}")
            if not re.search(rf"\b{re.escape(resource)}\s*\[\[{attribute}\({slot}\)\]\]", msl):
                die(f"{source_path.name}: wrong Metal binding for {resource}")
    return msl


def verify_msl_bindings(source_path, msl):
    """Verify the generated Metal entrypoint and SDL resource convention.

    Named sampler and compute-buffer normalization above resolves SPIRV-Cross
    enumeration differences. Reject missing/ambiguous resources before shipping.
    """
    source = shader_source(source_path)
    verify_msl_samplers(source_path, msl)
    if shader_stage(source_path) == "comp":
        verify_compute_bindings(source_path, msl, metal=True)
        return
    verify_graphics_buffers(source_path, msl, metal=True)
    # Vertex-pulling shaders use gl_VertexIndex and storage buffers rather
    # than vertex attributes, so SPIRV-Cross has no stage_in parameter.
    expected = ["[[stage_in]]"] if re.search(r"layout\s*\([^)]*location[^)]*\)\s*in\b", source) else []
    if "sampler" in source:
        expected.extend(["[[texture(0)]]", "[[sampler(0)]]"])
    if re.search(r"\buniform\s+(?!sampler)", source):
        expected.append("[[buffer(0)]]")
    missing = [token for token in expected if token not in msl]
    if missing:
        die(
            "%s: generated MSL is missing %s.\nspirv-cross may have changed its "
            "Metal resource index assignment; the shader would compile but bind\n"
            "the wrong slots. Inspect the output before shipping it."
            % (source_path.name, ", ".join(missing))
        )
    stage = shader_stage(source_path)
    msl_stage = {"frag": "fragment", "vert": "vertex"}[stage]
    if f"{msl_stage} main0_out main0(" not in msl:
        die(
            "%s: generated MSL entrypoint is not `main0`; SDL_GPUShaderCreateInfo"
            ".entrypoint would need updating to match." % source_path.name
        )


def verify_hlsl_bindings(source_path, hlsl):
    """Pin SDL_GPU's D3D12 register-space and semantic conventions."""
    source = shader_source(source_path)
    if shader_stage(source_path) == "comp":
        verify_compute_bindings(source_path, hlsl, metal=False)
        return
    verify_graphics_buffers(source_path, hlsl, metal=False)
    resource_space = 0 if shader_stage(source_path) == "vert" else 2
    expected = ["TEXCOORD0"]
    if "sampler" in source:
        expected.extend([f"register(t0, space{resource_space})",
                         f"register(s0, space{resource_space})"])
    if re.search(r"\buniform\s+(?!sampler)", source):
        expected.append(f"register(b0, space{resource_space + 1})")
    missing = [token for token in expected if token not in hlsl]
    if missing:
        die(
            "%s: generated HLSL is missing %s.\nspirv-cross may have changed "
            "its D3D12 register or semantic mapping; inspect the output "
            "before shipping it." % (source_path.name, ", ".join(missing))
        )


def verify_graphics_buffers(source_path, compiled, metal, remap=False):
    """Graphics SSBOs follow uniforms in Metal, but textures in SPIR-V/DXIL.

    Normalize named bindings, not whichever resource happens to receive slot
    zero from SPIRV-Cross. Older sampler-only shaders need no remapping.
    """
    source = re.sub(r"//[^\n]*|/\*.*?\*/", "", shader_source(source_path), flags=re.S)
    if not re.search(r"\bbuffer\s+\w+\s*\{", source):
        return compiled
    resource_set = 0 if shader_stage(source_path) == "vert" else 2
    pattern = r"layout\s*\(([^)]*)\)\s*(readonly\s+)?(buffer|uniform)\s+\w+\s*\{[^}]*\}\s*(\w+)\s*;"
    records = []
    for match in re.finditer(pattern, source, re.S):
        layout, readonly, kind, name = match.groups()
        set_match = re.search(r"\bset\s*=\s*(\d+)", layout)
        slot_match = re.search(r"\bbinding\s*=\s*(\d+)", layout)
        if not set_match or not slot_match:
            die(f"{source_path.name}: missing graphics buffer binding")
        set_id, slot = int(set_match[1]), int(slot_match[1])
        if set_id != resource_set + (kind == "uniform") or (kind == "buffer" and not readonly):
            die(f"{source_path.name}: invalid graphics buffer layout")
        records.append((kind, slot, name))
    if sum(k == "buffer" for k, _, _ in records) != len(re.findall(r"\bbuffer\s+\w+\s*\{", source)):
        die(f"{source_path.name}: unsupported graphics storage buffer declaration")
    uniforms = sorted(r for r in records if r[0] == "uniform")
    storage = sorted(r for r in records if r[0] == "buffer")
    samplers = re.findall(r"layout\s*\([^)]*\bbinding\s*=\s*(\d+)[^)]*\)\s*uniform\s+sampler\w+", source)
    if [r[1] for r in uniforms] != list(range(len(uniforms))) or \
            sorted(map(int, samplers)) + [r[1] for r in storage] != list(range(len(samplers) + len(storage))):
        die(f"{source_path.name}: graphics resources must use consecutive SDL bindings")
    for index, (kind, slot, name) in enumerate(uniforms + storage):
        if metal:
            if remap:
                compiled, count = re.subn(rf"(\b{re.escape(name)}\s*)\[\[buffer\(\d+\)\]\]",
                    rf"\g<1>[[buffer({index})]]", compiled)
                if count != 1:
                    die(f"{source_path.name}: ambiguous Metal binding for {name}")
            token = rf"\b{re.escape(name)}\s*\[\[buffer\({index}\)\]\]"
        else:
            prefix, space = ("b", resource_set + 1) if kind == "uniform" else ("t", resource_set)
            token = rf"register\({prefix}{slot}, space{space}\)"
        if not re.search(token, compiled):
            die(f"{source_path.name}: wrong {'Metal' if metal else 'HLSL'} binding for {name}")
    return compiled


def verify_compute_bindings(source_path, compiled, metal, remap=False):
    """Check SDL compute resource namespaces, including Metal's unified buffers.

    This deliberately accepts the small explicit layout vocabulary used by our
    compute sources. Unknown resource declarations fail rather than silently
    selecting a graphics-stage binding rule. Storage/sampler arrays are not yet
    supported by this verifier.
    """
    source = re.sub(r"//[^\n]*|/\*.*?\*/", "", shader_source(source_path), flags=re.S)
    resources = []
    pattern = r"layout\(([^)]*\bset\s*=\s*\d+[^)]*)\)\s*(readonly\s+|writeonly\s+)?(buffer|uniform)\s+(\w+)\s*(\{[^}]*\}\s*\w+|\w+)\s*;"
    for match in re.finditer(pattern, source, re.S):
        layout, qualifier, kind, typename, body = match.groups()
        qualifier = (qualifier or "").strip()
        set_id = int(re.search(r"\bset\s*=\s*(\d+)", layout)[1])
        slot = int(re.search(r"\bbinding\s*=\s*(\d+)", layout)[1])
        if kind == "buffer":
            resource = "read_buffer" if qualifier == "readonly" else "write_buffer"
        elif typename.startswith("sampler"):
            resource = "sampler"
        elif typename.startswith("image"):
            resource = "write_image" if qualifier == "writeonly" else "read_image"
        elif body.startswith("{"):
            resource = "uniform"
        else:
            die(f"{source_path.name}: unsupported compute resource {match[0]}")
        name = body.split()[-1] if body.startswith("{") else body
        required_set = 2 if resource == "uniform" else 1 if resource.startswith("write") else 0
        if set_id != required_set:
            die(f"{source_path.name}: {name} must use compute set {required_set}")
        resources.append((set_id, slot, resource, name))
    if len(resources) != len(re.findall(r"\bset\s*=", source)):
        die(f"{source_path.name}: unsupported compute resource layout")
    for set_id in range(3):
        group = sorted(r for r in resources if r[0] == set_id)
        if [r[1] for r in group] != list(range(len(group))):
            die(f"{source_path.name}: compute set {set_id} must have consecutive bindings")
        rank = {"sampler": 0, "read_image": 1, "read_buffer": 2,
                "write_image": 0, "write_buffer": 1, "uniform": 0}
        if [rank[r[2]] for r in group] != sorted(rank[r[2]] for r in group):
            die(f"{source_path.name}: compute resources violate SDL binding order")
    order = {"uniform": 0, "read_buffer": 1, "write_buffer": 2,
             "sampler": 0, "read_image": 1, "write_image": 2}
    buffers = sorted((r for r in resources if r[2] in ("uniform", "read_buffer", "write_buffer")), key=lambda r: (order[r[2]], r[1]))
    textures = sorted((r for r in resources if r[2] in ("sampler", "read_image", "write_image")), key=lambda r: (order[r[2]], r[1]))
    for resource in resources:
        set_id, slot, kind, name = resource
        if metal:
            table = buffers if kind in ("uniform", "read_buffer", "write_buffer") else textures
            attribute = "buffer" if table is buffers else "texture"
            if remap:
                compiled, count = re.subn(
                    rf"(\b{re.escape(name)}\s*)\[\[{attribute}\(\d+\)\]\]",
                    rf"\g<1>[[{attribute}({table.index(resource)})]]", compiled)
                if count != 1:
                    die(f"{source_path.name}: ambiguous Metal binding for {name}")
            token = rf"\b{re.escape(name)}\s*\[\[{attribute}\({table.index(resource)}\)\]\]"
        else:
            prefix = "b" if kind == "uniform" else "u" if kind.startswith("write") else "t"
            # SPIRV-Cross names HLSL uniform blocks after their GLSL block type.
            token = rf"register\({prefix}{slot}, space{set_id}\)"
        if not re.search(token, compiled):
            die(f"{source_path.name}: wrong {'Metal' if metal else 'HLSL'} binding for {name}")
        if kind == "sampler":
            sampler_token = f"[[sampler({slot})]]" if metal else f"register(s{slot}, space0)"
            if sampler_token not in compiled:
                die(f"{source_path.name}: missing sampler binding for {name}")
    entry = r"kernel\s+void\s+main0\(" if metal else r"\[numthreads\("
    if not re.search(entry, compiled):
        die(f"{source_path.name}: compute entrypoint/thread layout missing")
    return compiled


def render_header(source_path, spirv, msl, dxil):
    base = base_name(source_path)
    stage = shader_stage(source_path)
    name = c_identifier(base, stage)
    guard = "AR_SHADER_%s_%s_H" % (
        re.sub(r"[^0-9A-Z]", "_", base.upper()), stage.upper())
    msl_bytes = msl.encode("utf-8")

    return """/* GENERATED FILE — DO NOT EDIT.
 *
 * Regenerate with:  tools/build_shaders.py %s
 * Source:           src/shaders/%s
 *
 * Committed on purpose: the hermetic build (`snesbuild build --hermetic`)
 * compiles with a pinned `zig cc` and nothing else, so no shader toolchain may
 * be required at build time. See tools/build_shaders.py for the full rationale.
 *
 * MSL is NUL-terminated source text (Metal compiles it at load); SPIR-V and
 * DXIL are binary modules consumed by Vulkan and D3D12. Pass the matching
 * *Size constant to SDL_GPUShaderCreateInfo.code_size — for MSL that EXCLUDES
 * the terminator.
 */
#ifndef %s
#define %s

static const unsigned char k%sMSL[] = {
%s
    0x00
};
static const unsigned int k%sMSLSize = %du;

static const unsigned char k%sSPV[] = {
%s
};
static const unsigned int k%sSPVSize = %du;

static const unsigned char k%sDXIL[] = {
%s
};
static const unsigned int k%sDXILSize = %du;

#endif /* %s */
""" % (
        base,
        source_path.name,
        guard,
        guard,
        name,
        byte_array(msl_bytes),
        name,
        len(msl_bytes),
        name,
        byte_array(spirv),
        name,
        len(spirv),
        name,
        byte_array(dxil),
        name,
        len(dxil),
        guard,
    )


def dxil_program(disassembly):
    """Ignore only DXC's build identity and the hash that includes that identity.

    Resource bindings, signatures, instructions, validation requirements and
    every other metadata record remain part of the comparison.
    """
    return re.sub(r'^; shader hash: [0-9a-f]+\n|^![0-9]+ = !\{!"dxc[^"\n]*"\}\n',
                  '', disassembly, flags=re.M)


def check_header(source, current, spirv, msl, dxil, temp_dir):
    expected = render_header(source, spirv, msl, dxil)
    if current == expected:
        return "OK"
    identifier = "k" + c_identifier(base_name(source), shader_stage(source)) + "DXIL"
    match = re.search(r'const unsigned char ' + re.escape(identifier) +
                      r'\[\] = \{(.*?)\};', current, re.S)
    if not match:
        return "STALE"
    committed_dxil = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-f]{2})', match[1]))
    # Require exact MSL, SPIR-V, size declarations and header structure before
    # considering a DXC identity-only difference. Never rewrite shipped blobs
    # just because a local compiler has a different build identifier.
    if render_header(source, spirv, msl, committed_dxil) != current:
        return "STALE"
    old_path, new_path = temp_dir / "committed.dxil", temp_dir / "regenerated.dxil"
    old_path.write_bytes(committed_dxil)
    new_path.write_bytes(dxil)
    old = run_tool("dxc", ["-dumpbin", dxc_path(old_path)])
    new = run_tool("dxc", ["-dumpbin", dxc_path(new_path)])
    return "OK (DXC build identity differs)" if dxil_program(old) == dxil_program(new) else "STALE"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shaders", nargs="*", help="shader stems (default: all)")
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify committed headers match their sources; do not write",
    )
    parser.add_argument(
        "--skip-if-tools-missing",
        action="store_true",
        help="with --check: print SKIPPED and exit 0 when a required tool is "
        "missing, so an aggregate check can run wherever the tools exist",
    )
    args = parser.parse_args()

    if args.skip_if_tools_missing:
        if not args.check:
            die("--skip-if-tools-missing only applies with --check")
        missing = missing_tools()
        if missing:
            print("shader header check SKIPPED; missing tools:\n"
                  + "\n".join(missing))
            return
    check_tools()

    sources = sorted(SHADER_DIR.glob("*.frag.glsl")) + \
        sorted(SHADER_DIR.glob("*.vert.glsl")) + \
        sorted(SHADER_DIR.glob("*.comp.glsl"))
    if args.shaders:
        wanted = set(args.shaders)
        sources = [s for s in sources if base_name(s) in wanted]
        unknown = wanted - {base_name(s) for s in sources}
        if unknown:
            die("no such shader(s): %s" % ", ".join(sorted(unknown)))
    if not sources:
        die("no shaders found in %s" % SHADER_DIR)

    stale = []
    with tempfile.TemporaryDirectory() as temp:
        temp_dir = pathlib.Path(temp)
        for source in sources:
            spirv, msl, dxil = compile_shader(source, temp_dir)
            header_path = SHADER_DIR / (
                base_name(source) + "_" + shader_stage(source) + ".h")
            contents = render_header(source, spirv, msl, dxil)

            if args.check:
                current = header_path.read_text() if header_path.exists() else ""
                status = check_header(source, current, spirv, msl, dxil, temp_dir)
                if status == "STALE":
                    stale.append(header_path.name)
                print("%-24s %s" % (header_path.name, status))
            else:
                header_path.write_text(contents)
                print(
                    "%-24s %6d B MSL  %6d B SPIR-V  %6d B DXIL"
                    % (header_path.name, len(msl.encode("utf-8")), len(spirv),
                       len(dxil))
                )

    if args.check and stale:
        die(
            "%d header(s) out of date: %s\nRun tools/build_shaders.py to regenerate."
            % (len(stale), ", ".join(stale))
        )


if __name__ == "__main__":
    main()

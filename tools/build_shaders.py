#!/usr/bin/env python3
"""Compile src/shaders/*.{vert,frag}.glsl into committed C headers.

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

    fragment stage: set 2 = sampled textures, set 3 = uniform buffers

Usage:
    tools/build_shaders.py            # regenerate all shaders
    tools/build_shaders.py rim        # regenerate one
    tools/build_shaders.py --check    # verify committed headers are current
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


def check_tools():
    missing = []
    for tool, hint in REQUIRED_TOOLS.items():
        override = os.environ.get(tool.upper().replace("-", "_"))
        command = shlex.split(override) if override else [tool]
        if not command or shutil.which(command[0]) is None:
            missing.append(f"  {tool} — {hint}")
        else:
            TOOL_COMMANDS[tool] = command
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
    verify_msl_bindings(source_path, msl)
    hlsl = run_tool("spirv-cross", ["--hlsl", "--shader-model", "60",
                                    str(readable_spv)])
    verify_hlsl_bindings(source_path, hlsl)
    hlsl_path.write_text(hlsl)
    profile = {"frag": "ps_6_0", "vert": "vs_6_0"}[stage]
    run_tool("dxc", ["-T", profile, "-E", "main", "-O3", "-Fo",
                     dxc_path(dxil_path), dxc_path(hlsl_path)])
    # DXC's dump mode parses and validates the finished container. Keep this
    # offline check beside compilation so corrupt or unsigned DXIL can never
    # become a byte array that only fails later on a Windows machine.
    run_tool("dxc", ["-dumpbin", dxc_path(dxil_path)])
    return optimized_spv.read_bytes(), msl, dxil_path.read_bytes()


def verify_msl_bindings(source_path, msl):
    """Guard the assumption the whole pipeline rests on.

    spirv-cross assigns Metal resource indices itself. It happens to land
    exactly on the slots SDL's render backend binds — [[texture(0)]],
    [[sampler(0)]], [[buffer(0)]] — with no remapping flags, which is what
    makes this pipeline viable at all. If a future spirv-cross ever changes
    that, the shader would still compile and then quietly sample nothing;
    fail loudly here instead.
    """
    source = source_path.read_text()
    expected = ["[[stage_in]]"]
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
    source = source_path.read_text()
    expected = ["TEXCOORD0"]
    if "sampler" in source:
        expected.extend(["register(t0, space2)", "register(s0, space2)"])
    if re.search(r"\buniform\s+(?!sampler)", source):
        expected.append("register(b0, space3)")
    missing = [token for token in expected if token not in hlsl]
    if missing:
        die(
            "%s: generated HLSL is missing %s.\nspirv-cross may have changed "
            "its D3D12 register or semantic mapping; inspect the output "
            "before shipping it." % (source_path.name, ", ".join(missing))
        )


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shaders", nargs="*", help="shader stems (default: all)")
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify committed headers match their sources; do not write",
    )
    args = parser.parse_args()

    check_tools()

    sources = sorted(SHADER_DIR.glob("*.frag.glsl")) + \
        sorted(SHADER_DIR.glob("*.vert.glsl"))
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
                status = "OK" if current == contents else "STALE"
                if current != contents:
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

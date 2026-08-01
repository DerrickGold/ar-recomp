#!/usr/bin/env python3
"""Compile src/shaders/*.frag.glsl into committed C headers.

DEVELOPER TOOL ONLY. This never runs during a build — not the CMake developer
build, and emphatically not `snesbuild build --hermetic`, whose entire premise
is that a bundle user has a pinned Zig, SDL3 and a ROM, and nothing else. The
generated headers are committed to the repo and both build paths simply compile
C, exactly as they already do for every other source file.

Pipeline (both tools are dev-machine only, installable from Homebrew/apt):

    rim.frag.glsl --glslc--> SPIR-V  ------------------> Vulkan  (Linux, Deck, Win)
                         \\--> SPIR-V --spirv-cross--> MSL --> Metal  (macOS)

Why GLSL rather than HLSL + SDL_shadercross: shadercross has no tagged releases
or prebuilt binaries, so pinning it means pinning a git SHA and building a
heavyweight C++ project against DXC — which fights this project's pinned,
sha256-verified toolchain story (internal/toolchain). glslc and spirv-cross
ship versioned, packaged releases that pin the same way Zig and SDL3 already do.

The one format this pipeline cannot emit is DXIL, needed only by Windows
machines with no Vulkan driver. That is a deliberate deferral, not a dead end:
the SPIR-V emitted here is valid SDL_shadercross *input*, so adding DXIL later
is additive and costs none of this work.

Binding convention (SDL_gpu.h, "Shader Resources") — the authored GLSL must
match it or the shader will compile and then silently misbehave:

    fragment stage: set 2 = sampled textures, set 3 = uniform buffers
    vertex interface: location 0 = COLOR0, location 1 = TEXCOORD0

Usage:
    tools/build_shaders.py            # regenerate all shaders
    tools/build_shaders.py rim        # regenerate one
    tools/build_shaders.py --check    # verify committed headers are current
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADER_DIR = REPO_ROOT / "src" / "shaders"

REQUIRED_TOOLS = {
    "glslc": "brew install shaderc   (Debian/Ubuntu: apt install glslc)",
    "spirv-cross": "brew install spirv-cross   (Debian/Ubuntu: apt install spirv-cross)",
}


def die(message):
    sys.exit("build_shaders: " + message)


def check_tools():
    missing = [
        f"  {tool} — {hint}"
        for tool, hint in REQUIRED_TOOLS.items()
        if shutil.which(tool) is None
    ]
    if missing:
        die(
            "missing required tools:\n"
            + "\n".join(missing)
            + "\n\nThese are needed only to REGENERATE shaders. Building the game\n"
            "(CMake or hermetic) uses the committed headers and needs neither."
        )


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        die(
            "command failed: %s\n%s%s"
            % (" ".join(str(c) for c in command), result.stdout, result.stderr)
        )
    return result.stdout


def base_name(source_path):
    """rim.frag.glsl -> rim.  (Path.stem would leave "rim.frag".)"""
    return source_path.name.split(".")[0]


def c_identifier(base):
    """rim -> RimFrag, dof_edge -> DofEdgeFrag."""
    return "".join(part.capitalize() for part in re.split(r"[^0-9a-zA-Z]+", base) if part) + "Frag"


def byte_array(data, indent="    "):
    lines = []
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append(indent + " ".join("0x%02x," % byte for byte in chunk))
    return "\n".join(lines)


def compile_shader(source_path, temp_dir):
    """Return (spirv_bytes, msl_text) for one .frag.glsl source."""
    optimized_spv = temp_dir / (source_path.stem + ".opt.spv")
    readable_spv = temp_dir / (source_path.stem + ".spv")

    # Shipped SPIR-V is optimized: Vulkan consumes these bytes directly.
    run(["glslc", "-fshader-stage=frag", "-O", str(source_path), "-o", str(optimized_spv)])
    # MSL is generated from UNoptimized SPIR-V purely so the emitted Metal
    # keeps the authored identifiers. Metal's own compiler optimizes the source
    # at load time, so this costs nothing at runtime and makes the generated
    # shader debuggable when something goes wrong on a real device.
    run(["glslc", "-fshader-stage=frag", str(source_path), "-o", str(readable_spv)])

    msl = run(["spirv-cross", "--msl", str(readable_spv)])
    verify_msl_bindings(source_path, msl)
    return optimized_spv.read_bytes(), msl


def verify_msl_bindings(source_path, msl):
    """Guard the assumption the whole pipeline rests on.

    spirv-cross assigns Metal resource indices itself. It happens to land
    exactly on the slots SDL's render backend binds — [[texture(0)]],
    [[sampler(0)]], [[buffer(0)]] — with no remapping flags, which is what
    makes this pipeline viable at all. If a future spirv-cross ever changes
    that, the shader would still compile and then quietly sample nothing;
    fail loudly here instead.
    """
    expected = ["[[texture(0)]]", "[[sampler(0)]]", "[[buffer(0)]]", "[[stage_in]]"]
    missing = [token for token in expected if token not in msl]
    if missing:
        die(
            "%s: generated MSL is missing %s.\nspirv-cross may have changed its "
            "Metal resource index assignment; the shader would compile but bind\n"
            "the wrong slots. Inspect the output before shipping it."
            % (source_path.name, ", ".join(missing))
        )
    if "fragment main0_out main0(" not in msl:
        die(
            "%s: generated MSL entrypoint is not `main0`; SDL_GPUShaderCreateInfo"
            ".entrypoint would need updating to match." % source_path.name
        )


def render_header(source_path, spirv, msl):
    base = base_name(source_path)
    name = c_identifier(base)
    guard = "AR_SHADER_%s_FRAG_H" % re.sub(r"[^0-9A-Z]", "_", base.upper())
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
 * MSL is NUL-terminated source text (Metal compiles it at load); SPIR-V is a
 * binary module consumed directly by Vulkan. Pass the matching *Size constant
 * to SDL_GPUShaderCreateInfo.code_size — for MSL that EXCLUDES the terminator.
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

    sources = sorted(SHADER_DIR.glob("*.frag.glsl"))
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
            spirv, msl = compile_shader(source, temp_dir)
            header_path = SHADER_DIR / (base_name(source) + "_frag.h")
            contents = render_header(source, spirv, msl)

            if args.check:
                current = header_path.read_text() if header_path.exists() else ""
                status = "OK" if current == contents else "STALE"
                if current != contents:
                    stale.append(header_path.name)
                print("%-24s %s" % (header_path.name, status))
            else:
                header_path.write_text(contents)
                print(
                    "%-24s %6d B MSL  %6d B SPIR-V"
                    % (header_path.name, len(msl.encode("utf-8")), len(spirv))
                )

    if args.check and stale:
        die(
            "%d header(s) out of date: %s\nRun tools/build_shaders.py to regenerate."
            % (len(stale), ", ".join(stale))
        )


if __name__ == "__main__":
    main()

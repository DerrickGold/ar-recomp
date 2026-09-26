"""Checkpoint manifest loading, path resolution and complete rendering-policy pinning."""

from __future__ import annotations
import json
from pathlib import Path

# Every stage toggle a checkpoint must pin once it pins any of them. A
# checkpoint that names only some stages silently inherits the shipped default
# for the rest, so landing a new stage would change what older checkpoints
# render -- which is exactly what happened when soft shadows landed. Add a new
# stage here and to every manifest env/baseline_env at the same time.
SIM3D_STAGE_ENV = (
    "AR_SIM3D_SEPARATED",
    "AR_SIM3D_GROUND",
    "AR_SIM3D_BILLBOARDS",
    "AR_SIM3D_HEIGHT",
    "AR_SIM3D_SHADOWS",
    "AR_SIM3D_SOFT_SHADOWS",
    "AR_SIM3D_RIM_LIGHT",
    "AR_SIM3D_WORLD_UNDERLAY",
    "AR_SIM3D_CLOUDS",
    "AR_SIM3D_CULL_HAZE_STAGE",
    "AR_SIM3D_BACKDROP",
    "AR_SIM3D_PICKER_EASE",
    "AR_SIM3D_EFFECT_LIGHTING",
    "AR_SIM3D_PARTICLES",
)
# The background voxel town is configured by settings rather than by stage
# toggles, so it needs its own all-or-nothing pinning rule. A voxel checkpoint
# that names only the preset inherits the shipped default for detail, LOD,
# shading, style, facing, render scale and landscape height -- and the preset
# itself rewrites several of them, so an unpinned block does not describe the
# scene it renders.
SIM3D_VOXEL_ENV = (
    "AR_SIM3D_VOXEL_PRESET",
    "AR_SIM3D_VOXEL_DETAIL",
    "AR_SIM3D_VOXEL_LOD",
    "AR_SIM3D_VOXEL_SHADING",
    "AR_SIM3D_VOXEL_STYLE",
    "AR_SIM3D_VOXEL_FACING",
    "AR_SIM3D_VOXEL_RENDER_SCALE",
    "AR_SIM3D_LANDSCAPE_HEIGHT",
)


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "actraiser-sim3d-checkpoints-v1":
        raise ValueError(f"unsupported checkpoint manifest schema in {path}")
    return data


def resolve(root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else root / path


def check_stage_pinning(name: str, checkpoint: dict) -> None:
    """Every env block that pins any SIM 3D stage must pin them all.

    A block that pins only some stages silently inherits the shipped default
    for the rest, so the checkpoint's meaning changes the day a new stage
    lands -- which is exactly how six checkpoints broke at once when soft
    shadows shipped.
    """
    for block_name in ("env", "baseline_env"):
        block = checkpoint.get(block_name) or {}
        pinned = [key for key in SIM3D_STAGE_ENV if key in block]
        if pinned and len(pinned) != len(SIM3D_STAGE_ENV):
            missing = [key for key in SIM3D_STAGE_ENV if key not in block]
            raise ValueError(
                f"checkpoint {name} {block_name} pins "
                f"{len(pinned)} of {len(SIM3D_STAGE_ENV)} SIM 3D stages; "
                f"unpinned stages inherit the shipped default and will change "
                f"under this checkpoint when a stage lands. Missing: "
                f"{', '.join(missing)}"
            )

        voxel_pinned = [key for key in SIM3D_VOXEL_ENV if key in block]
        if voxel_pinned and len(voxel_pinned) != len(SIM3D_VOXEL_ENV):
            missing = [key for key in SIM3D_VOXEL_ENV if key not in block]
            raise ValueError(
                f"checkpoint {name} {block_name} pins "
                f"{len(voxel_pinned)} of {len(SIM3D_VOXEL_ENV)} voxel town "
                f"settings; the preset rewrites the rest, so an unpinned "
                f"block does not describe the scene it renders. Missing: "
                f"{', '.join(missing)}"
            )

        camera_pose = ("AR_SIM3D_PITCH", "AR_SIM3D_YAW", "AR_SIM3D_DISTANCE")
        if any(key in block for key in camera_pose) and "AR_SIM3D_CAMERA_MODE" not in block:
            raise ValueError(
                f"checkpoint {name} {block_name} pins a SIM 3D camera pose "
                f"without AR_SIM3D_CAMERA_MODE; the pose would silently "
                f"change meaning with the shipped camera-mode default"
            )

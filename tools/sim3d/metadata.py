"""Read streamed JSONL metadata and retain line-numbered diagnostics."""

import json
from pathlib import Path

from .metadata_checks import MetadataValidator


def read_metadata(
    path: Path, allowed_unpacked_atlas_objects: list[dict] | None = None
) -> tuple[dict, list[str]]:
    validator = MetadataValidator(allowed_unpacked_atlas_objects)
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            try:
                frame = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: {error}") from error
            validator.observe(frame, line_number)
    return validator.finish()

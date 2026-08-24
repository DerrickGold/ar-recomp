"""Shared formatting helpers for oracle trace tools."""


def format_wram_address(address):
    if address >= 0x10000:
        return f"$7F{address - 0x10000:04X}"
    return f"$7E{address:04X}"

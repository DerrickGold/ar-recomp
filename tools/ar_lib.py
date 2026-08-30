"""ar_lib.py — shared primitives for the ActRaiser debug toolkit.

Used by game-specific inspection and asset tools such as the ActRaiser content
catalogs. One canonical place for:
  - ROM loading + LoROM address mapping
  - small endian readers and file hashes
  - dependency-free RGB PNG output
"""
import hashlib
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))

# ── loading ──────────────────────────────────────────────────────────────────

_rom_cache = [None]

def load_rom(path=None):
    """ROM bytes (cached). AR_ROM env overrides; default <repo>/ar.sfc."""
    if path:
        return open(path, 'rb').read()
    if _rom_cache[0] is None:
        for c in (os.environ.get('AR_ROM'), os.path.join(ROOT, 'ar.sfc'), 'ar.sfc'):
            if c and os.path.exists(c):
                _rom_cache[0] = open(c, 'rb').read()
                break
        else:
            raise SystemExit("ar_lib: ROM not found (set AR_ROM or place ar.sfc at repo root)")
    return _rom_cache[0]


def lorom_off(pc24):
    """pc24 -> ROM file offset (LoROM, headerless). None if not ROM-mapped."""
    bank, addr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
    if addr < 0x8000:
        return None
    return (bank & 0x7F) * 0x8000 + (addr - 0x8000)


def lorom_offset(bank, address):
    """Bank/address LoROM mapping for tools that keep the fields separate."""
    offset = lorom_off(((bank & 0xFF) << 16) | address)
    if offset is None:
        raise ValueError(f"not ROM-mapped: ${bank:02X}:{address:04X}")
    return offset


def read_le16(data, offset=0):
    return data[offset] | (data[offset + 1] << 8)


def file_sha256(path, chunk_size=1024 * 1024):
    """Hex SHA-256 of a file, streamed in bounded chunks."""
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for chunk in iter(lambda: source.read(chunk_size), b''):
            digest.update(chunk)
    return digest.hexdigest()


def write_rgb_png(path, width, height, rgb):
    """Write packed RGB bytes as a dependency-free PNG."""
    def chunk(tag, payload):
        content = tag + payload
        return (struct.pack('>I', len(payload)) + content +
                struct.pack('>I', zlib.crc32(content) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * width * 3:(y + 1) * width * 3])
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack(
        '>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as output:
        output.write(png)
